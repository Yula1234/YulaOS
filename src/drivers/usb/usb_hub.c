#include <drivers/usb/usb_hub.h>

#include <drivers/usb/usb_core.h>

#include <kernel/workqueue.h>

#include <kernel/proc.h>

#include <hal/lock.h>

#include <mm/heap.h>

#include <lib/string.h>

#define USB_CLASS_HUB 0x09u

#define USB_DESC_HUB 0x29u

#define USB_REQ_HUB_GET_STATUS 0x00u
#define USB_REQ_HUB_CLEAR_FEATURE 0x01u
#define USB_REQ_HUB_SET_FEATURE 0x03u

#define USB_HUB_FEATURE_PORT_RESET 4u
#define USB_HUB_FEATURE_PORT_POWER 8u

#define USB_HUB_FEATURE_C_PORT_CONNECTION 16u
#define USB_HUB_FEATURE_C_PORT_RESET 20u

#define USB_HUB_PORT_STATUS_CONNECTION (1u << 0)
#define USB_HUB_PORT_STATUS_LOW_SPEED (1u << 9)

typedef struct __attribute__((packed)) {
    uint8_t bDescLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
} usb_hub_descriptor_t;

typedef struct __attribute__((packed)) {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} usb_hub_port_status_t;

typedef struct {
    usb_device_t* dev;

    uint8_t port_count;
    uint32_t port_power_wait_us;

    uint8_t* intr_buf;
    uint16_t intr_len;

    usb_intr_pipe_t* intr;

    workqueue_t* wq;
    work_struct_t work;

    spinlock_t lock;
    uint8_t pending[32];
    uint16_t pending_len;
    uint8_t dying;
} usb_hub_t;

static int hub_get_descriptor(usb_device_t* dev, usb_hub_descriptor_t* out) {
    if (!dev || !out) {
        return 0;
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)USB_DESC_HUB << 8) | 0u);
    setup.wIndex = 0;
    setup.wLength = sizeof(*out);

    return usb_device_control_xfer(dev, &setup, out, sizeof(*out), 1000000u) == (int)sizeof(*out);
}

