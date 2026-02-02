// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_VIRTIO_PCI_H
#define DRIVERS_VIRTIO_PCI_H

#include <arch/i386/idt.h>

#include <stdint.h>

struct virtqueue;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t irq_line;
    int msi_enabled;

    volatile void* common_cfg;
    volatile void* notify_base;
    uint32_t notify_off_multiplier;
    volatile uint8_t* isr_cfg;
    volatile void* device_cfg;

    struct virtqueue* queues[8];
    uint32_t queue_count;
} virtio_pci_dev_t;

#define VIRTIO_PCI_VENDOR_ID 0x1AF4u

#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED      0x80u

#define VIRTIO_F_VERSION_1 (1ull << 32)

int virtio_pci_find_device(uint16_t vendor_id, uint16_t device_id, virtio_pci_dev_t* out_dev);

int virtio_pci_map_modern_caps(virtio_pci_dev_t* dev);

void virtio_pci_reset(virtio_pci_dev_t* dev);

void virtio_pci_set_status(virtio_pci_dev_t* dev, uint8_t status);
void virtio_pci_add_status(virtio_pci_dev_t* dev, uint8_t status_bits);

uint64_t virtio_pci_read_device_features(virtio_pci_dev_t* dev);
void virtio_pci_write_driver_features(virtio_pci_dev_t* dev, uint64_t features);

int virtio_pci_negotiate_features(virtio_pci_dev_t* dev, uint64_t wanted_features, uint64_t* out_accepted_features);

int virtio_pci_queue_init(virtio_pci_dev_t* dev, struct virtqueue* out_vq, uint16_t queue_index, uint16_t requested_size);

int virtio_pci_enable_msi(virtio_pci_dev_t* dev, uint8_t vector);
int virtio_pci_enable_intx(virtio_pci_dev_t* dev, void (*handler)(registers_t*));

void virtio_pci_irq_handler(registers_t* regs);

int virtio_pci_register_queue(virtio_pci_dev_t* dev, struct virtqueue* vq);

#endif
