#include <drivers/usb/usb_core.h>

#include <kernel/workqueue.h>
#include <kernel/proc.h>

#include <hal/lock.h>

#include <mm/heap.h>

#include <lib/string.h>
#include <lib/dlist.h>

#include <stdint.h>

#define USB_MAX_HCDS 8u
#define USB_MAX_CLASS_DRIVERS 16u

extern volatile uint32_t timer_ticks;

typedef struct {
    spinlock_t lock;
    dlist_head_t list;
} usb_intr_wrap_list_t;

typedef struct {
    usb_intr_pipe_t* pipe;
    usb_device_t* dev;

    usb_intr_cb_t cb;
    void* cb_ctx;

    uint32_t refs_;
    int closing;

    dlist_head_t node;
} usb_intr_wrap_t;

static usb_intr_wrap_list_t g_usb_intr_wraps = {
    .lock = {0},
    .list = {0},
};

static void usb_intr_wrap_init_once(void) {
    static int inited = 0;
    if (inited) {
        return;
    }

    spinlock_init(&g_usb_intr_wraps.lock);
    dlist_init(&g_usb_intr_wraps.list);

    inited = 1;
}

static void usb_intr_wrap_get(usb_intr_wrap_t* w) {
    if (!w) {
        return;
    }

    __atomic_fetch_add(&w->refs_, 1u, __ATOMIC_RELAXED);
}

static void usb_intr_wrap_put(usb_intr_wrap_t* w) {
    if (!w) {
        return;
    }

    const uint32_t old = __atomic_fetch_sub(&w->refs_, 1u, __ATOMIC_ACQ_REL);
    if (old != 1u) {
        return;
    }

    if (w->dev) {
        usb_device_put(w->dev);
        w->dev = 0;
    }

    kfree(w);
}

static void usb_intr_trampoline(void* ctx, const uint8_t* data, uint32_t len) {
    usb_intr_wrap_t* w = (usb_intr_wrap_t*)ctx;
    if (!w) {
        return;
    }

    usb_intr_wrap_get(w);

    if (!__atomic_load_n(&w->closing, __ATOMIC_ACQUIRE) && w->cb) {
        w->cb(w->cb_ctx, data, len);
    }

    usb_intr_wrap_put(w);
}

struct usb_device {
    usb_hcd_t* hcd;

    usb_device_info_t info;

    uint32_t refs_;

    uint8_t root_port;

    usb_device_t* parent;
    uint8_t hub_port;

    uint8_t* cfg_buf;
    uint16_t cfg_len;

    void* class_private;

    const usb_class_driver_t* bound_driver;

    dlist_head_t node;
};

typedef struct {
    usb_hcd_t* hcd;

    workqueue_t* wq;
    work_struct_t enum_work;

    task_t* monitor_task;

    uint32_t addr_bitmap[4];

    uint8_t root_connected[16];
    uint8_t root_port_count;

    spinlock_t lock;

    dlist_head_t dev_list;
} usb_bus_t;

static usb_bus_t g_usb_buses[USB_MAX_HCDS];
static spinlock_t g_usb_buses_lock;

static const usb_class_driver_t* g_class_drivers[USB_MAX_CLASS_DRIVERS];
static uint32_t g_class_driver_count;
static spinlock_t g_class_driver_lock;

static void usb_enum_work(work_struct_t* work);

static usb_bus_t* usb_bus_find(usb_hcd_t* hcd) {
    if (!hcd) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&g_usb_buses_lock);

    for (uint32_t i = 0; i < USB_MAX_HCDS; i++) {
        if (g_usb_buses[i].hcd == hcd) {
            spinlock_release_safe(&g_usb_buses_lock, flags);
            return &g_usb_buses[i];
        }
    }

    spinlock_release_safe(&g_usb_buses_lock, flags);
    return 0;
}

static void usb_addr_set(usb_bus_t* bus, uint8_t addr) {
    if (!bus || addr == 0 || addr >= 128) {
        return;
    }

    const uint32_t idx = (uint32_t)addr / 32u;
    const uint32_t bit = (uint32_t)addr % 32u;

    bus->addr_bitmap[idx] |= (1u << bit);
}

