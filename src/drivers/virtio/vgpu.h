
#ifndef DRIVERS_VIRTIO_GPU_H
#define DRIVERS_VIRTIO_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
uint32_t virtio_gpu_get_primary_resource_id(void);
int virtio_gpu_flush_rect(int x, int y, int w, int h);

int virtio_gpu_virgl_is_supported(void);

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

int virtio_gpu_resource_create_3d(uint32_t resource_id,
                                  uint32_t target,
                                  uint32_t format,
                                  uint32_t bind,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t depth,
                                  uint32_t array_size,
                                  uint32_t last_level,
                                  uint32_t nr_samples,
                                  uint32_t flags);

int virtio_gpu_transfer_to_host_3d(uint32_t resource_id,
                                   uint32_t level,
                                   uint32_t stride,
                                   uint32_t layer_stride,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t z,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t d,
                                   uint64_t offset);

int virtio_gpu_virgl_resource_attach(uint32_t resource_id);
int virtio_gpu_virgl_resource_detach(uint32_t resource_id);

int virtio_gpu_virgl_copy_region(uint32_t dst_resource_id,
                                 uint32_t dst_level,
                                 uint32_t dst_x,
                                 uint32_t dst_y,
                                 uint32_t dst_z,
                                 uint32_t src_resource_id,
                                 uint32_t src_level,
                                 uint32_t src_x,
                                 uint32_t src_y,
                                 uint32_t src_z,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t depth);

int virtio_gpu_resource_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
