// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_VIRTIO_VIRTIO_CORE_H
#define DRIVERS_VIRTIO_VIRTIO_CORE_H

#include <arch/i386/idt.h>

#include <drivers/driver.h>

#include <lib/dlist.h>

#include <stdint.h>

struct virtqueue;
struct virtio_device;

#define VIRTIO_ID_NET      1u
#define VIRTIO_ID_BLOCK    2u
#define VIRTIO_ID_GPU      16u

typedef struct virtio_driver virtio_driver_t;

typedef struct virtio_ops {
    void (*reset)(struct virtio_device* vdev);
    void (*add_status)(struct virtio_device* vdev, uint8_t status_bits);

    uint64_t (*read_device_features)(struct virtio_device* vdev);
    void (*write_driver_features)(struct virtio_device* vdev, uint64_t features);

    int (*negotiate_features)(struct virtio_device* vdev, uint64_t wanted_features, uint64_t* out_accepted);

    int (*setup_queue)(struct virtio_device* vdev, uint16_t queue_index, struct virtqueue* vq, uint16_t requested_size);

    int (*enable_msi)(struct virtio_device* vdev, uint8_t vector);
    int (*enable_intx)(struct virtio_device* vdev, void (*handler)(registers_t* regs));

    uint8_t (*read_config8)(struct virtio_device* vdev, uint32_t offset);
    uint16_t (*read_config16)(struct virtio_device* vdev, uint32_t offset);
    uint32_t (*read_config32)(struct virtio_device* vdev, uint32_t offset);
    void (*write_config32)(struct virtio_device* vdev, uint32_t offset, uint32_t val);

    void (*notify)(struct virtio_device* vdev, uint16_t queue_index);
} virtio_ops_t;

typedef struct virtio_device {
    device_t dev;

    void* transport_data;
    const virtio_ops_t* ops;

    uint16_t virtio_dev_id;

    virtio_driver_t* attached_virtio_driver;
} virtio_device_t;

struct virtio_driver {
    dlist_head_t drivers_node;

    const char* name;
    uint16_t device_type;

    int (*probe)(virtio_device_t* vdev);
    void (*remove)(virtio_device_t* vdev);
};

int virtio_register_driver(virtio_driver_t* driver);

int virtio_register_device(virtio_device_t* vdev);

#endif
