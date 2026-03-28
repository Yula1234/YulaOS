// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/acpi.h>
#include <drivers/driver.h>
#include <drivers/pci/pci.h>
#include <drivers/virtio/core.h>

#include <mm/dma/api.h>
#include <mm/heap.h>
#include <mm/iomem.h>

#include <hal/io.h>
#include <hal/ioapic.h>
#include <hal/irq.h>
#include <hal/lock.h>

#include <kernel/smp/cpu.h>

#include <lib/dlist.h>
#include <lib/string.h>

#include <stddef.h>

#include "pci.h"
#include "virtqueue.h"

#define VIRTIO_PCI_CAP_ID 0x09u

#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_ISR_CFG    3u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

#define VIRTIO_PCI_NO_VECTOR 0xFFFFu

/* Offsets within virtio PCI common config (virtio 1.0). */
#define VPCI_COMMON_DEVICE_FEAT_SEL    0u
#define VPCI_COMMON_DEVICE_FEAT        4u
#define VPCI_COMMON_DRIVER_FEAT_SEL    8u
#define VPCI_COMMON_DRIVER_FEAT       12u
#define VPCI_COMMON_MSIX_CONFIG       16u
#define VPCI_COMMON_NUM_QUEUES        18u
#define VPCI_COMMON_DEVICE_STATUS     20u
#define VPCI_COMMON_CONFIG_GENERATION 21u
#define VPCI_COMMON_QUEUE_SELECT      22u
#define VPCI_COMMON_QUEUE_SIZE        24u
#define VPCI_COMMON_QUEUE_MSIX_VEC    26u
#define VPCI_COMMON_QUEUE_ENABLE      28u
#define VPCI_COMMON_QUEUE_NOTIFY_OFF  30u
#define VPCI_COMMON_QUEUE_DESC        32u
#define VPCI_COMMON_QUEUE_AVAIL       40u
#define VPCI_COMMON_QUEUE_USED        48u

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

static void virtio_pci_free_all_bars(virtio_pci_dev_t* dev) {
    if (!dev) {
        return;
    }

    for (uint32_t i = 0; i < 6u; i++) {
        if (dev->bar_iomem[i]) {
            iomem_free(dev->bar_iomem[i]);
            dev->bar_iomem[i] = 0;
        }
    }
}

static __iomem* vpci_bar_io(virtio_pci_dev_t* dev, uint8_t bar) {
    if (!dev || !dev->pci || bar >= 6u) {
        return 0;
    }

    if (dev->bar_iomem[bar]) {
        return dev->bar_iomem[bar];
    }

    dev->bar_iomem[bar] = pci_request_bar(dev->pci, bar, "virtio_pci");
    return dev->bar_iomem[bar];
}

static __iomem* vpci_common_io(virtio_pci_dev_t* dev) {
    if (!dev || dev->common_bar >= VPCI_NO_CAP_BAR) {
        return 0;
    }

    return vpci_bar_io(dev, dev->common_bar);
}

static __iomem* vpci_device_cfg_io(virtio_pci_dev_t* dev) {
    if (!dev || dev->device_cfg_bar >= VPCI_NO_CAP_BAR) {
        return 0;
    }

    return vpci_bar_io(dev, dev->device_cfg_bar);
}

static void virtio_pci_irq_bh(work_struct_t* work) {
    virtio_pci_dev_t* dev = container_of(work, virtio_pci_dev_t, irq_work);
    if (!dev) {
        return;
    }

    for (uint32_t q = 0; q < dev->queue_count; q++) {
        virtqueue_t* vq = (virtqueue_t*)dev->queues[q];
        if (vq) {
            virtqueue_handle_irq(vq);
        }
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

    dev->common_bar = VPCI_NO_CAP_BAR;
    dev->notify_bar = VPCI_NO_CAP_BAR;
    dev->isr_bar = VPCI_NO_CAP_BAR;
    dev->device_cfg_bar = VPCI_NO_CAP_BAR;
    dev->common_off = 0;
    dev->notify_off = 0;
    dev->isr_off = 0;
    dev->device_cfg_off = 0;
    dev->notify_off_multiplier = 0;

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

            if (bar >= 6u) {
                cap = cap_next;
                continue;
            }

            const pci_bar_t* pb = &pci->bars[bar];
            if (pb->type != PCI_BAR_TYPE_MMIO || pb->size == 0u) {
                cap = cap_next;
                continue;
            }

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                dev->common_bar = bar;
                dev->common_off = offset;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                dev->notify_bar = bar;
                dev->notify_off = offset;
                dev->notify_off_multiplier = pci_dev_read32(pci, cap + 16);
            } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                dev->isr_bar = bar;
                dev->isr_off = offset;
            } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                dev->device_cfg_bar = bar;
                dev->device_cfg_off = offset;
            }
        }

        cap = cap_next;
    }

    if (dev->common_bar >= VPCI_NO_CAP_BAR || dev->notify_bar >= VPCI_NO_CAP_BAR || dev->isr_bar >= VPCI_NO_CAP_BAR) {
        virtio_pci_free_all_bars(dev);
        return 0;
    }

    if (!vpci_bar_io(dev, dev->common_bar) || !vpci_bar_io(dev, dev->notify_bar) || !vpci_bar_io(dev, dev->isr_bar)) {
        virtio_pci_free_all_bars(dev);
        return 0;
    }

    return 1;
}

