
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
int virtio_gpu_flush_rect(int x, int y, int w, int h);

#endif
