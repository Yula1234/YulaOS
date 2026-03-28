// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <arch/i386/paging.h>

#include <drivers/acpi.h>
#include <drivers/driver.h>
#include <drivers/pci/pci.h>
#include <drivers/virtio/virtio_core.h>

#include <mm/heap.h>

#include <hal/io.h>
#include <hal/ioapic.h>
#include <hal/irq.h>
#include <hal/lock.h>

#include <kernel/smp/cpu.h>

#include <lib/string.h>

#include <stddef.h>

#include "virtio_pci.h"
#include "virtqueue.h"

#define VIRTIO_PCI_CAP_ID 0x09u

#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_ISR_CFG    3u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

#define VIRTIO_PCI_NO_VECTOR 0xFFFFu

#define PCI_CAP_ID_MSIX 0x11u

typedef struct __attribute__((packed)) {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t id;
    uint8_t padding[2];
    uint32_t offset;
    uint32_t length;
} virtio_pci_cap_t;

typedef struct __attribute__((packed)) {
    virtio_pci_cap_t cap;
    uint32_t notify_off_multiplier;
} virtio_pci_notify_cap_t;

typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;

    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_avail;
    uint64_t queue_used;
} virtio_pci_common_cfg_t;

static void virtio_pci_dev_enable_mmio_master(pci_device_t* pci) {
    if (!pci) {
        return;
    }

    uint32_t cmdsts = pci_dev_read32(pci, 0x04);
    uint16_t cmd = (uint16_t)(cmdsts & 0xFFFFu);

    cmd |= 0x0006u;
    cmd &= (uint16_t)~0x0400u;

    pci_dev_write16(pci, 0x04, cmd);
}

static int virtio_pci_mmio_bar_base(const pci_device_t* pci, uint8_t bar_idx, uint32_t* out_base) {
    if (!pci || bar_idx >= 6u || !out_base) {
        return 0;
    }

    const pci_bar_t* b = &pci->bars[bar_idx];

    if (b->type != PCI_BAR_TYPE_MMIO || b->size == 0u) {
        return 0;
    }

    if (b->is_64bit == 0u) {
        if (b->base_addr == 0u) {
            return 0;
        }

        *out_base = b->base_addr;
        return 1;
    }

    if (bar_idx + 1u >= 6u) {
        return 0;
    }

    const uint8_t off = (uint8_t)(0x10u + bar_idx * 4u);
    const uint32_t hi = pci_dev_read32(pci, (uint8_t)(off + 4u));

    if (hi != 0u) {
        return 0;
    }

    const uint32_t lo = b->base_addr != 0u ? b->base_addr : (pci_dev_read32(pci, off) & ~0x0Fu);
    if (lo == 0u) {
        return 0;
    }

    *out_base = lo;
    return 1;
}

static void map_mmio_region_uc(uint32_t phys_base, uint32_t length) {
    if (length == 0) return;

    uint32_t start = phys_base & ~0xFFFu;
    uint32_t end = phys_base + length;
    end = (end + 0xFFFu) & ~0xFFFu;

    for (uint32_t p = start; p < end; p += 4096u) {
        paging_map(kernel_page_directory, p, p, 0x13u);
        if (p + 4096u < p) break;
    }

    __sync_synchronize();
}

