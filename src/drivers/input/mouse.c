/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/locking/guards.h>
#include <kernel/input_focus.h>
#include <kernel/workqueue.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <hal/delay.h>
#include <hal/align.h>
#include <hal/lock.h>
#include <hal/pmio.h>
#include <hal/pic.h>
#include <hal/irq.h>

#include <lib/compiler.h>
#include <lib/atomic.h>
#include <lib/string.h>

#include <drivers/virtio/vgpu.h>
#include <drivers/video/fbdev.h>

#include <arch/i386/idt.h>

#include <fs/vfs.h>

#include "mouse.h"

int mouse_x = 512;
int mouse_y = 384;

int mouse_buttons = 0;

static __cacheline_aligned rwspinlock_t g_mouse_state_lock;
static __cacheline_aligned poll_waitq_t g_mouse_poll_waitq;

static __cacheline_aligned atomic_uint_t g_mouse_dirty = ATOMIC_UINT_INIT(0);

enum {
    PS2_PORT_DATA    = 0x60u,
    PS2_PORT_STATUS  = 0x64u,
    PS2_PORT_COMMAND = 0x64u,
};

__cacheline_aligned static spinlock_t g_ps2_lock;

__cacheline_aligned static pmio_region_t* g_ps2_data_region = NULL;
__cacheline_aligned static pmio_region_t* g_ps2_cmd_region = NULL;

__cacheline_aligned static int g_ps2_pmio_inited = 0;

#define MOUSE_RING_CAPACITY 256u

typedef struct mouse_ring {
    uint8_t buffer_[MOUSE_RING_CAPACITY];
    
    atomic_uint_t head_;
    atomic_uint_t tail_;
} mouse_ring_t;

__cacheline_aligned static mouse_ring_t g_mouse_ring;
__cacheline_aligned static work_struct_t g_mouse_work;

__cacheline_aligned static workqueue_t* g_mouse_wq = NULL;

___inline void mouse_ring_init(void) {
    atomic_uint_set(&g_mouse_ring.head_, 0u);
    atomic_uint_set(&g_mouse_ring.tail_, 0u);
}

___inline int mouse_ring_push(uint8_t data) {
    const uint32_t head = atomic_uint_load_explicit(&g_mouse_ring.head_, ATOMIC_RELAXED);
    const uint32_t tail = atomic_uint_load_explicit(&g_mouse_ring.tail_, ATOMIC_ACQUIRE);
    
    const uint32_t next = (head + 1u) % MOUSE_RING_CAPACITY;

    if (unlikely(next == tail)) {
        return 0;
    }

    g_mouse_ring.buffer_[head] = data;
    atomic_uint_store_explicit(&g_mouse_ring.head_, next, ATOMIC_RELEASE);

    return 1;
}

___inline int mouse_ring_pop(uint8_t* out_data) {
    const uint32_t tail = atomic_uint_load_explicit(&g_mouse_ring.tail_, ATOMIC_RELAXED);
    const uint32_t head = atomic_uint_load_explicit(&g_mouse_ring.head_, ATOMIC_ACQUIRE);

    if (head == tail) {
        return 0;
    }

    *out_data = g_mouse_ring.buffer_[tail];

    atomic_uint_store_explicit(&g_mouse_ring.tail_, (tail + 1u) % MOUSE_RING_CAPACITY, ATOMIC_RELEASE);

    return 1;
}

static void mouse_get_display_bounds(int* out_width, int* out_height) {
    int max_w = (int)fb_width;
    int max_h = (int)fb_height;

    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();

        if (fb
            && fb->width > 0u
            && fb->height > 0u) {
            
            max_w = (int)fb->width;
            max_h = (int)fb->height;
        }
    }

    if (unlikely(max_w < 1)) {
        max_w = 1;
    }

    if (unlikely(max_h < 1)) {
        max_h = 1;
    }

    *out_width = max_w;
    *out_height = max_h;
}

static int mouse_has_focus(task_t* task) {
    if (unlikely(!task)) {
        return 0;
    }

    const uint32_t owner_pid = fb_get_owner_pid();

    if (owner_pid != 0u) {
        return (task->pid == owner_pid) ? 1 : 0;
    }

    const uint32_t focus_pid = input_focus_get_pid();

    if (focus_pid > 0u
        && task->pid != focus_pid) {
        return 0;
    }

    return 1;
}

