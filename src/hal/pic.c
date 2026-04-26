/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/locking/spinlock.h>

#include "align.h"
#include "pmio.h"
#include "pic.h"

#define PIC_MASTER_PORT 0x21u
#define PIC_SLAVE_PORT  0xA1u
#define PIC_MASK_KEYBOARD (1u << 1)
#define PIC_MASK_CASCADE  (1u << 2)
#define PIC_MASK_MOUSE    (1u << 4)
#define PIC_MASK_HDD      (1u << 6)

static pmio_region_t* g_pic_master_region = 0;
static pmio_region_t* g_pic_slave_region = 0;

static __cacheline_aligned spinlock_t g_pic_lock;
static int g_pic_lock_inited = 0;

void pic_init(void) {
    if (!g_pic_lock_inited) {
        spinlock_init(&g_pic_lock);
        g_pic_lock_inited = 1;
    }

    if (!g_pic_master_region) {
        g_pic_master_region = pmio_request_region((uint16_t)PIC_MASTER_PORT, 1u, "pic_master_data");
    }

    if (!g_pic_slave_region) {
        g_pic_slave_region = pmio_request_region((uint16_t)PIC_SLAVE_PORT, 1u, "pic_slave_data");
    }
}

int pic_unmask_irq(uint8_t irq_line) {
    if (irq_line >= 16u) {
        return 0;
    }

    pic_init();

    if (!g_pic_master_region || !g_pic_slave_region) {
        return 0;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_pic_lock);

    if (irq_line < 8u) {
        uint8_t master_mask = 0u;

        pmio_acquire_bus(g_pic_master_region);

        if (pmio_readb(g_pic_master_region, 0u, &master_mask) != 0) {
            pmio_release_bus(g_pic_master_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        master_mask = (uint8_t)(master_mask & (uint8_t)~(1u << irq_line));

        if (pmio_writeb(g_pic_master_region, 0u, master_mask) != 0) {
            pmio_release_bus(g_pic_master_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        pmio_release_bus(g_pic_master_region);
        spinlock_release_safe(&g_pic_lock, flags);
        return 1;
    }

    const uint8_t slave_irq = (uint8_t)(irq_line - 8u);

    {
        uint8_t slave_mask = 0u;

        pmio_acquire_bus(g_pic_slave_region);

        if (pmio_readb(g_pic_slave_region, 0u, &slave_mask) != 0) {
            pmio_release_bus(g_pic_slave_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        slave_mask = (uint8_t)(slave_mask & (uint8_t)~(1u << slave_irq));

        if (pmio_writeb(g_pic_slave_region, 0u, slave_mask) != 0) {
            pmio_release_bus(g_pic_slave_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        pmio_release_bus(g_pic_slave_region);
    }

    {
        uint8_t master_mask = 0u;

        pmio_acquire_bus(g_pic_master_region);

        if (pmio_readb(g_pic_master_region, 0u, &master_mask) != 0) {
            pmio_release_bus(g_pic_master_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        master_mask = (uint8_t)(master_mask & (uint8_t)~(1u << 2u));

        if (pmio_writeb(g_pic_master_region, 0u, master_mask) != 0) {
            pmio_release_bus(g_pic_master_region);
            spinlock_release_safe(&g_pic_lock, flags);
            return 0;
        }

        pmio_release_bus(g_pic_master_region);
    }

    spinlock_release_safe(&g_pic_lock, flags);

    return 1;
}

void pic_configure_legacy(void) {
    pic_init();

    if (!g_pic_master_region || !g_pic_slave_region) {
        return;
    }

    pmio_acquire_bus(g_pic_master_region);
    (void)pmio_writeb(g_pic_master_region, 0u, (uint8_t)~(PIC_MASK_KEYBOARD | PIC_MASK_CASCADE));
    pmio_release_bus(g_pic_master_region);

    pmio_acquire_bus(g_pic_slave_region);
    (void)pmio_writeb(g_pic_slave_region, 0u, (uint8_t)~(PIC_MASK_MOUSE | PIC_MASK_HDD));
    pmio_release_bus(g_pic_slave_region);
}
