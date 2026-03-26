#ifndef DRIVERS_BLOCK_BDEV_H
#define DRIVERS_BLOCK_BDEV_H

#include <stdint.h>

#include <fs/vfs.h>

#include <lib/dlist.h>

#ifdef __cplusplus
extern "C" {
#endif

struct block_device;

typedef struct block_ops {
    int (*read_sectors)(
        struct block_device* dev,
        uint64_t lba,
        uint32_t count,
        void* buf
    );

    int (*write_sectors)(
        struct block_device* dev,
        uint64_t lba,
        uint32_t count,
        const void* buf
    );

    int (*flush)(struct block_device* dev);
} block_ops_t;

typedef struct block_device {
    const char* name;

    uint32_t sector_size;

    uint64_t sector_count;

    const block_ops_t* ops;

    void* private_data;

    vfs_node_t node_template;

    uint32_t refs_;

    dlist_head_t list_node;
} block_device_t;

void bdev_init(void);

int bdev_register(block_device_t* dev);
int bdev_unregister(const char* name);

block_device_t* bdev_find_by_name(const char* name);
block_device_t* bdev_first(void);

void bdev_set_root(block_device_t* dev);
block_device_t* bdev_root(void);

int bdev_read_sectors(block_device_t* dev, uint64_t lba, uint32_t count, void* buf);
int bdev_write_sectors(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf);
int bdev_flush(block_device_t* dev);

void bdev_retain(block_device_t* dev);
void bdev_release(block_device_t* dev);

#ifdef __cplusplus
}
#endif

#endif