static int virtio_pci_enable_msix_entry0(virtio_pci_dev_t* dev, uint8_t irq_vector) {
    if (!dev || !dev->pci) {
        return 0;
    }

    pci_device_t* pci = dev->pci;

    uint32_t cmdsts = pci_dev_read32(pci, 0x04);
    uint16_t status = (uint16_t)((cmdsts >> 16) & 0xFFFFu);
    if ((status & 0x0010u) == 0u) {
        return 0;
    }

    uint8_t cap = pci_dev_read8(pci, 0x34);
    for (int iter = 0; iter < 64 && cap != 0; iter++) {
        uint8_t cap_id = pci_dev_read8(pci, cap + 0);
        uint8_t cap_next = pci_dev_read8(pci, cap + 1);

        if (cap_id == PCI_CAP_ID_MSIX) {
            uint16_t msg_ctl = pci_dev_read16(pci, cap + 2);

            uint32_t table = pci_dev_read32(pci, cap + 4);
            uint8_t bir = (uint8_t)(table & 0x7u);
            uint32_t table_off = table & ~0x7u;

            uint32_t bar_base;
            if (!virtio_pci_mmio_bar_base(pci, bir, &bar_base)) {
                return 0;
            }

            map_mmio_region_uc(bar_base + table_off, 4096u);

            volatile uint32_t* entry = (volatile uint32_t*)(uintptr_t)(bar_base + table_off);

            uint8_t dest_apic_id = 0;
            if (cpu_count > 0 && cpus[0].id >= 0) {
                dest_apic_id = (uint8_t)cpus[0].id;
            }

            uint32_t msg_addr_lo = 0xFEE00000u | ((uint32_t)dest_apic_id << 12);
            uint32_t msg_addr_hi = 0u;
            uint32_t msg_data = (uint32_t)irq_vector;

            entry[0] = msg_addr_lo;
            entry[1] = msg_addr_hi;
            entry[2] = msg_data;
            entry[3] = 0u;

            __sync_synchronize();

            msg_ctl &= (uint16_t)~(1u << 14);
            msg_ctl |= (uint16_t)(1u << 15);
            pci_dev_write16(pci, cap + 2, msg_ctl);

            uint16_t command = (uint16_t)(cmdsts & 0xFFFFu);
            command |= (uint16_t)(1u << 10);
            pci_dev_write16(pci, 0x04, command);

            return 1;
        }

        cap = cap_next;
    }

    return 0;
}

static spinlock_t g_virtio_devs_lock;
static virtio_pci_dev_t* g_virtio_devs[8];
static uint32_t g_virtio_devs_count;
static volatile int g_virtio_global_inited;

static void virtio_pci_global_init_once(void) {
    if (__sync_bool_compare_and_swap(&g_virtio_global_inited, 0, 1)) {
        spinlock_init(&g_virtio_devs_lock);
        memset(g_virtio_devs, 0, sizeof(g_virtio_devs));
        g_virtio_devs_count = 0;
    }
}

static void virtio_pci_global_register_dev(virtio_pci_dev_t* dev) {
    if (!dev) return;

    virtio_pci_global_init_once();
    uint32_t iflags = spinlock_acquire_safe(&g_virtio_devs_lock);

    for (uint32_t i = 0; i < g_virtio_devs_count; i++) {
        if (g_virtio_devs[i] == dev) {
            spinlock_release_safe(&g_virtio_devs_lock, iflags);
            return;
        }
    }

    if (g_virtio_devs_count < (uint32_t)(sizeof(g_virtio_devs) / sizeof(g_virtio_devs[0]))) {
        g_virtio_devs[g_virtio_devs_count++] = dev;
    }

    spinlock_release_safe(&g_virtio_devs_lock, iflags);
}

static void virtio_pci_irq_handler_trampoline(registers_t* regs, void* ctx) {
    (void)ctx;
    virtio_pci_irq_handler(regs);
}

