
#ifndef DRIVERS_VIRTIO_GPU_H
#define DRIVERS_VIRTIO_GPU_H

#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t size_bytes;

    uint32_t* fb_ptr;
    uint32_t  fb_phys;
} virtio_gpu_fb_t;

int virtio_gpu_init(void);
int virtio_gpu_is_active(void);
const virtio_gpu_fb_t* virtio_gpu_get_fb(void);
uint32_t virtio_gpu_get_scanout_id(void);
int virtio_gpu_flush_rect(int x, int y, int w, int h);

int virtio_gpu_resource_create_2d(uint32_t resource_id, uint32_t format, uint32_t width, uint32_t height);
int virtio_gpu_resource_attach_phys_pages(uint32_t resource_id,
                                         const uint32_t* phys_pages,
                                         uint32_t page_count,
                                         uint32_t page_offset,
                                         uint32_t size_bytes);
int virtio_gpu_resource_detach_backing(uint32_t resource_id);
int virtio_gpu_resource_unref(uint32_t resource_id);

int virtio_gpu_set_scanout(uint32_t scanout_id,
                           uint32_t resource_id,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height);

int virtio_gpu_transfer_to_host_2d(uint32_t resource_id,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t width,
                                   uint32_t height,
                                   uint64_t offset);

int virtio_gpu_resource_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

#endif
