// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <arch/i386/paging.h>

#include <drivers/acpi.h>
#include <drivers/pci.h>

#include <hal/io.h>
#include <hal/ioapic.h>
#include <hal/irq.h>
#include <hal/lock.h>

#include <kernel/cpu.h>

#include <lib/string.h>

#include "virtio_pci.h"
#include "virtqueue.h"

#define VIRTIO_PCI_CAP_ID 0x09u

#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_ISR_CFG    3u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

#define VIRTIO_PCI_NO_VECTOR 0xFFFFu

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

static inline uint8_t pci_read8_local(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFC);
    return (uint8_t)((reg >> ((offset & 3u) * 8u)) & 0xFFu);
}

static inline uint16_t pci_read16_local(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFC);
    return (uint16_t)((reg >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static inline void pci_write16_local(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint8_t aligned = offset & 0xFC;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    uint32_t shift = (uint32_t)(offset & 2u) * 8u;
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    pci_write(bus, slot, func, aligned, reg);
}

static void pci_enable_mem_busmaster(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t cmdsts = pci_read(bus, slot, func, 0x04);
    uint16_t cmd = (uint16_t)(cmdsts & 0xFFFFu);

    cmd |= 0x0002u;   // Memory Space
    cmd |= 0x0004u;   // Bus Master
    cmd &= (uint16_t)~0x0400u; // Interrupt Disable

    pci_write16_local(bus, slot, func, 0x04, cmd);
}

static int pci_get_bar32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index, uint32_t* out_base) {
    if (!out_base || bar_index >= 6) return 0;

    uint8_t off = (uint8_t)(0x10u + (uint8_t)(bar_index * 4u));
    uint32_t lo = pci_read(bus, slot, func, off);

    if (lo & 1u) {
        return 0;
    }

    uint32_t type = (lo >> 1) & 3u;
    if (type == 2u) {
        if (bar_index == 5) return 0;
        uint32_t hi = pci_read(bus, slot, func, (uint8_t)(off + 4u));
        if (hi != 0) {
            return 0;
        }
    }

    *out_base = lo & ~0xFu;
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

int virtio_pci_find_device(uint16_t vendor_id, uint16_t device_id, virtio_pci_dev_t* out_dev) {
    if (!out_dev) return 0;

    memset(out_dev, 0, sizeof(*out_dev));

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                if (vendor == 0xFFFFu) continue;

                uint16_t dev_id = (uint16_t)((id >> 16) & 0xFFFFu);
                if (vendor != vendor_id || dev_id != device_id) continue;

                uint32_t irq_info = pci_read((uint8_t)bus, slot, func, 0x3C);

                out_dev->bus = (uint8_t)bus;
                out_dev->slot = slot;
                out_dev->func = func;
                out_dev->vendor_id = vendor;
                out_dev->device_id = dev_id;
                out_dev->irq_line = (uint8_t)(irq_info & 0xFFu);
                out_dev->msi_enabled = 0;
                out_dev->queue_count = 0;

                return 1;
            }
        }
    }

    return 0;
}

int virtio_pci_map_modern_caps(virtio_pci_dev_t* dev) {
    if (!dev) return 0;

    virtio_pci_global_init_once();

    pci_enable_mem_busmaster(dev->bus, dev->slot, dev->func);

    uint32_t cmdsts = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    uint16_t status = (uint16_t)((cmdsts >> 16) & 0xFFFFu);
    if ((status & 0x0010u) == 0u) {
        return 0;
    }

    uint8_t cap = pci_read8_local(dev->bus, dev->slot, dev->func, 0x34);
    for (int iter = 0; iter < 64 && cap != 0; iter++) {
        uint8_t cap_id = pci_read8_local(dev->bus, dev->slot, dev->func, cap + 0);
        uint8_t cap_next = pci_read8_local(dev->bus, dev->slot, dev->func, cap + 1);

        if (cap_id == VIRTIO_PCI_CAP_ID) {
            uint8_t cfg_type = pci_read8_local(dev->bus, dev->slot, dev->func, cap + 3);
            uint8_t bar = pci_read8_local(dev->bus, dev->slot, dev->func, cap + 4);
            uint32_t offset = pci_read(dev->bus, dev->slot, dev->func, cap + 8);
            uint32_t length = pci_read(dev->bus, dev->slot, dev->func, cap + 12);

            uint32_t bar_base;
            if (!pci_get_bar32(dev->bus, dev->slot, dev->func, bar, &bar_base)) {
                cap = cap_next;
                continue;
            }

            uint32_t phys = bar_base + offset;
            map_mmio_region_uc(phys, length);

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                dev->common_cfg = (volatile void*)phys;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                dev->notify_base = (volatile void*)phys;
                dev->notify_off_multiplier = pci_read(dev->bus, dev->slot, dev->func, cap + 16);
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

    c->queue_desc = (uint64_t)desc_phys;
    c->queue_avail = (uint64_t)avail_phys;
    c->queue_used = (uint64_t)used_phys;
    c->queue_msix_vector = VIRTIO_PCI_NO_VECTOR;

    __sync_synchronize();
    c->queue_enable = 1;
    __sync_synchronize();

    virtio_pci_register_queue(dev, (virtqueue_t*)out_vq);

    return 1;
}

int virtio_pci_enable_msi(virtio_pci_dev_t* dev, uint8_t vector) {
    if (!dev) return 0;

    virtio_pci_global_register_dev(dev);

    int msi_ok = 0;
    if (cpu_count > 0 && cpus[0].id >= 0) {
        msi_ok = pci_msi_configure(dev->bus, dev->slot, dev->func, vector, (uint8_t)cpus[0].id);
    }

    if (!msi_ok) {
        dev->msi_enabled = 0;
        return 0;
    }

    irq_install_vector_handler(vector, virtio_pci_irq_handler);
    dev->msi_enabled = 1;
    return 1;
}

int virtio_pci_enable_intx(virtio_pci_dev_t* dev, void (*handler)(registers_t*)) {
    if (!dev || !handler) return 0;

    virtio_pci_global_register_dev(dev);

    uint8_t irq_line = dev->irq_line;

    irq_install_handler(irq_line, handler);

    if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
        uint32_t gsi;
        int active_low;
        int level_trigger;
        if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
            gsi = (uint32_t)irq_line;
            active_low = 0;
            level_trigger = 0;
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