static void usb_addr_clear(usb_bus_t* bus, uint8_t addr) {
    if (!bus || addr == 0 || addr >= 128) {
        return;
    }

    const uint32_t idx = (uint32_t)addr / 32u;
    const uint32_t bit = (uint32_t)addr % 32u;

    bus->addr_bitmap[idx] &= ~(1u << bit);
}

static uint8_t usb_addr_alloc(usb_bus_t* bus) {
    if (!bus) {
        return 0;
    }

    for (uint32_t addr = 1; addr < 127; addr++) {
        const uint32_t idx = addr / 32u;
        const uint32_t bit = addr % 32u;

        if ((bus->addr_bitmap[idx] & (1u << bit)) == 0) {
            usb_addr_set(bus, (uint8_t)addr);
            return (uint8_t)addr;
        }
    }

    return 0;
}

static int usb_bus_init_once(void) {
    static int inited;

    if (inited) {
        return 1;
    }

    spinlock_init(&g_usb_buses_lock);

    for (uint32_t i = 0; i < USB_MAX_HCDS; i++) {
        spinlock_init(&g_usb_buses[i].lock);

        g_usb_buses[i].hcd = 0;
        g_usb_buses[i].wq = 0;
        g_usb_buses[i].monitor_task = 0;

        memset(g_usb_buses[i].addr_bitmap, 0, sizeof(g_usb_buses[i].addr_bitmap));
        memset(g_usb_buses[i].root_connected, 0, sizeof(g_usb_buses[i].root_connected));
        g_usb_buses[i].root_port_count = 0;

        dlist_init(&g_usb_buses[i].dev_list);
    }

    spinlock_init(&g_class_driver_lock);

    inited = 1;
    return 1;
}

void usb_request_enumeration(usb_hcd_t* hcd) {
    usb_bus_t* bus = usb_bus_find(hcd);
    if (!bus || !bus->wq) {
        return;
    }

    queue_work(bus->wq, &bus->enum_work);
}

void usb_device_set_class_private(usb_device_t* dev, void* p) {
    if (!dev) {
        return;
    }

    dev->class_private = p;
}

void* usb_device_get_class_private(usb_device_t* dev) {
    if (!dev) {
        return 0;
    }

    return dev->class_private;
}

int usb_register_class_driver(const usb_class_driver_t* drv) {
    if (!drv || !drv->probe || !drv->name) {
        return 0;
    }

    if (!usb_bus_init_once()) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&g_class_driver_lock);

    if (g_class_driver_count >= USB_MAX_CLASS_DRIVERS) {
        spinlock_release_safe(&g_class_driver_lock, flags);
        return 0;
    }

    g_class_drivers[g_class_driver_count++] = drv;

    spinlock_release_safe(&g_class_driver_lock, flags);

    usb_bus_t* buses[USB_MAX_HCDS];
    uint32_t bus_count = 0;

    uint32_t buses_flags = spinlock_acquire_safe(&g_usb_buses_lock);

    for (uint32_t i = 0; i < USB_MAX_HCDS; i++) {
        if (!g_usb_buses[i].hcd || !g_usb_buses[i].wq) {
            continue;
        }

        buses[bus_count++] = &g_usb_buses[i];
    }

    spinlock_release_safe(&g_usb_buses_lock, buses_flags);

    for (uint32_t i = 0; i < bus_count; i++) {
        queue_work(buses[i]->wq, &buses[i]->enum_work);
    }

    return 1;
}

usb_hcd_t* usb_device_get_hcd(usb_device_t* dev) {
    if (!dev) {
        return 0;
    }

    return dev->hcd;
}

const usb_device_info_t* usb_device_get_info(const usb_device_t* dev) {
    if (!dev) {
        return 0;
    }

    return &dev->info;
}

typedef struct {
    semaphore_t sem;
    int status;
    uint32_t actual;
    usb_urb_t urb;
} usb_hcd_urb_sync_t;