void virtio_pci_reset(virtio_pci_dev_t* dev) {
    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return;
    }

    iowrite8(io, dev->common_off + VPCI_COMMON_DEVICE_STATUS, 0);
    __sync_synchronize();
}

void virtio_pci_set_status(virtio_pci_dev_t* dev, uint8_t status) {
    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return;
    }

    iowrite8(io, dev->common_off + VPCI_COMMON_DEVICE_STATUS, status);
    __sync_synchronize();
}

void virtio_pci_add_status(virtio_pci_dev_t* dev, uint8_t status_bits) {
    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return;
    }

    uint32_t co = dev->common_off;
    uint8_t s = ioread8(io, co + VPCI_COMMON_DEVICE_STATUS);
    iowrite8(io, co + VPCI_COMMON_DEVICE_STATUS, (uint8_t)(s | status_bits));
    __sync_synchronize();
}

uint64_t virtio_pci_read_device_features(virtio_pci_dev_t* dev) {
    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return 0;
    }

    uint32_t co = dev->common_off;

    iowrite32(io, co + VPCI_COMMON_DEVICE_FEAT_SEL, 0u);
    __sync_synchronize();
    uint32_t lo = ioread32(io, co + VPCI_COMMON_DEVICE_FEAT);

    iowrite32(io, co + VPCI_COMMON_DEVICE_FEAT_SEL, 1u);
    __sync_synchronize();
    uint32_t hi = ioread32(io, co + VPCI_COMMON_DEVICE_FEAT);

    return ((uint64_t)hi << 32) | lo;
}

void virtio_pci_write_driver_features(virtio_pci_dev_t* dev, uint64_t features) {
    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return;
    }

    uint32_t co = dev->common_off;

    iowrite32(io, co + VPCI_COMMON_DRIVER_FEAT_SEL, 0u);
    __sync_synchronize();
    iowrite32(io, co + VPCI_COMMON_DRIVER_FEAT, (uint32_t)(features & 0xFFFFFFFFu));

    iowrite32(io, co + VPCI_COMMON_DRIVER_FEAT_SEL, 1u);
    __sync_synchronize();
    iowrite32(io, co + VPCI_COMMON_DRIVER_FEAT, (uint32_t)((features >> 32) & 0xFFFFFFFFu));

    __sync_synchronize();
}

int virtio_pci_negotiate_features(virtio_pci_dev_t* dev, uint64_t wanted_features, uint64_t* out_accepted_features) {
    if (!vpci_common_io(dev)) {
        return 0;
    }

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

    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return 0;
    }

    uint8_t s = ioread8(io, dev->common_off + VPCI_COMMON_DEVICE_STATUS);
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
    __iomem* io = vpci_common_io(dev);
    __iomem* nio = (!dev || dev->notify_bar >= VPCI_NO_CAP_BAR) ? 0 : vpci_bar_io(dev, dev->notify_bar);

    if (!io || !nio || !out_vq) {
        return 0;
    }

    uint32_t co = dev->common_off;

    iowrite16(io, co + VPCI_COMMON_QUEUE_SELECT, queue_index);
    __sync_synchronize();

    uint16_t max_size = ioread16(io, co + VPCI_COMMON_QUEUE_SIZE);
    if (max_size == 0) {
        return 0;
    }

    uint16_t qsz = max_size;
    if (requested_size != 0 && requested_size < qsz) {
        qsz = requested_size;
    }

    iowrite16(io, co + VPCI_COMMON_QUEUE_SIZE, qsz);
    __sync_synchronize();

    uint16_t notify_idx = ioread16(io, co + VPCI_COMMON_QUEUE_NOTIFY_OFF);
    uint32_t notify_word_off = dev->notify_off + (uint32_t)notify_idx * dev->notify_off_multiplier;

    if (!virtqueue_init((virtqueue_t*)out_vq, queue_index, qsz, nio, notify_word_off)) {
        return 0;
    }

    virtqueue_t* vq = (virtqueue_t*)out_vq;
    uint32_t desc_phys = dma_virt_to_phys((void*)vq->desc);
    uint32_t avail_phys = dma_virt_to_phys((void*)vq->avail);
    uint32_t used_phys = dma_virt_to_phys((void*)vq->used);

    iowrite32(io, co + VPCI_COMMON_QUEUE_DESC, desc_phys);
    __sync_synchronize();
    iowrite32(io, co + VPCI_COMMON_QUEUE_DESC + 4u, 0u);
    __sync_synchronize();

    iowrite32(io, co + VPCI_COMMON_QUEUE_AVAIL, avail_phys);
    __sync_synchronize();
    iowrite32(io, co + VPCI_COMMON_QUEUE_AVAIL + 4u, 0u);
    __sync_synchronize();

    iowrite32(io, co + VPCI_COMMON_QUEUE_USED, used_phys);
    __sync_synchronize();
    iowrite32(io, co + VPCI_COMMON_QUEUE_USED + 4u, 0u);
    __sync_synchronize();

    if (dev->msi_enabled) {
        iowrite16(io, co + VPCI_COMMON_QUEUE_MSIX_VEC, 0);
    } else {
        iowrite16(io, co + VPCI_COMMON_QUEUE_MSIX_VEC, VIRTIO_PCI_NO_VECTOR);
    }

    __sync_synchronize();
    iowrite16(io, co + VPCI_COMMON_QUEUE_ENABLE, 1);
    __sync_synchronize();

    virtio_pci_register_queue(dev, (virtqueue_t*)out_vq);

    return 1;
}