int virtio_pci_map_modern_caps(virtio_pci_dev_t* dev) {
    if (!dev || !dev->pci) {
        return 0;
    }

    virtio_pci_global_init_once();

    pci_device_t* pci = dev->pci;

    virtio_pci_dev_enable_mmio_master(pci);

    uint32_t cmdsts = pci_dev_read32(pci, 0x04);
    uint16_t st = (uint16_t)((cmdsts >> 16) & 0xFFFFu);
    if ((st & 0x0010u) == 0u) {
        return 0;
    }

    uint8_t cap = pci_dev_read8(pci, 0x34);
    for (int iter = 0; iter < 64 && cap != 0; iter++) {
        uint8_t cap_id = pci_dev_read8(pci, cap + 0);
        uint8_t cap_next = pci_dev_read8(pci, cap + 1);

        if (cap_id == VIRTIO_PCI_CAP_ID) {
            uint8_t cfg_type = pci_dev_read8(pci, cap + 3);
            uint8_t bar = pci_dev_read8(pci, cap + 4);
            uint32_t offset = pci_dev_read32(pci, cap + 8);
            uint32_t length = pci_dev_read32(pci, cap + 12);

            uint32_t bar_base;
            if (!virtio_pci_mmio_bar_base(pci, bar, &bar_base)) {
                cap = cap_next;
                continue;
            }

            uint32_t phys = bar_base + offset;
            map_mmio_region_uc(phys, length);

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                dev->common_cfg = (volatile void*)phys;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                dev->notify_base = (volatile void*)phys;
                dev->notify_off_multiplier = pci_dev_read32(pci, cap + 16);
            } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                dev->isr_cfg = (volatile uint8_t*)phys;
            } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                dev->device_cfg = (volatile void*)phys;
            }
        }

        cap = cap_next;
    }

    if (!dev->common_cfg || !dev->notify_base || !dev->isr_cfg) {
        return 0;
    }

    return 1;
}

void virtio_pci_reset(virtio_pci_dev_t* dev) {
    if (!dev || !dev->common_cfg) return;
    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;
    c->device_status = 0;
    __sync_synchronize();
}

void virtio_pci_set_status(virtio_pci_dev_t* dev, uint8_t status) {
    if (!dev || !dev->common_cfg) return;
    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;
    c->device_status = status;
    __sync_synchronize();
}

void virtio_pci_add_status(virtio_pci_dev_t* dev, uint8_t status_bits) {
    if (!dev || !dev->common_cfg) return;
    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;
    uint8_t s = c->device_status;
    c->device_status = (uint8_t)(s | status_bits);
    __sync_synchronize();
}

uint64_t virtio_pci_read_device_features(virtio_pci_dev_t* dev) {
    if (!dev || !dev->common_cfg) return 0;

    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;

    c->device_feature_select = 0;
    __sync_synchronize();
    uint32_t lo = c->device_feature;

    c->device_feature_select = 1;
    __sync_synchronize();
    uint32_t hi = c->device_feature;

    return ((uint64_t)hi << 32) | lo;
}

void virtio_pci_write_driver_features(virtio_pci_dev_t* dev, uint64_t features) {
    if (!dev || !dev->common_cfg) return;

    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;

    c->driver_feature_select = 0;
    __sync_synchronize();
    c->driver_feature = (uint32_t)(features & 0xFFFFFFFFu);

    c->driver_feature_select = 1;
    __sync_synchronize();
    c->driver_feature = (uint32_t)((features >> 32) & 0xFFFFFFFFu);

    __sync_synchronize();
}

int virtio_pci_negotiate_features(virtio_pci_dev_t* dev, uint64_t wanted_features, uint64_t* out_accepted_features) {
    if (!dev || !dev->common_cfg) return 0;

    virtio_pci_add_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_pci_add_status(dev, VIRTIO_STATUS_DRIVER);

    uint64_t device = virtio_pci_read_device_features(dev);
    uint64_t accepted = device & wanted_features;

    if ((accepted & VIRTIO_F_VERSION_1) == 0u) {
        virtio_pci_add_status(dev, VIRTIO_STATUS_FAILED);
        return 0;
    }

    virtio_pci_write_driver_features(dev, accepted);

    virtio_pci_add_status(dev, VIRTIO_STATUS_FEATURES_OK);

    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;
    uint8_t s = c->device_status;
    if ((s & VIRTIO_STATUS_FEATURES_OK) == 0u) {
        virtio_pci_add_status(dev, VIRTIO_STATUS_FAILED);
        return 0;
    }

    if (out_accepted_features) {
        *out_accepted_features = accepted;
    }

    return 1;
}