static void usb_urb_sync_complete(usb_urb_t* urb) {
    usb_hcd_urb_sync_t* s = (usb_hcd_urb_sync_t*)urb->context;

    s->status = urb->status;
    s->actual = urb->actual_length;
    sem_signal(&s->sem);
}

static uint32_t usb_core_timeout_deadline_tick(uint32_t timeout_us) {
    if (timeout_us == 0u) {
        return 0u;
    }

    uint64_t ms64 = ((uint64_t)timeout_us + 999ull) / 1000ull;
    if (ms64 == 0ull) {
        ms64 = 1ull;
    }
    if (ms64 > 0xFFFFFFFFull) {
        ms64 = 0xFFFFFFFFull;
    }

    uint64_t t = (uint64_t)timer_ticks + ms64;
    if (t > 0xFFFFFFFFull) {
        t = 0xFFFFFFFFull;
    }

    return (uint32_t)t;
}

int usb_submit_urb(usb_device_t* dev, usb_urb_t* urb) {
    if (!dev || !urb || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->submit_urb) {
        return -1;
    }

    return dev->hcd->ops->submit_urb(dev->hcd, urb);
}

int usb_cancel_urb(usb_device_t* dev, usb_urb_t* urb) {
    if (!dev || !urb || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->cancel_urb) {
        return -1;
    }

    return dev->hcd->ops->cancel_urb(dev->hcd, urb);
}

int usb_device_control_xfer(
    usb_device_t* dev,
    const usb_setup_packet_t* setup,
    void* data,
    uint16_t length,
    uint32_t timeout_us
) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->submit_urb) {
        return -1;
    }

    if (!setup) {
        return -1;
    }

    usb_hcd_urb_sync_t* s = (usb_hcd_urb_sync_t*)kzalloc(sizeof(*s));
    if (!s) {
        return -1;
    }

    sem_init(&s->sem, 0);

    memset(&s->urb, 0, sizeof(s->urb));

    s->urb.type = USB_URB_CONTROL;
    s->urb.dev_addr = dev->info.dev_addr;
    s->urb.speed = dev->info.speed;
    s->urb.ep0_mps = dev->info.ep0_mps;
    memcpy(&s->urb.setup, setup, sizeof(s->urb.setup));
    s->urb.buffer = data;
    s->urb.length = length;
    s->urb.timeout_us = timeout_us;
    s->urb.complete = usb_urb_sync_complete;
    s->urb.context = s;

    if (dev->hcd->ops->submit_urb(dev->hcd, &s->urb) != 0) {
        kfree(s);

        return -1;
    }

    if (timeout_us == 0u) {
        sem_wait(&s->sem);
    } else {
        const uint32_t deadline = usb_core_timeout_deadline_tick(timeout_us);

        if (sem_wait_timeout(&s->sem, deadline) == 0) {
            (void)usb_cancel_urb(dev, &s->urb);
            kfree(s);

            return -1;
        }
    }

    const int out = s->status;

    kfree(s);

    return out;
}

int usb_device_bulk_xfer(
    usb_device_t* dev,
    uint8_t ep_num,
    uint8_t dir_in,
    uint16_t max_packet,
    void* data,
    uint32_t length,
    uint32_t timeout_us,
    uint8_t* toggle_io
) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->submit_urb) {
        return -1;
    }

    usb_hcd_urb_sync_t* s = (usb_hcd_urb_sync_t*)kzalloc(sizeof(*s));
    if (!s) {
        return -1;
    }

    sem_init(&s->sem, 0);

    memset(&s->urb, 0, sizeof(s->urb));

    s->urb.type = USB_URB_BULK;
    s->urb.dev_addr = dev->info.dev_addr;
    s->urb.speed = dev->info.speed;
    s->urb.ep_num = ep_num;
    s->urb.dir_in = dir_in;
    s->urb.max_packet = max_packet;
    s->urb.buffer = data;
    s->urb.transfer_buffer_length = length;
    s->urb.toggle_io = toggle_io;
    s->urb.timeout_us = timeout_us;
    s->urb.complete = usb_urb_sync_complete;
    s->urb.context = s;

    if (dev->hcd->ops->submit_urb(dev->hcd, &s->urb) != 0) {
        kfree(s);

        return -1;
    }

    if (timeout_us == 0u) {
        sem_wait(&s->sem);
    } else {
        const uint32_t deadline = usb_core_timeout_deadline_tick(timeout_us);

        if (sem_wait_timeout(&s->sem, deadline) == 0) {
            (void)usb_cancel_urb(dev, &s->urb);
            kfree(s);

            return -1;
        }
    }

    const int out = s->status;

    kfree(s);

    return out;
}