int virtio_pci_enable_msi(virtio_pci_dev_t* dev, uint8_t vector) {
    if (!dev) {
        return 0;
    }

    virtio_pci_global_register_dev(dev);

    uint8_t dest_apic_id = 0;
    if (cpu_count > 0 && cpus[0].id >= 0) {
        dest_apic_id = (uint8_t)cpus[0].id;
    }

    if (!pci_dev_enable_msix(dev->pci, 0, vector, dest_apic_id)) {
        dev->msi_enabled = 0;
        dev->msi_vector = 0;
        return 0;
    }

    __iomem* io = vpci_common_io(dev);
    if (!io) {
        return 0;
    }

    iowrite16(io, dev->common_off + VPCI_COMMON_MSIX_CONFIG, 0);
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
        if (!dev || dev->isr_bar >= VPCI_NO_CAP_BAR) continue;

        __iomem* isr_io = vpci_bar_io(dev, dev->isr_bar);
        if (!isr_io) continue;

        uint8_t isr = ioread8(isr_io, dev->isr_off);
        if ((isr & 0x1u) == 0u) {
            continue;
        }

        if (dev->wq) {
            queue_work(dev->wq, &dev->irq_work);
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
    __iomem* dio = vpci_device_cfg_io(p);

    if (!dio) {
        return 0;
    }

    return ioread8(dio, p->device_cfg_off + offset);
}

static uint16_t virtio_pci_ops_read_config16(virtio_device_t* vdev, uint32_t offset) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    __iomem* dio = vpci_device_cfg_io(p);

    if (!dio) {
        return 0;
    }

    return ioread16(dio, p->device_cfg_off + offset);
}

static uint32_t virtio_pci_ops_read_config32(virtio_device_t* vdev, uint32_t offset) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    __iomem* dio = vpci_device_cfg_io(p);

    if (!dio) {
        return 0;
    }

    return ioread32(dio, p->device_cfg_off + offset);
}

static void virtio_pci_ops_write_config32(virtio_device_t* vdev, uint32_t offset, uint32_t val) {
    virtio_pci_dev_t* p = virtio_pci_dev_from_vdev(vdev);
    __iomem* dio = vpci_device_cfg_io(p);

    if (!dio) {
        return;
    }

    iowrite32(dio, p->device_cfg_off + offset, val);
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

    virtio_pci_dev_t* p = kzalloc(sizeof(*p));
    if (!p) {
        return -1;
    }

    memset(p, 0, sizeof(*p));

    p->pci = pdev;

    init_work(&p->irq_work, virtio_pci_irq_bh);

    p->wq = create_workqueue("virtiopci");
    if (!p->wq) {
        kfree(p);
        return -1;
    }

    if (!virtio_pci_map_modern_caps(p)) {
        destroy_workqueue(p->wq);
        p->wq = 0;
        kfree(p);
        return -1;
    }

    const uint16_t virtio_id = virtio_pci_virtio_id_from_pci_device_id(pdev->device_id);

    virtio_device_t* v = kmalloc(sizeof(*v));
    if (!v) {
        virtio_pci_free_all_bars(p);

        destroy_workqueue(p->wq);
        p->wq = 0;

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
        virtio_pci_free_all_bars(p);

        destroy_workqueue(p->wq);
        p->wq = 0;

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