int virtio_pci_queue_init(virtio_pci_dev_t* dev, struct virtqueue* out_vq, uint16_t queue_index, uint16_t requested_size) {
    if (!dev || !dev->common_cfg || !dev->notify_base || !out_vq) return 0;

    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;

    c->queue_select = queue_index;
    __sync_synchronize();

    uint16_t max_size = c->queue_size;
    if (max_size == 0) {
        return 0;
    }

    uint16_t qsz = max_size;
    if (requested_size != 0 && requested_size < qsz) {
        qsz = requested_size;
    }

    c->queue_size = qsz;
    __sync_synchronize();

    uint16_t notify_off = c->queue_notify_off;

    uintptr_t notify_addr = (uintptr_t)dev->notify_base + (uintptr_t)notify_off * (uintptr_t)dev->notify_off_multiplier;

    if (!virtqueue_init((virtqueue_t*)out_vq, queue_index, qsz, (volatile uint16_t*)notify_addr)) {
        return 0;
    }

    uint32_t desc_phys = paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)((virtqueue_t*)out_vq)->desc);
    uint32_t avail_phys = paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)((virtqueue_t*)out_vq)->avail);
    uint32_t used_phys = paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)((virtqueue_t*)out_vq)->used);

    volatile uint8_t* cfg = (volatile uint8_t*)c;

    volatile uint32_t* qd = (volatile uint32_t*)(cfg + offsetof(virtio_pci_common_cfg_t, queue_desc));
    qd[0] = desc_phys;
    __sync_synchronize();
    qd[1] = 0u;
    __sync_synchronize();

    volatile uint32_t* qa = (volatile uint32_t*)(cfg + offsetof(virtio_pci_common_cfg_t, queue_avail));
    qa[0] = avail_phys;
    __sync_synchronize();
    qa[1] = 0u;
    __sync_synchronize();

    volatile uint32_t* qu = (volatile uint32_t*)(cfg + offsetof(virtio_pci_common_cfg_t, queue_used));
    qu[0] = used_phys;
    __sync_synchronize();
    qu[1] = 0u;
    __sync_synchronize();

    if (dev->msi_enabled) {
        c->queue_msix_vector = 0;
    } else {
        c->queue_msix_vector = VIRTIO_PCI_NO_VECTOR;
    }

    __sync_synchronize();
    c->queue_enable = 1;
    __sync_synchronize();

    virtio_pci_register_queue(dev, (virtqueue_t*)out_vq);

    return 1;
}

int virtio_pci_enable_msi(virtio_pci_dev_t* dev, uint8_t vector) {
    if (!dev) return 0;

    virtio_pci_global_register_dev(dev);

    if (!virtio_pci_enable_msix_entry0(dev, vector)) {
        dev->msi_enabled = 0;
        dev->msi_vector = 0;
        return 0;
    }

    volatile virtio_pci_common_cfg_t* c = (volatile virtio_pci_common_cfg_t*)dev->common_cfg;
    c->msix_config = 0;
    __sync_synchronize();

    irq_install_vector_handler(vector, virtio_pci_irq_handler_trampoline, 0);
    dev->msi_enabled = 1;
    dev->msi_vector = vector;
    return 1;
}

static void virtio_pci_intx_trampoline(registers_t* regs, void* ctx) {
    void (*handler)(registers_t*) = (void (*)(registers_t*))ctx;

    if (handler) {
        handler(regs);
    }
}

int virtio_pci_enable_intx(virtio_pci_dev_t* dev, void (*handler)(registers_t*)) {
    if (!dev || !handler || !dev->pci) return 0;

    virtio_pci_global_register_dev(dev);

    uint8_t irq_line = dev->pci->irq_line;

    irq_install_handler(irq_line, virtio_pci_intx_trampoline, (void*)handler);

    if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
        uint32_t gsi;
        int active_low;
        int level_trigger;
        if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
            gsi = (uint32_t)irq_line;
            active_low = 1;
            level_trigger = 1;
        }
        ioapic_route_gsi(gsi, (uint8_t)(32 + irq_line), (uint8_t)cpus[0].id, active_low, level_trigger);
    } else {
        if (irq_line < 8) outb(0x21, inb(0x21) & (uint8_t)~(1u << irq_line));
        else {
            outb(0xA1, inb(0xA1) & (uint8_t)~(1u << (irq_line - 8)));
            outb(0x21, inb(0x21) & (uint8_t)~(1u << 2));
        }
    }

    dev->msi_enabled = 0;
    return 1;
}