usb_intr_pipe_t* usb_device_intr_open(
    usb_device_t* dev,
    uint8_t ep_num,
    uint16_t max_packet,
    uint8_t interval,
    usb_intr_cb_t cb,
    void* cb_ctx
) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->intr_open) {
        return 0;
    }

    if (!cb) {
        return 0;
    }

    usb_intr_wrap_init_once();

    usb_intr_wrap_t* w = (usb_intr_wrap_t*)kzalloc(sizeof(*w));
    if (!w) {
        return 0;
    }

    usb_device_get(dev);

    w->dev = dev;
    w->cb = cb;
    w->cb_ctx = cb_ctx;

    w->refs_ = 1u;
    w->closing = 0;

    usb_intr_pipe_t* pipe = dev->hcd->ops->intr_open(
        dev->hcd,
        dev->info.dev_addr,
        dev->info.speed,
        ep_num,
        max_packet,
        interval,
        usb_intr_trampoline,
        w
    );

    if (!pipe) {
        usb_intr_wrap_put(w);
        return 0;
    }

    w->pipe = pipe;

    uint32_t flags = spinlock_acquire_safe(&g_usb_intr_wraps.lock);
    dlist_add_tail(&w->node, &g_usb_intr_wraps.list);
    spinlock_release_safe(&g_usb_intr_wraps.lock, flags);

    return pipe;
}

void usb_device_intr_close(usb_device_t* dev, usb_intr_pipe_t* pipe) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !dev->hcd->ops->intr_close || !pipe) {
        return;
    }

    usb_intr_wrap_t* victim = 0;

    uint32_t flags = spinlock_acquire_safe(&g_usb_intr_wraps.lock);
    dlist_head_t* it = g_usb_intr_wraps.list.next;
    while (it != &g_usb_intr_wraps.list) {
        usb_intr_wrap_t* w = container_of(it, usb_intr_wrap_t, node);
        it = it->next;

        if (w->pipe == pipe) {
            dlist_del(&w->node);
            victim = w;
            break;
        }
    }
    spinlock_release_safe(&g_usb_intr_wraps.lock, flags);

    if (victim) {
        __atomic_store_n(&victim->closing, 1, __ATOMIC_RELEASE);
    }

    dev->hcd->ops->intr_close(dev->hcd, pipe);

    if (victim) {
        usb_intr_wrap_put(victim);
    }
}

static int usb_control(
    usb_hcd_t* hcd,
    uint8_t dev_addr,
    usb_speed_t speed,
    uint16_t ep0_mps,
    const usb_setup_packet_t* setup,
    void* data,
    uint16_t length,
    uint32_t timeout_us
) {
    if (!hcd || !hcd->ops || !hcd->ops->submit_urb) {
        return -1;
    }

    if (!setup) {
        return -1;
    }

    usb_hcd_urb_sync_t* s = (usb_hcd_urb_sync_t*)kzalloc(sizeof(*s));
    if (!s) {
        return -1;
    }

    sem_init(&s->sem, 0);

    memset(&s->urb, 0, sizeof(s->urb));

    s->urb.type = USB_URB_CONTROL;
    s->urb.dev_addr = dev_addr;
    s->urb.speed = speed;
    s->urb.ep0_mps = ep0_mps;
    memcpy(&s->urb.setup, setup, sizeof(s->urb.setup));
    s->urb.buffer = data;
    s->urb.length = length;
    s->urb.timeout_us = timeout_us;
    s->urb.complete = usb_urb_sync_complete;
    s->urb.context = s;

    if (hcd->ops->submit_urb(hcd, &s->urb) != 0) {
        kfree(s);

        return -1;
    }

    if (timeout_us == 0u) {
        sem_wait(&s->sem);
    } else {
        const uint32_t deadline = usb_core_timeout_deadline_tick(timeout_us);

        if (sem_wait_timeout(&s->sem, deadline) == 0) {
            (void)hcd->ops->cancel_urb(hcd, &s->urb);
            kfree(s);

            return -1;
        }
    }

    const int out = s->status;

    kfree(s);

    return out;
}