static int hub_port_get_status(usb_device_t* dev, uint8_t port, usb_hub_port_status_t* out) {
    if (!dev || !out || port == 0) {
        return 0;
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER;
    setup.bRequest = USB_REQ_HUB_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = port;
    setup.wLength = sizeof(*out);

    return usb_device_control_xfer(dev, &setup, out, sizeof(*out), 1000000u) == (int)sizeof(*out);
}

static int hub_port_set_feature(usb_device_t* dev, uint8_t port, uint16_t feature) {
    if (!dev || port == 0) {
        return 0;
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER;
    setup.bRequest = USB_REQ_HUB_SET_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    return usb_device_control_xfer(dev, &setup, 0, 0, 1000000u) >= 0;
}

static int hub_port_clear_feature(usb_device_t* dev, uint8_t port, uint16_t feature) {
    if (!dev || port == 0) {
        return 0;
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER;
    setup.bRequest = USB_REQ_HUB_CLEAR_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    return usb_device_control_xfer(dev, &setup, 0, 0, 1000000u) >= 0;
}

static usb_speed_t hub_port_get_speed(const usb_hub_port_status_t* st) {
    if (!st) {
        return USB_SPEED_FULL;
    }

    const uint16_t ps = usb_le16_read(&st->wPortStatus);
    if (ps & USB_HUB_PORT_STATUS_LOW_SPEED) {
        return USB_SPEED_LOW;
    }

    return USB_SPEED_FULL;
}

static int hub_port_connected(const usb_hub_port_status_t* st) {
    if (!st) {
        return 0;
    }

    const uint16_t ps = usb_le16_read(&st->wPortStatus);
    return (ps & USB_HUB_PORT_STATUS_CONNECTION) != 0;
}

static void hub_handle_port_change(usb_hub_t* hub, uint8_t port) {
    if (!hub || !hub->dev || port == 0 || port > hub->port_count) {
        return;
    }

    usb_hub_port_status_t st;
    memset(&st, 0, sizeof(st));

    if (!hub_port_get_status(hub->dev, port, &st)) {
        return;
    }

    const uint16_t ch = usb_le16_read(&st.wPortChange);

    if (ch) {
        if (ch & (1u << 0)) {
            (void)hub_port_clear_feature(hub->dev, port, USB_HUB_FEATURE_C_PORT_CONNECTION);
        }

        if (ch & (1u << 4)) {
            (void)hub_port_clear_feature(hub->dev, port, USB_HUB_FEATURE_C_PORT_RESET);
        }
    }

    if (!hub_port_connected(&st)) {
        usb_detach_child_device(hub->dev, port);
        return;
    }

    (void)hub_port_set_feature(hub->dev, port, USB_HUB_FEATURE_PORT_RESET);
    proc_usleep(60000);

    (void)hub_port_clear_feature(hub->dev, port, USB_HUB_FEATURE_C_PORT_RESET);

    memset(&st, 0, sizeof(st));
    if (!hub_port_get_status(hub->dev, port, &st)) {
        return;
    }

    usb_enumerate_child_device(hub->dev, port, hub_port_get_speed(&st));
}

static void hub_pending_set_bit(usb_hub_t* hub, uint32_t bit) {
    if (!hub) {
        return;
    }

    const uint32_t idx = bit / 8u;
    const uint32_t mask = 1u << (bit % 8u);

    if (idx >= (uint32_t)hub->pending_len) {
        return;
    }

    hub->pending[idx] |= (uint8_t)mask;
}

static void hub_pending_or_bitmap(usb_hub_t* hub, const uint8_t* src, uint16_t len) {
    if (!hub || !src || len == 0) {
        return;
    }

    const uint16_t n = len > hub->pending_len ? hub->pending_len : len;
    for (uint16_t i = 0; i < n; i++) {
        hub->pending[i] |= src[i];
    }
}

static void hub_work(work_struct_t* work) {
    usb_hub_t* hub = container_of(work, usb_hub_t, work);
    if (!hub || !hub->dev) {
        return;
    }

    uint8_t bitmap[32];
    uint16_t len = 0;

    uint32_t flags = spinlock_acquire_safe(&hub->lock);

    if (hub->dying) {
        spinlock_release_safe(&hub->lock, flags);
        return;
    }

    len = hub->pending_len;
    if (len > sizeof(bitmap)) {
        len = sizeof(bitmap);
    }

    memcpy(bitmap, hub->pending, len);
    memset(hub->pending, 0, len);

    spinlock_release_safe(&hub->lock, flags);

    for (uint32_t port = 1; port <= hub->port_count; port++) {
        const uint32_t bit = port;
        const uint32_t idx = bit / 8u;
        const uint32_t mask = 1u << (bit % 8u);

        if (idx >= len) {
            continue;
        }

        if ((bitmap[idx] & mask) == 0) {
            continue;
        }

        hub_handle_port_change(hub, (uint8_t)port);
    }
}

static void hub_intr_cb(void* ctx, const uint8_t* data, uint32_t len) {
    usb_hub_t* hub = (usb_hub_t*)ctx;
    if (!hub || !hub->dev || !data || len == 0) {
        return;
    }

    const uint16_t use_len = (uint16_t)(len > hub->intr_len ? hub->intr_len : (uint16_t)len);
    if (use_len == 0) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&hub->lock);

    if (!hub->dying) {
        hub_pending_or_bitmap(hub, data, use_len);
    }

    spinlock_release_safe(&hub->lock, flags);

    if (hub->wq) {
        queue_work(hub->wq, &hub->work);
    }
}

static int hub_parse_cfg(
    const uint8_t* cfg,
    uint16_t cfg_len,
    uint8_t* out_iface,
    uint8_t* out_ep_in,
    uint16_t* out_ep_mps,
    uint8_t* out_ep_interval
) {
    if (!cfg || cfg_len < sizeof(usb_config_descriptor_t)) {
        return 0;
    }

    const usb_config_descriptor_t* cd = (const usb_config_descriptor_t*)cfg;
    if (cd->bLength < 9 || cd->bDescriptorType != USB_DESC_CONFIGURATION) {
        return 0;
    }

    uint8_t iface_num = 0;
    int in_hub = 0;

    uint16_t i = 0;
    while (i + 2 <= cfg_len) {
        const uint8_t blen = cfg[i + 0];
        const uint8_t dtype = cfg[i + 1];

        if (blen < 2) {
            break;
        }

        if ((uint32_t)i + (uint32_t)blen > cfg_len) {
            break;
        }

        if (dtype == USB_DESC_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t* id = (const usb_interface_descriptor_t*)&cfg[i];

            if (id->bInterfaceClass == USB_CLASS_HUB) {
                in_hub = 1;
                iface_num = id->bInterfaceNumber;
            } else {
                in_hub = 0;
            }
        } else if (dtype == USB_DESC_ENDPOINT && blen >= sizeof(usb_endpoint_descriptor_t)) {
            if (in_hub) {
                const usb_endpoint_descriptor_t* ed = (const usb_endpoint_descriptor_t*)&cfg[i];

                const uint8_t ep_addr = ed->bEndpointAddress;
                const uint8_t ep_attr = ed->bmAttributes & USB_EP_XFER_MASK;

                if ((ep_addr & USB_EP_DIR_IN) && ep_attr == USB_EP_XFER_INT) {
                    *out_iface = iface_num;
                    *out_ep_in = (uint8_t)(ep_addr & USB_EP_NUM_MASK);
                    *out_ep_mps = (uint16_t)(usb_le16_read(&ed->wMaxPacketSize) & 0x07FFu);
                    *out_ep_interval = ed->bInterval;
                    return 1;
                }
            }
        }

        i = (uint16_t)(i + blen);
    }

    return 0;
}

static void usb_hub_disconnect(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    usb_hub_t* hub = (usb_hub_t*)usb_device_get_class_private(dev);
    if (!hub) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&hub->lock);
    hub->dying = 1;
    spinlock_release_safe(&hub->lock, flags);

    for (uint8_t port = 1; port <= hub->port_count; port++) {
        usb_detach_child_device(dev, port);
    }

    if (hub->intr) {
        usb_device_intr_close(dev, hub->intr);
        hub->intr = 0;
    }

    if (hub->intr_buf) {
        kfree(hub->intr_buf);
        hub->intr_buf = 0;
    }

    if (hub->wq) {
        destroy_workqueue(hub->wq);
        hub->wq = 0;
    }

    usb_device_set_class_private(dev, 0);
    kfree(hub);
}

static int usb_hub_probe(usb_device_t* dev, const uint8_t* cfg, uint16_t cfg_len) {
    if (!dev || !cfg || cfg_len == 0) {
        return 0;
    }

    uint8_t iface = 0;
    uint8_t ep_in = 0;
    uint16_t ep_mps = 0;
    uint8_t interval = 0;

    if (!hub_parse_cfg(cfg, cfg_len, &iface, &ep_in, &ep_mps, &interval)) {
        return 0;
    }

    usb_hub_descriptor_t hd;
    memset(&hd, 0, sizeof(hd));

    if (!hub_get_descriptor(dev, &hd)) {
        return 0;
    }

    if (hd.bNbrPorts == 0) {
        return 0;
    }

    usb_hub_t* hub = (usb_hub_t*)kzalloc(sizeof(*hub));
    if (!hub) {
        return 0;
    }

    hub->dev = dev;
    hub->port_count = hd.bNbrPorts;

    hub->intr_len = (uint16_t)((hub->port_count + 1u + 7u) / 8u);
    if (hub->intr_len == 0 || hub->intr_len > 32u) {
        kfree(hub);
        return 0;
    }

    hub->pending_len = hub->intr_len;
    if (hub->pending_len > sizeof(hub->pending)) {
        hub->pending_len = sizeof(hub->pending);
    }

    spinlock_init(&hub->lock);
    hub->dying = 0;
    memset(hub->pending, 0, sizeof(hub->pending));

    init_work(&hub->work, hub_work);
    hub->wq = create_workqueue("usbhub");
    if (!hub->wq) {
        kfree(hub);
        return 0;
    }

    hub->intr_buf = (uint8_t*)kmalloc(hub->intr_len);
    if (!hub->intr_buf) {
        kfree(hub);
        return 0;
    }

    memset(hub->intr_buf, 0, hub->intr_len);

    hub->port_power_wait_us = (uint32_t)hd.bPwrOn2PwrGood * 2000u;

    for (uint8_t port = 1; port <= hub->port_count; port++) {
        (void)hub_port_set_feature(dev, port, USB_HUB_FEATURE_PORT_POWER);
    }

    if (hub->port_power_wait_us) {
        proc_usleep(hub->port_power_wait_us);
    }

    usb_device_set_class_private(dev, hub);

    hub->intr = usb_device_intr_open(
        dev,
        ep_in,
        ep_mps ? ep_mps : 8u,
        interval ? interval : 12u,
        hub_intr_cb,
        hub
    );

    if (!hub->intr) {
        usb_hub_disconnect(dev);
        return 0;
    }

    uint32_t lock_flags = spinlock_acquire_safe(&hub->lock);
    for (uint32_t port = 1; port <= hub->port_count; port++) {
        hub_pending_set_bit(hub, port);
    }
    spinlock_release_safe(&hub->lock, lock_flags);

    queue_work(hub->wq, &hub->work);

    return 1;
}

static const usb_class_driver_t g_usb_hub_drv = {
    .name = "usb_hub",
    .probe = usb_hub_probe,
    .disconnect = usb_hub_disconnect,
};

int usb_hub_init(void) {
    return usb_register_class_driver(&g_usb_hub_drv);
}