int virtio_pci_register_queue(virtio_pci_dev_t* dev, struct virtqueue* vq) {
    if (!dev || !vq) return 0;

    if (dev->queue_count >= (uint32_t)(sizeof(dev->queues) / sizeof(dev->queues[0]))) {
        return 0;
    }

    dev->queues[dev->queue_count++] = vq;
    return 1;
}

void virtio_pci_irq_handler(registers_t* regs) {
    (void)regs;

    virtio_pci_global_init_once();
    uint32_t iflags = spinlock_acquire_safe(&g_virtio_devs_lock);

    for (uint32_t i = 0; i < g_virtio_devs_count; i++) {
        virtio_pci_dev_t* dev = g_virtio_devs[i];
        if (!dev || !dev->isr_cfg) continue;

        uint8_t isr = *dev->isr_cfg;
        if ((isr & 0x1u) == 0u) {
            continue;
        }

        for (uint32_t q = 0; q < dev->queue_count; q++) {
            virtqueue_t* vq = (virtqueue_t*)dev->queues[q];
            if (vq) {
                virtqueue_handle_irq(vq);
            }
        }
    }

    spinlock_release_safe(&g_virtio_devs_lock, iflags);
}

static uint16_t virtio_pci_virtio_id_from_pci_device_id(uint16_t pci_did) {
    if (pci_did >= 0x1040u && pci_did <= 0x107Fu) {
        return (uint16_t)(pci_did - 0x1040u);
    }

    if (pci_did >= 0x1000u && pci_did < 0x1040u) {
        return (uint16_t)(pci_did - 0x1000u);
    }

    return 0;
}

static virtio_pci_dev_t* virtio_pci_dev_from_vdev(virtio_device_t* vdev) {
    if (!vdev) {
        return 0;
    }

    return (virtio_pci_dev_t*)vdev->transport_data;
}

static void virtio_pci_ops_reset(virtio_device_t* vdev) {
    virtio_pci_reset(virtio_pci_dev_from_vdev(vdev));
}

static void virtio_pci_ops_add_status(virtio_device_t* vdev, uint8_t status_bits) {
    virtio_pci_add_status(virtio_pci_dev_from_vdev(vdev), status_bits);
}

static uint64_t virtio_pci_ops_read_device_features(virtio_device_t* vdev) {
    return virtio_pci_read_device_features(virtio_pci_dev_from_vdev(vdev));
}

static void virtio_pci_ops_write_driver_features(virtio_device_t* vdev, uint64_t features) {
    virtio_pci_write_driver_features(virtio_pci_dev_from_vdev(vdev), features);
}

static int virtio_pci_ops_negotiate_features(virtio_device_t* vdev, uint64_t wanted, uint64_t* out_accepted) {
    return virtio_pci_negotiate_features(virtio_pci_dev_from_vdev(vdev), wanted, out_accepted);
}

static int virtio_pci_ops_setup_queue(
    virtio_device_t* vdev,
    uint16_t queue_index,
    struct virtqueue* vq,
    uint16_t requested_size
) {
    return virtio_pci_queue_init(virtio_pci_dev_from_vdev(vdev), vq, queue_index, requested_size);
}

static int virtio_pci_ops_enable_msi(virtio_device_t* vdev, uint8_t vector) {
    return virtio_pci_enable_msi(virtio_pci_dev_from_vdev(vdev), vector);
}

static int virtio_pci_ops_enable_intx(virtio_device_t* vdev, void (*handler)(registers_t* regs)) {
    return virtio_pci_enable_intx(virtio_pci_dev_from_vdev(vdev), handler);
}

static uint8_t virtio_pci_ops_read_config8(virtio_device_t* vdev, uint32_t offset) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    if (!p || !p->device_cfg) {
        return 0;
    }

    return *(volatile uint8_t*)((uint8_t*)p->device_cfg + offset);
}

