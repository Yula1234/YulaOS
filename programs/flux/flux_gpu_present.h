#pragma once

#include <stdint.h>

#include <yula.h>
#include <yos/gpu.h>

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t size_bytes;

    uint32_t scanout_id;
    uint32_t resource_id;

    int shm_fd;
    uint32_t* pixels;
} flux_gpu_present_t;

int flux_gpu_present_init(flux_gpu_present_t* p, uint32_t width, uint32_t height, uint32_t pitch);
void flux_gpu_present_shutdown(flux_gpu_present_t* p);

static inline uint32_t* flux_gpu_present_pixels(flux_gpu_present_t* p) {
    return p ? p->pixels : 0;
}

int flux_gpu_present_present(flux_gpu_present_t* p, const fb_rect_t* rects, uint32_t rect_count);