void mouse_inject_delta(int dx, int dy, int buttons) {
    int max_w = 0;
    int max_h = 0;

    mouse_get_display_bounds(&max_w, &max_h);

    {
        guard_rwspin_write_safe(&g_mouse_state_lock);

        mouse_buttons = buttons & 0x07;

        mouse_x += dx;
        mouse_y += dy;

        if (mouse_x < 0) {
            mouse_x = 0;
        }

        if (mouse_y < 0) {
            mouse_y = 0;
        }

        if (mouse_x >= max_w) {
            mouse_x = max_w - 1;
        }

        if (mouse_y >= max_h) {
            mouse_y = max_h - 1;
        }
    }

    atomic_uint_store_explicit(&g_mouse_dirty, 1u, ATOMIC_RELEASE);

    mouse_poll_notify_focus_change();
}

static void mouse_bh_worker(___unused work_struct_t* work) {
    static uint8_t cycle = 0u;

    static uint8_t packet[3] = { 0 };

    uint8_t data = 0u;

    while (mouse_ring_pop(&data)) {
        if (cycle == 0u
            && (data & 0x08u) == 0u) {
            continue;
        }

        packet[cycle++] = data;

        if (cycle == 3u) {
            cycle = 0u;

            if (unlikely((packet[0] & 0x80u) != 0u
                || (packet[0] & 0x40u) != 0u)) {
                continue;
            }

            const int32_t rel_x = (int8_t)packet[1];
            const int32_t rel_y = (int8_t)packet[2];

            const int buttons = (int)(packet[0] & 0x07u);

            mouse_inject_delta((int)rel_x, (int)-rel_y, buttons);
        }
    }
}

void mouse_process_byte(uint8_t data) {
    if (likely(mouse_ring_push(data))) {
        if (likely(g_mouse_wq != NULL)) {
            queue_work(g_mouse_wq, &g_mouse_work);
        }
    }
}

___inline int mouse_pmio_is_ready(void) {
    return g_ps2_data_region != NULL && g_ps2_cmd_region != NULL;
}

static int mouse_pmio_init(void) {
    if (unlikely(g_ps2_pmio_inited)) {
        return 0;
    }

    rwspinlock_init(&g_mouse_state_lock);
    spinlock_init(&g_ps2_lock);
    
    mouse_ring_init();

    g_mouse_wq = create_workqueue("ps2mwork");
    
    init_work(&g_mouse_work, mouse_bh_worker);

    g_ps2_data_region = pmio_request_region(PS2_PORT_DATA, 1u, "ps2_data");

    if (unlikely(!g_ps2_data_region)) {
        return -1;
    }

    g_ps2_cmd_region = pmio_request_region(PS2_PORT_COMMAND, 1u, "ps2_ctrl");

    if (unlikely(!g_ps2_cmd_region)) {
        pmio_release_region(g_ps2_data_region);

        g_ps2_data_region = NULL;

        return -1;
    }

    g_ps2_pmio_inited = 1;

    return 0;
}

___inline uint8_t mouse_ps2_read_status(void) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return 0u;
    }

    uint8_t status = 0u;

    (void)pmio_readb(g_ps2_cmd_region, 0u, &status);

    return status;
}

___inline void mouse_ps2_write_command(uint8_t cmd) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    (void)pmio_writeb(g_ps2_cmd_region, 0u, cmd);
}

___inline void mouse_ps2_write_data(uint8_t data) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    (void)pmio_writeb(g_ps2_data_region, 0u, data);
}

___inline uint8_t mouse_ps2_read_data(void) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return 0u;
    }

    uint8_t data = 0u;

    (void)pmio_readb(g_ps2_data_region, 0u, &data);

    return data;
}

static void mouse_wait_unlocked(uint8_t type) {
    uint32_t timeout_iters = 10000u;

    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    while (timeout_iters > 0u) {
        const uint8_t status = mouse_ps2_read_status();

        if (type == 0u && (status & 1u) != 0u) {
            return;
        }

        if (type == 1u && (status & 2u) == 0u) {
            return;
        }

        udelay(10u);
        timeout_iters--;
    }
}

___inline void mouse_write_unlocked(uint8_t a) {
    mouse_wait_unlocked(1u);
    mouse_ps2_write_command(0xD4u);

    mouse_wait_unlocked(1u);
    mouse_ps2_write_data(a);
}

___inline uint8_t mouse_read_unlocked(void) {
    mouse_wait_unlocked(0u);

    return mouse_ps2_read_data();
}

void mouse_wait(uint8_t type) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    guard_spinlock_safe(&g_ps2_lock);

    mouse_wait_unlocked(type);
}

void mouse_write(uint8_t a) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    guard_spinlock_safe(&g_ps2_lock);

    mouse_write_unlocked(a);
}

uint8_t mouse_read(void) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return 0u;
    }

    guard_spinlock_safe(&g_ps2_lock);

    return mouse_read_unlocked();
}