static int usb_get_descriptor(
    usb_hcd_t* hcd,
    uint8_t addr,
    usb_speed_t speed,
    uint16_t ep0_mps,
    uint8_t desc_type,
    uint8_t desc_index,
    void* out,
    uint16_t out_len
) {
    if (!out || out_len == 0) {
        return -1;
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)desc_type << 8) | desc_index);
    setup.wIndex = 0;
    setup.wLength = out_len;

    return usb_control(hcd, addr, speed, ep0_mps, &setup, out, out_len, 1000000u);
}

static int usb_set_address(usb_hcd_t* hcd, usb_speed_t speed, uint16_t ep0_mps, uint8_t new_addr) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;

    const int r = usb_control(hcd, 0, speed, ep0_mps, &setup, 0, 0, 1000000u);
    return r >= 0;
}

static int usb_set_configuration(usb_hcd_t* hcd, uint8_t addr, usb_speed_t speed, uint16_t ep0_mps, uint8_t cfg_value) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = cfg_value;
    setup.wIndex = 0;
    setup.wLength = 0;

    const int r = usb_control(hcd, addr, speed, ep0_mps, &setup, 0, 0, 1000000u);
    return r >= 0;
}

static uint16_t usb_ep0_mps_sanitize(uint8_t bMaxPacketSize0) {
    if (bMaxPacketSize0 == 0) {
        return 8;
    }

    if (bMaxPacketSize0 > 64) {
        return 64;
    }

    return bMaxPacketSize0;
}

static usb_device_t* usb_device_alloc(usb_hcd_t* hcd) {
    usb_device_t* dev = (usb_device_t*)kzalloc(sizeof(*dev));
    if (!dev) {
        return 0;
    }

    dev->hcd = hcd;

    dev->refs_ = 1u;
    return dev;
}

static void usb_device_free(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    if (dev->cfg_buf) {
        kfree(dev->cfg_buf);
        dev->cfg_buf = 0;
    }

    kfree(dev);
}

__attribute__((noinline)) void usb_device_get(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    __atomic_fetch_add(&dev->refs_, 1u, __ATOMIC_RELAXED);
}

__attribute__((noinline)) void usb_device_put(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    const uint32_t old = __atomic_fetch_sub(&dev->refs_, 1u, __ATOMIC_ACQ_REL);
    if (old == 1u) {
        usb_device_free(dev);
    }
}

static int usb_fetch_full_config(
    usb_hcd_t* hcd,
    uint8_t addr,
    usb_speed_t speed,
    uint16_t ep0_mps,
    uint8_t* out_cfg_value,
    uint8_t** out_buf,
    uint16_t* out_len
) {
    if (!out_cfg_value || !out_buf || !out_len) {
        return 0;
    }

    usb_config_descriptor_t cd;
    memset(&cd, 0, sizeof(cd));

    const int r0 = usb_get_descriptor(hcd, addr, speed, ep0_mps, USB_DESC_CONFIGURATION, 0, &cd, sizeof(cd));
    if (r0 < 9) {
        return 0;
    }

    uint16_t total = usb_le16_read(&cd.wTotalLength);
    if (total < 9) {
        total = 9;
    }

    if (total > 2048) {
        total = 2048;
    }

    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) {
        return 0;
    }

    memset(buf, 0, total);

    const int r1 = usb_get_descriptor(hcd, addr, speed, ep0_mps, USB_DESC_CONFIGURATION, 0, buf, total);
    if (r1 < (int)total) {
        kfree(buf);
        return 0;
    }

    const usb_config_descriptor_t* cdh = (const usb_config_descriptor_t*)buf;
    *out_cfg_value = cdh->bConfigurationValue;

    *out_buf = buf;
    *out_len = total;

    return 1;
}

