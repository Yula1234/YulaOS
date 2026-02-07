#pragma once

#include <stdint.h>

#include <yula.h>
#include <comp_ipc.h>
#include <yos/gpu.h>

typedef struct flux_gpu_present_surface_slot flux_gpu_present_surface_slot_t;

typedef enum {
    FLUX_GPU_PRESENT_MODE_NONE = 0,
    FLUX_GPU_PRESENT_MODE_2D_UPLOAD = 1,
    FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE = 2,
} flux_gpu_present_mode_t;

typedef struct {
    uint32_t client_id;
    uint32_t surface_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t shm_size_bytes;
    int shm_fd;
    uint32_t commit_gen;

    uint32_t damage_count;
    const comp_ipc_rect_t* damage;
} flux_gpu_comp_surface_t;

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;

    uint32_t scanout_id;
    uint32_t mode;

    uint32_t size_bytes;
    uint32_t resource_id;
    int shm_fd;
    uint32_t* pixels;

    uint32_t virgl_front_resource_id;
    uint32_t virgl_bg_resource_id;
    uint32_t virgl_preview_h_resource_id;
    uint32_t virgl_preview_v_resource_id;
    uint32_t virgl_cursor_resource_id;
    uint32_t virgl_solid_white_resource_id;
    uint32_t virgl_solid_black_resource_id;
    uint32_t virgl_solid_red_resource_id;

    flux_gpu_present_surface_slot_t* virgl_surfaces;
    uint32_t virgl_surfaces_cap;
    uint32_t virgl_surfaces_epoch;
} flux_gpu_present_t;

int flux_gpu_present_init(flux_gpu_present_t* p, uint32_t width, uint32_t height, uint32_t pitch);
void flux_gpu_present_shutdown(flux_gpu_present_t* p);

static inline uint32_t flux_gpu_present_mode(const flux_gpu_present_t* p) {
    return p ? p->mode : (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;
}

static inline uint32_t* flux_gpu_present_pixels(flux_gpu_present_t* p) {
    return p ? p->pixels : 0;
}

int flux_gpu_present_present(flux_gpu_present_t* p, const fb_rect_t* rects, uint32_t rect_count);

int flux_gpu_present_compose(flux_gpu_present_t* p,
                             const fb_rect_t* rects,
                             uint32_t rect_count,
                             const flux_gpu_comp_surface_t* surfaces,
                             uint32_t surface_count,
                             const fb_rect_t* preview_rect,
                             int32_t cursor_x,
                             int32_t cursor_y);