static uint16_t virtio_pci_ops_read_config16(virtio_device_t* vdev, uint32_t offset) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    if (!p || !p->device_cfg) {
        return 0;
    }

    return *(volatile uint16_t*)((uint8_t*)p->device_cfg + offset);
}

static uint32_t virtio_pci_ops_read_config32(virtio_device_t* vdev, uint32_t offset) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    if (!p || !p->device_cfg) {
        return 0;
    }

    return *(volatile uint32_t*)((uint8_t*)p->device_cfg + offset);
}

static void virtio_pci_ops_write_config32(virtio_device_t* vdev, uint32_t offset, uint32_t val) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    if (!p || !p->device_cfg) {
        return;
    }

    *(volatile uint32_t*)((uint8_t*)p->device_cfg + offset) = val;
}

static void virtio_pci_ops_notify(virtio_device_t* vdev, uint16_t queue_index) {
    (void)vdev;
    (void)queue_index;
}

static const virtio_ops_t virtio_pci_ops = {
    .reset = virtio_pci_ops_reset,
    .add_status = virtio_pci_ops_add_status,
    .read_device_features = virtio_pci_ops_read_device_features,
    .write_driver_features = virtio_pci_ops_write_driver_features,
    .negotiate_features = virtio_pci_ops_negotiate_features,
    .setup_queue = virtio_pci_ops_setup_queue,
    .enable_msi = virtio_pci_ops_enable_msi,
    .enable_intx = virtio_pci_ops_enable_intx,
    .read_config8 = virtio_pci_ops_read_config8,
    .read_config16 = virtio_pci_ops_read_config16,
    .read_config32 = virtio_pci_ops_read_config32,
    .write_config32 = virtio_pci_ops_write_config32,
    .notify = virtio_pci_ops_notify,
};

static int virtio_pci_probe(pci_device_t* pdev) {
    if (!pdev) {
        return -1;
    }

    virtio_pci_dev_t* p = kmalloc(sizeof(*p));
    if (!p) {
        return -1;
    }

    memset(p, 0, sizeof(*p));

    p->pci = pdev;

    if (!virtio_pci_map_modern_caps(p)) {
        kfree(p);
        return -1;
    }

    const uint16_t virtio_id = virtio_pci_virtio_id_from_pci_device_id(pdev->device_id);

    virtio_device_t* v = kmalloc(sizeof(*v));
    if (!v) {
        kfree(p);
        return -1;
    }

    memset(v, 0, sizeof(*v));
    v->transport_data = p;
    v->ops = &virtio_pci_ops;
    v->virtio_dev_id = virtio_id;
    v->dev.name = "virtio";
    v->dev.driver = 0;

    const int rc = virtio_register_device(v);
    if (rc != 0) {
        kfree(v);
        kfree(p);
        return -1;
    }

    return 0;
}

static const pci_device_id_t g_virtio_pci_ids[] = {
    {
        .match_flags = PCI_MATCH_VENDOR_ID | PCI_MATCH_DEVICE_ID_RANGE,
        .vendor_id = 0x1AF4u,
        .device_id = 0x1000u,
        .device_id_last = 0x107Fu,
        .class_code = 0u,
        .subclass = 0u,
        .prog_if = 0u,
    },
    { .match_flags = 0u },
};

static pci_driver_t g_virtio_pci_driver = {
    .base = {
        .name = "virtio-pci",
        .klass = DRIVER_CLASS_PSEUDO,
        .stage = DRIVER_STAGE_CORE,
        .init = 0,
        .shutdown = 0,
    },
    .id_table = g_virtio_pci_ids,
    .probe = virtio_pci_probe,
    .remove = 0,
};

extern void virtio_gpu_register_driver(void);

static int virtio_pci_transport_init(void) {
    virtio_gpu_register_driver();
    return pci_register_driver(&g_virtio_pci_driver);
}

DRIVER_REGISTER(
    .name = "virtio-pci",
    .klass = DRIVER_CLASS_PSEUDO,
    .stage = DRIVER_STAGE_CORE,
    .init = virtio_pci_transport_init,
    .shutdown = 0
);