static void usb_try_probe_classes(usb_device_t* dev) {
    if (!dev || !dev->cfg_buf || dev->cfg_len < sizeof(usb_config_descriptor_t)) {
        return;
    }

    const usb_class_driver_t* snapshot[USB_MAX_CLASS_DRIVERS];
    uint32_t count = 0;

    uint32_t flags = spinlock_acquire_safe(&g_class_driver_lock);

    count = g_class_driver_count;
    if (count > USB_MAX_CLASS_DRIVERS) {
        count = USB_MAX_CLASS_DRIVERS;
    }

    for (uint32_t i = 0; i < count; i++) {
        snapshot[i] = g_class_drivers[i];
    }

    spinlock_release_safe(&g_class_driver_lock, flags);

    for (uint32_t i = 0; i < count; i++) {
        const usb_class_driver_t* drv = snapshot[i];
        if (!drv || !drv->probe) {
            continue;
        }

        if (drv->probe(dev, dev->cfg_buf, dev->cfg_len)) {
            dev->bound_driver = drv;
            return;
        }
    }
}

static void usb_class_disconnect(usb_device_t* dev) {
    if (!dev || !dev->bound_driver || !dev->bound_driver->disconnect) {
        return;
    }

    dev->bound_driver->disconnect(dev);
}

void usb_detach_device(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    usb_bus_t* bus = usb_bus_find(dev->hcd);
    if (!bus) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&bus->lock);

    dlist_del(&dev->node);
    usb_addr_clear(bus, dev->info.dev_addr);

    spinlock_release_safe(&bus->lock, flags);

    usb_class_disconnect(dev);

    usb_device_put(dev);
}

void usb_detach_child_device(usb_device_t* parent, uint8_t hub_port) {
    if (!parent || hub_port == 0) {
        return;
    }

    usb_bus_t* bus = usb_bus_find(parent->hcd);
    if (!bus) {
        return;
    }

    usb_device_t* victim = 0;

    uint32_t flags = spinlock_acquire_safe(&bus->lock);

    dlist_head_t* it = bus->dev_list.next;
    while (it != &bus->dev_list) {
        usb_device_t* d = container_of(it, usb_device_t, node);
        it = it->next;

        if (d->parent == parent && d->hub_port == hub_port) {
            victim = d;
            break;
        }
    }

    if (victim) {
        dlist_del(&victim->node);
        usb_addr_clear(bus, victim->info.dev_addr);
    }

    spinlock_release_safe(&bus->lock, flags);

    if (!victim) {
        return;
    }

    usb_class_disconnect(victim);
    usb_device_put(victim);
}

static usb_device_t* usb_find_root_device(usb_bus_t* bus, uint8_t root_port) {
    if (!bus || root_port == 0) {
        return 0;
    }

    dlist_head_t* it = bus->dev_list.next;
    while (it != &bus->dev_list) {
        usb_device_t* d = container_of(it, usb_device_t, node);
        it = it->next;

        if (!d->parent && d->root_port == root_port) {
            return d;
        }
    }

    return 0;
}

static void usb_detach_root_port(usb_bus_t* bus, uint8_t port) {
    if (!bus || port == 0) {
        return;
    }

    usb_device_t* victim = 0;

    uint32_t flags = spinlock_acquire_safe(&bus->lock);

    victim = usb_find_root_device(bus, port);
    if (victim) {
        dlist_del(&victim->node);
        usb_addr_clear(bus, victim->info.dev_addr);
    }

    spinlock_release_safe(&bus->lock, flags);

    if (!victim) {
        return;
    }

    usb_class_disconnect(victim);
    usb_device_put(victim);
}