void mouse_irq_handler(___unused registers_t* regs) {
    if (unlikely(!mouse_pmio_is_ready())) {
        return;
    }

    uint8_t data = 0u;

    int has_data = 0;

    {
        guard_spinlock_safe(&g_ps2_lock);

        const uint8_t status = mouse_ps2_read_status();

        if ((status & 0x01u) != 0u) {
            data = mouse_ps2_read_data();
            
            /* Verify the data is actually from the mouse */
            has_data = (status & 0x20u) != 0u;
        }
    }

    if (has_data) {
        if (likely(mouse_ring_push(data))) {
            if (likely(g_mouse_wq != NULL)) {
                queue_work(g_mouse_wq, &g_mouse_work);
            }
        }
    }
}

static void mouse_irq_handler_trampoline(registers_t* regs, ___unused void* ctx) {
    mouse_irq_handler(regs);
}

void mouse_init(void) {
    if (mouse_pmio_init() != 0) {
        return;
    }

    guard_spinlock_safe(&g_ps2_lock);

    mouse_wait_unlocked(1u);
    mouse_ps2_write_command(0xA8u);

    mouse_wait_unlocked(1u);
    mouse_ps2_write_command(0x20u);

    uint8_t status = mouse_read_unlocked() | 2u;
    status &= (uint8_t)~0x20u;

    mouse_wait_unlocked(1u);
    mouse_ps2_write_command(0x60u);

    mouse_wait_unlocked(1u);
    mouse_ps2_write_data(status);

    mouse_write_unlocked(0xF6u);
    (void)mouse_read_unlocked();

    mouse_write_unlocked(0xF4u);
    (void)mouse_read_unlocked();

    irq_install_handler(12, mouse_irq_handler_trampoline, NULL);

    (void)pic_unmask_irq(12u);
}

static int mouse_vfs_read(
    ___unused vfs_node_t* node,
    ___unused uint32_t offset,
    uint32_t size, void* buffer
) {
    if (unlikely(!buffer
        || size < sizeof(mouse_state_t))) {
        return -1;
    }

    task_t* curr = proc_current();

    if (!mouse_has_focus(curr)) {
        return 0;
    }

    mouse_state_t st;

    {
        guard_rwspin_read_safe(&g_mouse_state_lock);

        st.x = mouse_x;
        st.y = mouse_y;

        st.buttons = mouse_buttons;
    }

    atomic_uint_store_explicit(&g_mouse_dirty, 0u, ATOMIC_RELAXED);

    *((mouse_state_t*)buffer) = st;

    return (int)sizeof(mouse_state_t);
}

int mouse_poll_ready(task_t* task) {
    if (unlikely(!task)) {
        return 0;
    }

    if (!mouse_has_focus(task)) {
        return 0;
    }

    return atomic_uint_load_explicit(&g_mouse_dirty, ATOMIC_ACQUIRE) ? 1 : 0;
}

int mouse_poll_waitq_register(poll_waiter_t* w, task_t* task) {
    if (unlikely(!w
        || !task)) {
        return -1;
    }

    return poll_waitq_register(&g_mouse_poll_waitq, w, task);
}

static int mouse_vfs_poll_status(___unused vfs_node_t* node, int events) {
    if ((events & VFS_POLLIN) == 0) {
        return 0;
    }

    task_t* curr = proc_current();

    if (unlikely(!curr)) {
        return 0;
    }

    if (mouse_poll_ready(curr)) {
        return VFS_POLLIN;
    }

    return 0;
}

static int mouse_vfs_poll_register(___unused vfs_node_t* node, poll_waiter_t* w, task_t* task) {
    return mouse_poll_waitq_register(w, task);
}

void mouse_poll_notify_focus_change(void) {
    poll_waitq_wake_all(&g_mouse_poll_waitq, VFS_POLLIN);
}

static vfs_ops_t g_mouse_ops = {
    .write = 0, .open = 0,
    .close = 0, .ioctl = 0,
    .get_phys_page = 0,

    .read = mouse_vfs_read,
    .poll_status = mouse_vfs_poll_status,
    .poll_register = mouse_vfs_poll_register,
};

static vfs_node_t g_mouse_node = {
    .name = "mouse",
    .size = sizeof(mouse_state_t),
    .ops = &g_mouse_ops,

    .flags = 0u, .inode_idx = 0u,
    .refs = 0u, .fs_driver = NULL,
    .private_data = NULL,
    .private_retain = NULL,
    .private_release = NULL,
};

void mouse_vfs_init(void) {
    poll_waitq_init(&g_mouse_poll_waitq);
    
    devfs_register(&g_mouse_node);
}