static int usb_enumerate_new_device(
    usb_bus_t* bus,
    usb_device_t* parent,
    uint8_t root_port,
    uint8_t hub_port,
    usb_speed_t speed
) {
    if (!bus) {
        return 0;
    }

    usb_hcd_t* hcd = bus->hcd;
    if (!hcd || !hcd->ops) {
        return 0;
    }

    usb_device_descriptor_t dd;
    memset(&dd, 0, sizeof(dd));

    int r = usb_get_descriptor(hcd, 0, speed, 8, USB_DESC_DEVICE, 0, &dd, 8);
    if (r < 8) {
        return 0;
    }

    const uint16_t ep0_mps = usb_ep0_mps_sanitize(dd.bMaxPacketSize0);

    uint8_t addr = 0;

    uint32_t lock_flags = spinlock_acquire_safe(&bus->lock);
    addr = usb_addr_alloc(bus);
    spinlock_release_safe(&bus->lock, lock_flags);

    if (!addr) {
        return 0;
    }

    if (!usb_set_address(hcd, speed, ep0_mps, addr)) {
        lock_flags = spinlock_acquire_safe(&bus->lock);
        usb_addr_clear(bus, addr);
        spinlock_release_safe(&bus->lock, lock_flags);
        return 0;
    }

    proc_usleep(2000);

    memset(&dd, 0, sizeof(dd));
    (void)usb_get_descriptor(hcd, addr, speed, ep0_mps, USB_DESC_DEVICE, 0, &dd, sizeof(dd));

    uint8_t cfg_value = 0;
    uint8_t* cfg_buf = 0;
    uint16_t cfg_len = 0;

    if (!usb_fetch_full_config(hcd, addr, speed, ep0_mps, &cfg_value, &cfg_buf, &cfg_len)) {
        lock_flags = spinlock_acquire_safe(&bus->lock);
        usb_addr_clear(bus, addr);
        spinlock_release_safe(&bus->lock, lock_flags);
        return 0;
    }

    if (!usb_set_configuration(hcd, addr, speed, ep0_mps, cfg_value)) {
        kfree(cfg_buf);

        lock_flags = spinlock_acquire_safe(&bus->lock);
        usb_addr_clear(bus, addr);
        spinlock_release_safe(&bus->lock, lock_flags);

        return 0;
    }

    usb_device_t* dev = usb_device_alloc(hcd);
    if (!dev) {
        kfree(cfg_buf);

        lock_flags = spinlock_acquire_safe(&bus->lock);
        usb_addr_clear(bus, addr);
        spinlock_release_safe(&bus->lock, lock_flags);

        return 0;
    }

    dev->root_port = root_port;
    dev->parent = parent;
    dev->hub_port = hub_port;

    dev->info.dev_addr = addr;
    dev->info.speed = speed;
    dev->info.ep0_mps = ep0_mps;
    dev->info.device_desc = dd;
    dev->info.active_config = cfg_value;

    dev->info.hub_depth = parent ? (uint8_t)(parent->info.hub_depth + 1u) : 0u;

    dev->cfg_buf = cfg_buf;
    dev->cfg_len = cfg_len;

    lock_flags = spinlock_acquire_safe(&bus->lock);
    dlist_add_tail(&dev->node, &bus->dev_list);
    spinlock_release_safe(&bus->lock, lock_flags);

    usb_try_probe_classes(dev);

    if (!dev->bound_driver) {
        usb_detach_device(dev);
        return 0;
    }

    return 1;
}

static int usb_enum_one_root_port(usb_bus_t* bus, uint8_t port) {
    if (!bus) {
        return 0;
    }

    usb_hcd_t* hcd = bus->hcd;
    if (!hcd || !hcd->ops) {
        return 0;
    }

    usb_port_status_t st;
    memset(&st, 0, sizeof(st));

    if (!hcd->ops->root_port_get_status(hcd, port, &st)) {
        return 0;
    }

    const uint8_t prev = bus->root_connected[port];
    bus->root_connected[port] = st.connected ? 1u : 0u;

    if (!st.connected) {
        if (prev) {
            usb_detach_root_port(bus, port);
        }

        return 0;
    }

    if (prev) {
        return 1;
    }

    if (!hcd->ops->root_port_reset(hcd, port)) {
        return 0;
    }

    return usb_enumerate_new_device(bus, 0, port, 0, st.speed);
}

int usb_enumerate_child_device(usb_device_t* parent, uint8_t hub_port, usb_speed_t speed) {
    if (!parent || hub_port == 0) {
        return 0;
    }

    usb_bus_t* bus = usb_bus_find(parent->hcd);
    if (!bus) {
        return 0;
    }

    usb_detach_child_device(parent, hub_port);

    return usb_enumerate_new_device(bus, parent, parent->root_port, hub_port, speed);
}

static void usb_enum_work(work_struct_t* work) {
    usb_bus_t* bus = container_of(work, usb_bus_t, enum_work);
    if (!bus || !bus->hcd || !bus->hcd->ops || !bus->hcd->ops->root_port_count) {
        return;
    }

    const uint8_t ports = bus->hcd->ops->root_port_count(bus->hcd);
    bus->root_port_count = ports;

    const uint8_t max_ports = (uint8_t)(sizeof(bus->root_connected) / sizeof(bus->root_connected[0]));
    const uint8_t capped = ports < max_ports ? ports : (uint8_t)(max_ports - 1u);

    for (uint8_t port = 1; port <= capped; port++) {
        (void)usb_enum_one_root_port(bus, port);
    }
}

static void usb_monitor_thread(void* arg) {
    usb_bus_t* bus = (usb_bus_t*)arg;
    if (!bus) {
        return;
    }

    uint32_t last_tick = timer_ticks;

    for (;;) {
        uint32_t now = timer_ticks;
        if ((uint32_t)(now - last_tick) >= 100u) {
            last_tick = now;
            usb_request_enumeration(bus->hcd);
        }

        proc_usleep(20000);
    }
}

int usb_register_hcd(usb_hcd_t* hcd) {
    if (!hcd || !hcd->ops || !hcd->name) {
        return 0;
    }

    if (!usb_bus_init_once()) {
        return 0;
    }

    usb_bus_t* bus = 0;

    uint32_t flags = spinlock_acquire_safe(&g_usb_buses_lock);

    for (uint32_t i = 0; i < USB_MAX_HCDS; i++) {
        if (g_usb_buses[i].hcd == hcd) {
            spinlock_release_safe(&g_usb_buses_lock, flags);
            return 0;
        }
    }

    for (uint32_t i = 0; i < USB_MAX_HCDS; i++) {
        if (!g_usb_buses[i].hcd) {
            bus = &g_usb_buses[i];
            bus->hcd = hcd;

            memset(bus->addr_bitmap, 0, sizeof(bus->addr_bitmap));
            memset(bus->root_connected, 0, sizeof(bus->root_connected));
            bus->root_port_count = 0;
            bus->monitor_task = 0;

            dlist_init(&bus->dev_list);

            break;
        }
    }

    spinlock_release_safe(&g_usb_buses_lock, flags);

    if (!bus) {
        return 0;
    }

    bus->wq = create_workqueue("usb");
    if (!bus->wq) {
        uint32_t clear_flags = spinlock_acquire_safe(&g_usb_buses_lock);
        bus->hcd = 0;
        spinlock_release_safe(&g_usb_buses_lock, clear_flags);
        return 0;
    }

    init_work(&bus->enum_work, usb_enum_work);

    if (hcd->ops->start) {
        if (!hcd->ops->start(hcd)) {
            destroy_workqueue(bus->wq);
            bus->wq = 0;

            uint32_t clear_flags = spinlock_acquire_safe(&g_usb_buses_lock);
            bus->hcd = 0;
            spinlock_release_safe(&g_usb_buses_lock, clear_flags);

            return 0;
        }
    }

    queue_work(bus->wq, &bus->enum_work);

    bus->monitor_task = proc_spawn_kthread("usbmon", PRIO_LOW, usb_monitor_thread, bus);

    return 1;
}
