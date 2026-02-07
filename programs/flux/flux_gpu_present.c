#include "flux_internal.h"
#include "flux_gpu_present.h"

enum {
    FLUX_VIRGL_PIPE_TEXTURE_2D = 2u,

    FLUX_VIRGL_PIPE_BIND_RENDER_TARGET = 2u,
    FLUX_VIRGL_PIPE_BIND_SAMPLER_VIEW = 8u,
    FLUX_VIRGL_PIPE_BIND_SCANOUT = 1u << 19,
};

struct flux_gpu_present_surface_slot {
    uint32_t state;
    uint64_t key;

    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t shm_size_bytes;
    int shm_fd;

    uint32_t commit_gen;
    uint32_t epoch;
};

static void flux_gpu_present_virgl_destroy_resource(flux_gpu_present_t* p, uint32_t* resource_id);
static int flux_gpu_present_virgl_create_3d(flux_gpu_present_t* p, uint32_t resource_id, uint32_t width, uint32_t height, uint32_t bind);
static int flux_gpu_present_virgl_attach_shm(flux_gpu_present_t* p, uint32_t resource_id, int shm_fd, uint32_t shm_size_bytes);
static int flux_gpu_present_virgl_transfer_box(flux_gpu_present_t* p,
                                              uint32_t resource_id,
                                              uint32_t tex_width,
                                              uint32_t tex_height,
                                              uint32_t stride_bytes,
                                              uint32_t x,
                                              uint32_t y,
                                              uint32_t w,
                                              uint32_t h);
static int flux_gpu_present_virgl_copy_2d(flux_gpu_present_t* p,
                                         uint32_t dst_resource_id,
                                         uint32_t dst_x,
                                         uint32_t dst_y,
                                         uint32_t src_resource_id,
                                         uint32_t src_x,
                                         uint32_t src_y,
                                         uint32_t w,
                                         uint32_t h);
static int flux_gpu_present_virgl_flush_rect(flux_gpu_present_t* p, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

static void flux_gpu_present_virgl_slot_destroy(flux_gpu_present_t* p, flux_gpu_present_surface_slot_t* s);
static int flux_gpu_present_virgl_slots_ensure(flux_gpu_present_t* p, uint32_t want_cap);
static flux_gpu_present_surface_slot_t* flux_gpu_present_virgl_slot_get(flux_gpu_present_t* p, uint64_t key);
static void flux_gpu_present_virgl_gc(flux_gpu_present_t* p, uint32_t keep_frames);

static uint32_t flux_gpu_present_hash_u32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

static uint64_t flux_gpu_present_hash_u64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccduLL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53uLL;
    v ^= v >> 33;
    return v;
}

static uint64_t flux_gpu_present_surface_key(uint32_t client_id, uint32_t surface_id) {
    return (uint64_t)client_id << 32 | (uint64_t)surface_id;
}

static int flux_gpu_present_virgl_upload_cursor(flux_gpu_present_t* p, uint32_t resource_id) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;

    const uint32_t width = (uint32_t)COMP_CURSOR_SAVE_W;
    const uint32_t height = (uint32_t)COMP_CURSOR_SAVE_H;

    const uint32_t size_bytes = width * height * 4u;
    if (size_bytes == 0u) return -1;

    int fd = shm_create(size_bytes);
    if (fd < 0) return -1;

    uint32_t* px = (uint32_t*)mmap(fd, size_bytes, MAP_SHARED);
    if (!px) {
        close(fd);
        return -1;
    }

    for (uint32_t i = 0; i < width * height; i++) {
        px[i] = 0u;
    }

    for (uint32_t y = 0; y < 12u && y < height; y++) {
        for (uint32_t x = 0; x <= y && x < width; x++) {
            px[y * width + x] = 0x000000u;
        }
        for (uint32_t x = 1; x < y && x < width; x++) {
            if (y >= 2u) {
                px[y * width + x] = 0xFFFFFFu;
            }
        }
    }

    for (uint32_t y = 9u; y < height; y++) {
        for (uint32_t x = 4u; x < 8u && x < width; x++) {
            px[y * width + x] = 0x000000u;
        }
    }
    for (uint32_t y = 10u; y + 1u < height; y++) {
        for (uint32_t x = 5u; x < 7u && x < width; x++) {
            px[y * width + x] = 0xFFFFFFu;
        }
    }

    int ok = 0;
    if (flux_gpu_present_virgl_attach_shm(p, resource_id, fd, size_bytes) == 0) {
        ok = (flux_gpu_present_virgl_transfer_box(p, resource_id, width, height, width * 4u, 0u, 0u, width, height) == 0);
    }

    (void)munmap((void*)px, size_bytes);
    close(fd);
    return ok ? 0 : -1;
}

static void flux_gpu_present_reset(flux_gpu_present_t* p) {
    if (!p) return;

    p->fd = -1;
    p->width = 0;
    p->height = 0;
    p->pitch = 0;

    p->size_bytes = 0;
    p->scanout_id = 0;
    p->mode = (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;

    p->resource_id = 0;
    p->shm_fd = -1;
    p->pixels = 0;

    p->virgl_front_resource_id = 0u;
    p->virgl_bg_resource_id = 0u;
    p->virgl_preview_h_resource_id = 0u;
    p->virgl_preview_v_resource_id = 0u;
    p->virgl_cursor_resource_id = 0u;
    p->virgl_solid_white_resource_id = 0u;
    p->virgl_solid_black_resource_id = 0u;
    p->virgl_solid_red_resource_id = 0u;

    p->virgl_surfaces = 0;
    p->virgl_surfaces_cap = 0u;
    p->virgl_surfaces_epoch = 0u;
}

static uint32_t flux_gpu_present_choose_resource_id(void) {
    static uint32_t seq = 1u;
    const uint32_t pid = (uint32_t)getpid();

    uint32_t rid = 0x40000000u ^ (pid * 2654435761u) ^ (seq++);
    if (rid == 0u) rid = 1u;
    return rid;
}

static int flux_gpu_present_clip_rect(const flux_gpu_present_t* p,
                                     const fb_rect_t* r,
                                     uint32_t* out_x,
                                     uint32_t* out_y,
                                     uint32_t* out_w,
                                     uint32_t* out_h) {
    if (!p || !r || !out_x || !out_y || !out_w || !out_h) return 0;
    if (r->w <= 0 || r->h <= 0) return 0;

    int32_t x1 = r->x;
    int32_t y1 = r->y;
    int32_t x2 = r->x + r->w;
    int32_t y2 = r->y + r->h;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)p->width) x2 = (int32_t)p->width;
    if (y2 > (int32_t)p->height) y2 = (int32_t)p->height;

    if (x2 <= x1 || y2 <= y1) return 0;

    *out_x = (uint32_t)x1;
    *out_y = (uint32_t)y1;
    *out_w = (uint32_t)(x2 - x1);
    *out_h = (uint32_t)(y2 - y1);
    return 1;
}

static int flux_gpu_present_transfer_and_flush(flux_gpu_present_t* p,
                                              uint32_t x,
                                              uint32_t y,
                                              uint32_t w,
                                              uint32_t h) {
    if (!p || p->fd < 0 || p->resource_id == 0u) return -1;
    if (w == 0u || h == 0u) return 0;

    const uint64_t offset = (uint64_t)y * (uint64_t)p->pitch + (uint64_t)x * 4ull;
    const uint64_t end = offset + ((uint64_t)(h - 1u) * (uint64_t)p->pitch) + (uint64_t)w * 4ull;
    if (end > (uint64_t)p->size_bytes) return -1;

    __asm__ volatile("sfence" ::: "memory");

    yos_gpu_transfer_to_host_2d_t tr;
    tr.resource_id = p->resource_id;
    tr.x = x;
    tr.y = y;
    tr.width = w;
    tr.height = h;
    tr.offset = offset;
    if (ioctl(p->fd, YOS_GPU_TRANSFER_TO_HOST_2D, &tr) != 0) return -1;

    yos_gpu_rect_t fl;
    fl.resource_id = p->resource_id;
    fl.x = x;
    fl.y = y;
    fl.width = w;
    fl.height = h;
    if (ioctl(p->fd, YOS_GPU_RESOURCE_FLUSH, &fl) != 0) return -1;

    return 0;
}

static int flux_gpu_present_virgl_intersect_damage_with_surface(uint32_t dmg_x,
                                                               uint32_t dmg_y,
                                                               uint32_t dmg_w,
                                                               uint32_t dmg_h,
                                                               const flux_gpu_comp_surface_t* s,
                                                               uint32_t* out_dst_x,
                                                               uint32_t* out_dst_y,
                                                               uint32_t* out_src_x,
                                                               uint32_t* out_src_y,
                                                               uint32_t* out_w,
                                                               uint32_t* out_h) {
    if (!s || !out_dst_x || !out_dst_y || !out_src_x || !out_src_y || !out_w || !out_h) return 0;
    if (dmg_w == 0u || dmg_h == 0u) return 0;
    if (s->width == 0u || s->height == 0u) return 0;

    const int64_t dmg_x1 = (int64_t)dmg_x;
    const int64_t dmg_y1 = (int64_t)dmg_y;
    const int64_t dmg_x2 = dmg_x1 + (int64_t)dmg_w;
    const int64_t dmg_y2 = dmg_y1 + (int64_t)dmg_h;

    const int64_t surf_x1 = (int64_t)s->x;
    const int64_t surf_y1 = (int64_t)s->y;
    const int64_t surf_x2 = surf_x1 + (int64_t)s->width;
    const int64_t surf_y2 = surf_y1 + (int64_t)s->height;

    const int64_t ix1 = (dmg_x1 > surf_x1) ? dmg_x1 : surf_x1;
    const int64_t iy1 = (dmg_y1 > surf_y1) ? dmg_y1 : surf_y1;
    const int64_t ix2 = (dmg_x2 < surf_x2) ? dmg_x2 : surf_x2;
    const int64_t iy2 = (dmg_y2 < surf_y2) ? dmg_y2 : surf_y2;
    if (ix2 <= ix1 || iy2 <= iy1) return 0;

    if (ix1 < 0 || iy1 < 0) return 0;
    if (ix2 > 0xFFFFFFFFll || iy2 > 0xFFFFFFFFll) return 0;

    const uint32_t w = (uint32_t)(ix2 - ix1);
    const uint32_t h = (uint32_t)(iy2 - iy1);
    if (w == 0u || h == 0u) return 0;

    const int64_t rel_x = ix1 - surf_x1;
    const int64_t rel_y = iy1 - surf_y1;
    if (rel_x < 0 || rel_y < 0) return 0;
    if ((uint64_t)rel_x + (uint64_t)w > (uint64_t)s->width) return 0;
    if ((uint64_t)rel_y + (uint64_t)h > (uint64_t)s->height) return 0;

    *out_dst_x = (uint32_t)ix1;
    *out_dst_y = (uint32_t)iy1;
    *out_src_x = (uint32_t)rel_x;
    *out_src_y = (uint32_t)rel_y;
    *out_w = w;
    *out_h = h;
    return 1;
}

static int flux_gpu_present_virgl_intersect_damage_with_rect(uint32_t dmg_x,
                                                            uint32_t dmg_y,
                                                            uint32_t dmg_w,
                                                            uint32_t dmg_h,
                                                            int32_t rx,
                                                            int32_t ry,
                                                            uint32_t rw,
                                                            uint32_t rh,
                                                            uint32_t* out_x,
                                                            uint32_t* out_y,
                                                            uint32_t* out_w,
                                                            uint32_t* out_h) {
    if (!out_x || !out_y || !out_w || !out_h) return 0;
    if (dmg_w == 0u || dmg_h == 0u) return 0;
    if (rw == 0u || rh == 0u) return 0;

    const int64_t dmg_x1 = (int64_t)dmg_x;
    const int64_t dmg_y1 = (int64_t)dmg_y;
    const int64_t dmg_x2 = dmg_x1 + (int64_t)dmg_w;
    const int64_t dmg_y2 = dmg_y1 + (int64_t)dmg_h;

    const int64_t r_x1 = (int64_t)rx;
    const int64_t r_y1 = (int64_t)ry;
    const int64_t r_x2 = r_x1 + (int64_t)rw;
    const int64_t r_y2 = r_y1 + (int64_t)rh;

    const int64_t ix1 = (dmg_x1 > r_x1) ? dmg_x1 : r_x1;
    const int64_t iy1 = (dmg_y1 > r_y1) ? dmg_y1 : r_y1;
    const int64_t ix2 = (dmg_x2 < r_x2) ? dmg_x2 : r_x2;
    const int64_t iy2 = (dmg_y2 < r_y2) ? dmg_y2 : r_y2;
    if (ix2 <= ix1 || iy2 <= iy1) return 0;

    if (ix1 < 0 || iy1 < 0) return 0;
    if (ix2 > 0xFFFFFFFFll || iy2 > 0xFFFFFFFFll) return 0;

    const uint32_t w = (uint32_t)(ix2 - ix1);
    const uint32_t h = (uint32_t)(iy2 - iy1);
    if (w == 0u || h == 0u) return 0;

    *out_x = (uint32_t)ix1;
    *out_y = (uint32_t)iy1;
    *out_w = w;
    *out_h = h;
    return 1;
}

static int flux_gpu_present_virgl_copy_solid(flux_gpu_present_t* p,
                                            uint32_t dst_resource_id,
                                            uint32_t dst_x,
                                            uint32_t dst_y,
                                            uint32_t solid_resource_id,
                                            uint32_t w,
                                            uint32_t h) {
    if (!p) return -1;
    if (w == 0u || h == 0u) return 0;
    if (w > 32u || h > 32u) return -1;
    return flux_gpu_present_virgl_copy_2d(p, dst_resource_id, dst_x, dst_y, solid_resource_id, 0u, 0u, w, h);
}

static int flux_gpu_present_virgl_draw_solid_rect_damage_clipped(flux_gpu_present_t* p,
                                                                uint32_t dmg_x,
                                                                uint32_t dmg_y,
                                                                uint32_t dmg_w,
                                                                uint32_t dmg_h,
                                                                int32_t rx,
                                                                int32_t ry,
                                                                uint32_t rw,
                                                                uint32_t rh,
                                                                uint32_t solid_resource_id) {
    if (!p) return -1;
    if (rw == 0u || rh == 0u) return 0;

    uint32_t x = 0u, y = 0u, w = 0u, h = 0u;
    if (!flux_gpu_present_virgl_intersect_damage_with_rect(dmg_x, dmg_y, dmg_w, dmg_h, rx, ry, rw, rh, &x, &y, &w, &h)) {
        return 0;
    }

    return flux_gpu_present_virgl_copy_solid(p, p->virgl_front_resource_id, x, y, solid_resource_id, w, h);
}

static int flux_gpu_present_virgl_draw_cursor_arrow(flux_gpu_present_t* p,
                                                   uint32_t dmg_x,
                                                   uint32_t dmg_y,
                                                   uint32_t dmg_w,
                                                   uint32_t dmg_h,
                                                   int32_t cursor_x,
                                                   int32_t cursor_y) {
    if (!p) return -1;

    const uint32_t black = p->virgl_solid_black_resource_id;
    const uint32_t white = p->virgl_solid_white_resource_id;
    if (black == 0u || white == 0u) return -1;

    enum {
        TIP_H = 13,

        HANDLE_X = 4,
        HANDLE_Y = 9,
        HANDLE_W = 4,
        HANDLE_H = 8,

        HANDLE_INNER_X = HANDLE_X + 1,
        HANDLE_INNER_Y = HANDLE_Y + 1,
        HANDLE_INNER_W = HANDLE_W - 2,
        HANDLE_INNER_H = HANDLE_H - 2,
    };

    {
        const int rc = flux_gpu_present_virgl_draw_solid_rect_damage_clipped(p,
                                                                            dmg_x,
                                                                            dmg_y,
                                                                            dmg_w,
                                                                            dmg_h,
                                                                            cursor_x,
                                                                            cursor_y,
                                                                            2u,
                                                                            2u,
                                                                            black);
        if (rc != 0) return -1;
    }

    for (int y = 0; y < TIP_H; y++) {
        const uint32_t w = (uint32_t)(y + 2);
        const int rc = flux_gpu_present_virgl_draw_solid_rect_damage_clipped(p,
                                                                            dmg_x,
                                                                            dmg_y,
                                                                            dmg_w,
                                                                            dmg_h,
                                                                            cursor_x,
                                                                            cursor_y + y,
                                                                            w,
                                                                            1u,
                                                                            black);
        if (rc != 0) return -1;
    }

    for (int y = 1; y < TIP_H - 1; y++) {
        const uint32_t w = (uint32_t)(y);
        if (w == 0u) continue;
        const int rc = flux_gpu_present_virgl_draw_solid_rect_damage_clipped(p,
                                                                            dmg_x,
                                                                            dmg_y,
                                                                            dmg_w,
                                                                            dmg_h,
                                                                            cursor_x + 1,
                                                                            cursor_y + y,
                                                                            w,
                                                                            1u,
                                                                            white);
        if (rc != 0) return -1;
    }

    {
        const int rc = flux_gpu_present_virgl_draw_solid_rect_damage_clipped(p,
                                                                            dmg_x,
                                                                            dmg_y,
                                                                            dmg_w,
                                                                            dmg_h,
                                                                            cursor_x + HANDLE_X,
                                                                            cursor_y + HANDLE_Y,
                                                                            (uint32_t)HANDLE_W,
                                                                            (uint32_t)HANDLE_H,
                                                                            black);
        if (rc != 0) return -1;
    }

    {
        const int rc = flux_gpu_present_virgl_draw_solid_rect_damage_clipped(p,
                                                                            dmg_x,
                                                                            dmg_y,
                                                                            dmg_w,
                                                                            dmg_h,
                                                                            cursor_x + HANDLE_INNER_X,
                                                                            cursor_y + HANDLE_INNER_Y,
                                                                            (uint32_t)HANDLE_INNER_W,
                                                                            (uint32_t)HANDLE_INNER_H,
                                                                            white);
        if (rc != 0) return -1;
    }

    return 0;
}

int flux_gpu_present_compose(flux_gpu_present_t* p,
                             const fb_rect_t* rects,
                             uint32_t rect_count,
                             const flux_gpu_comp_surface_t* surfaces,
                             uint32_t surface_count,
                             const fb_rect_t* preview_rect,
                             int32_t cursor_x,
                             int32_t cursor_y) {
    if (!p || p->fd < 0) return -1;
    if (p->mode != (uint32_t)FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE) return -1;
    if (p->virgl_front_resource_id == 0u || p->virgl_bg_resource_id == 0u) return -1;
    if (rect_count == 0u) return 0;
    if (!rects) return -1;
    if (surface_count && !surfaces) return -1;

    if (!p->virgl_surfaces || p->virgl_surfaces_cap == 0u) return -1;

    p->virgl_surfaces_epoch++;
    if (p->virgl_surfaces_epoch == 0u) p->virgl_surfaces_epoch = 1u;
    const uint32_t epoch = p->virgl_surfaces_epoch;

    if (surface_count) {
        const uint32_t want = surface_count * 2u + 16u;
        if (flux_gpu_present_virgl_slots_ensure(p, want) != 0) return -1;
    }

    for (uint32_t i = 0; i < surface_count; i++) {
        const flux_gpu_comp_surface_t* cs = &surfaces[i];
        if (cs->client_id == 0u) {
            if (cs->surface_id == 0u) continue;
        }
        if (cs->surface_id == 0u) continue;
        if (cs->width == 0u || cs->height == 0u) continue;
        if (cs->shm_fd < 0 || cs->shm_size_bytes == 0u) continue;
        if (cs->stride_bytes < cs->width * 4u) continue;

        const uint64_t key = flux_gpu_present_surface_key(cs->client_id, cs->surface_id);
        flux_gpu_present_surface_slot_t* slot = flux_gpu_present_virgl_slot_get(p, key);
        if (!slot) return -1;

        if (slot->state != 1u || slot->key != key) {
            if (slot->state == 1u) {
                flux_gpu_present_virgl_slot_destroy(p, slot);
            }
            memset(slot, 0, sizeof(*slot));
            slot->state = 1u;
            slot->key = key;
            slot->resource_id = flux_gpu_present_choose_resource_id();
            slot->shm_fd = -1;
        }

        const int need_recreate = (slot->resource_id == 0u) || (slot->width != cs->width) || (slot->height != cs->height);
        if (need_recreate) {
            if (slot->resource_id != 0u) {
                flux_gpu_present_virgl_destroy_resource(p, &slot->resource_id);
            }
            slot->resource_id = flux_gpu_present_choose_resource_id();
            if (flux_gpu_present_virgl_create_3d(p,
                                                 slot->resource_id,
                                                 cs->width,
                                                 cs->height,
                                                 FLUX_VIRGL_PIPE_BIND_SAMPLER_VIEW) != 0) {
                flux_gpu_present_virgl_slot_destroy(p, slot);
                return -1;
            }
            slot->width = cs->width;
            slot->height = cs->height;
            slot->commit_gen = 0u;
            slot->shm_fd = -1;
            slot->shm_size_bytes = 0u;
        }

        if (slot->shm_fd != cs->shm_fd || slot->shm_size_bytes != cs->shm_size_bytes) {
            if (flux_gpu_present_virgl_attach_shm(p, slot->resource_id, cs->shm_fd, cs->shm_size_bytes) != 0) {
                flux_gpu_present_virgl_slot_destroy(p, slot);
                return -1;
            }
            slot->shm_fd = cs->shm_fd;
            slot->shm_size_bytes = cs->shm_size_bytes;
        }

        slot->stride_bytes = cs->stride_bytes;
        slot->epoch = epoch;

        if (slot->commit_gen != cs->commit_gen) {
            int ok = 1;

            if (cs->damage_count && cs->damage && cs->damage_count <= COMP_IPC_DAMAGE_MAX_RECTS) {
                for (uint32_t di = 0; di < cs->damage_count; di++) {
                    const comp_ipc_rect_t r = cs->damage[di];
                    if (r.w <= 0 || r.h <= 0) continue;

                    int64_t x1 = (int64_t)r.x;
                    int64_t y1 = (int64_t)r.y;
                    int64_t x2 = x1 + (int64_t)r.w;
                    int64_t y2 = y1 + (int64_t)r.h;

                    if (x2 <= 0 || y2 <= 0) continue;
                    if (x1 >= (int64_t)cs->width || y1 >= (int64_t)cs->height) continue;

                    if (x1 < 0) x1 = 0;
                    if (y1 < 0) y1 = 0;
                    if (x2 > (int64_t)cs->width) x2 = (int64_t)cs->width;
                    if (y2 > (int64_t)cs->height) y2 = (int64_t)cs->height;
                    if (x2 <= x1 || y2 <= y1) continue;

                    const uint32_t ux = (uint32_t)x1;
                    const uint32_t uy = (uint32_t)y1;
                    const uint32_t uw = (uint32_t)(x2 - x1);
                    const uint32_t uh = (uint32_t)(y2 - y1);
                    if (uw == 0u || uh == 0u) continue;

                    if (flux_gpu_present_virgl_transfer_box(p,
                                                           slot->resource_id,
                                                           cs->width,
                                                           cs->height,
                                                           cs->stride_bytes,
                                                           ux,
                                                           uy,
                                                           uw,
                                                           uh) != 0) {
                        ok = 0;
                        break;
                    }
                }
            } else {
                ok = (flux_gpu_present_virgl_transfer_box(p,
                                                         slot->resource_id,
                                                         cs->width,
                                                         cs->height,
                                                         cs->stride_bytes,
                                                         0u,
                                                         0u,
                                                         cs->width,
                                                         cs->height) == 0);
            }

            if (!ok) {
                flux_gpu_present_virgl_slot_destroy(p, slot);
                return -1;
            }
            slot->commit_gen = cs->commit_gen;
        }
    }

    flux_gpu_present_virgl_gc(p, 120u);

    for (uint32_t ri = 0; ri < rect_count; ri++) {
        uint32_t x = 0u, y = 0u, w = 0u, h = 0u;
        if (!flux_gpu_present_clip_rect(p, &rects[ri], &x, &y, &w, &h)) continue;

        if (flux_gpu_present_virgl_copy_2d(p,
                                           p->virgl_front_resource_id,
                                           x,
                                           y,
                                           p->virgl_bg_resource_id,
                                           x,
                                           y,
                                           w,
                                           h) != 0) {
            return -1;
        }

        for (uint32_t si = 0; si < surface_count; si++) {
            const flux_gpu_comp_surface_t* cs = &surfaces[si];
            if (cs->surface_id == 0u) continue;
            const uint64_t key = flux_gpu_present_surface_key(cs->client_id, cs->surface_id);
            flux_gpu_present_surface_slot_t* slot = flux_gpu_present_virgl_slot_get(p, key);
            if (!slot || slot->state != 1u || slot->resource_id == 0u) continue;

            uint32_t dst_x = 0u, dst_y = 0u, src_x = 0u, src_y = 0u, cw = 0u, ch = 0u;
            if (!flux_gpu_present_virgl_intersect_damage_with_surface(x, y, w, h, cs, &dst_x, &dst_y, &src_x, &src_y, &cw, &ch)) continue;

            if (flux_gpu_present_virgl_copy_2d(p,
                                               p->virgl_front_resource_id,
                                               dst_x,
                                               dst_y,
                                               slot->resource_id,
                                               src_x,
                                               src_y,
                                               cw,
                                               ch) != 0) {
                return -1;
            }
        }

        if (preview_rect && preview_rect->w > 0 && preview_rect->h > 0) {
            enum { T = 2 };

            const int32_t px = preview_rect->x;
            const int32_t py = preview_rect->y;
            const uint32_t pw = (uint32_t)preview_rect->w;
            const uint32_t ph = (uint32_t)preview_rect->h;

            uint32_t rx = 0u, ry = 0u, rw = 0u, rh = 0u;

            if (flux_gpu_present_virgl_intersect_damage_with_rect(x, y, w, h, px, py, pw, (uint32_t)T, &rx, &ry, &rw, &rh)) {
                if (flux_gpu_present_virgl_copy_2d(p, p->virgl_front_resource_id, rx, ry, p->virgl_preview_h_resource_id, 0u, 0u, rw, rh) != 0) return -1;
            }

            if (ph >= (uint32_t)T) {
                if (flux_gpu_present_virgl_intersect_damage_with_rect(x, y, w, h, px, py + (int32_t)ph - (int32_t)T, pw, (uint32_t)T, &rx, &ry, &rw, &rh)) {
                    if (flux_gpu_present_virgl_copy_2d(p, p->virgl_front_resource_id, rx, ry, p->virgl_preview_h_resource_id, 0u, 0u, rw, rh) != 0) return -1;
                }
            }

            if (flux_gpu_present_virgl_intersect_damage_with_rect(x, y, w, h, px, py, (uint32_t)T, ph, &rx, &ry, &rw, &rh)) {
                if (flux_gpu_present_virgl_copy_2d(p, p->virgl_front_resource_id, rx, ry, p->virgl_preview_v_resource_id, 0u, 0u, rw, rh) != 0) return -1;
            }

            if (pw >= (uint32_t)T) {
                if (flux_gpu_present_virgl_intersect_damage_with_rect(x, y, w, h, px + (int32_t)pw - (int32_t)T, py, (uint32_t)T, ph, &rx, &ry, &rw, &rh)) {
                    if (flux_gpu_present_virgl_copy_2d(p, p->virgl_front_resource_id, rx, ry, p->virgl_preview_v_resource_id, 0u, 0u, rw, rh) != 0) return -1;
                }
            }
        }

        if (cursor_x != 0x7FFFFFFF && cursor_y != 0x7FFFFFFF) {
            if (flux_gpu_present_virgl_draw_cursor_arrow(p, x, y, w, h, cursor_x, cursor_y) != 0) {
                return -1;
            }
        }

        if (flux_gpu_present_virgl_flush_rect(p, p->virgl_front_resource_id, x, y, w, h) != 0) {
            return -1;
        }
    }

    return 0;
}

static void flux_gpu_present_virgl_destroy_resource(flux_gpu_present_t* p, uint32_t* resource_id) {
    if (!p || p->fd < 0 || !resource_id || *resource_id == 0u) return;
    const uint32_t rid = *resource_id;
    (void)ioctl(p->fd, YOS_GPU_RESOURCE_DETACH_BACKING, (void*)&rid);
    (void)ioctl(p->fd, YOS_GPU_RESOURCE_UNREF, (void*)&rid);
    *resource_id = 0u;
}

static int flux_gpu_present_virgl_create_3d(flux_gpu_present_t* p,
                                           uint32_t resource_id,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t bind) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;
    if (width == 0u || height == 0u) return -1;

    yos_gpu_resource_create_3d_t cr;
    memset(&cr, 0, sizeof(cr));
    cr.resource_id = resource_id;
    cr.target = FLUX_VIRGL_PIPE_TEXTURE_2D;
    cr.format = YOS_GPU_FORMAT_B8G8R8X8_UNORM;
    cr.bind = bind;
    cr.width = width;
    cr.height = height;
    cr.depth = 1u;
    cr.array_size = 1u;
    cr.last_level = 0u;
    cr.nr_samples = 0u;
    cr.flags = 0u;

    return ioctl(p->fd, YOS_GPU_RESOURCE_CREATE_3D, &cr) == 0 ? 0 : -1;
}

static int flux_gpu_present_virgl_attach_shm(flux_gpu_present_t* p,
                                            uint32_t resource_id,
                                            int shm_fd,
                                            uint32_t shm_size_bytes) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;
    if (shm_fd < 0 || shm_size_bytes == 0u) return -1;

    yos_gpu_resource_attach_shm_t at;
    memset(&at, 0, sizeof(at));
    at.resource_id = resource_id;
    at.shm_fd = shm_fd;
    at.shm_offset = 0u;
    at.size_bytes = shm_size_bytes;
    return ioctl(p->fd, YOS_GPU_RESOURCE_ATTACH_SHM, &at) == 0 ? 0 : -1;
}

static int flux_gpu_present_virgl_transfer_box(flux_gpu_present_t* p,
                                              uint32_t resource_id,
                                              uint32_t tex_width,
                                              uint32_t tex_height,
                                              uint32_t stride_bytes,
                                              uint32_t x,
                                              uint32_t y,
                                              uint32_t w,
                                              uint32_t h) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;
    if (tex_width == 0u || tex_height == 0u) return -1;
    if (stride_bytes == 0u) return -1;
    if (w == 0u || h == 0u) return 0;
    if (x >= tex_width || y >= tex_height) return -1;
    if (w > tex_width - x || h > tex_height - y) return -1;

    __asm__ volatile("sfence" ::: "memory");

    yos_gpu_transfer_host_3d_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.resource_id = resource_id;
    tr.level = 0u;
    tr.stride = stride_bytes;
    tr.layer_stride = tex_height * stride_bytes;
    tr.box.x = x;
    tr.box.y = y;
    tr.box.z = 0u;
    tr.box.w = w;
    tr.box.h = h;
    tr.box.d = 1u;
    tr.offset = (uint64_t)y * (uint64_t)stride_bytes + (uint64_t)x * 4ull;
    return ioctl(p->fd, YOS_GPU_TRANSFER_TO_HOST_3D, &tr) == 0 ? 0 : -1;
}

static int flux_gpu_present_virgl_copy_2d(flux_gpu_present_t* p,
                                         uint32_t dst_resource_id,
                                         uint32_t dst_x,
                                         uint32_t dst_y,
                                         uint32_t src_resource_id,
                                         uint32_t src_x,
                                         uint32_t src_y,
                                         uint32_t w,
                                         uint32_t h) {
    if (!p || p->fd < 0) return -1;
    if (dst_resource_id == 0u || src_resource_id == 0u) return -1;
    if (w == 0u || h == 0u) return 0;

    yos_gpu_copy_region_3d_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.dst_resource_id = dst_resource_id;
    cp.dst_level = 0u;
    cp.dst_x = dst_x;
    cp.dst_y = dst_y;
    cp.dst_z = 0u;
    cp.src_resource_id = src_resource_id;
    cp.src_level = 0u;
    cp.src_x = src_x;
    cp.src_y = src_y;
    cp.src_z = 0u;
    cp.width = w;
    cp.height = h;
    cp.depth = 1u;
    return ioctl(p->fd, YOS_GPU_RESOURCE_COPY_REGION_3D, &cp) == 0 ? 0 : -1;
}

static int flux_gpu_present_virgl_flush_rect(flux_gpu_present_t* p,
                                            uint32_t resource_id,
                                            uint32_t x,
                                            uint32_t y,
                                            uint32_t w,
                                            uint32_t h) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;
    if (w == 0u || h == 0u) return 0;

    yos_gpu_rect_t fl;
    memset(&fl, 0, sizeof(fl));
    fl.resource_id = resource_id;
    fl.x = x;
    fl.y = y;
    fl.width = w;
    fl.height = h;
    return ioctl(p->fd, YOS_GPU_RESOURCE_FLUSH, &fl) == 0 ? 0 : -1;
}

static void flux_gpu_present_virgl_slot_destroy(flux_gpu_present_t* p, flux_gpu_present_surface_slot_t* s) {
    if (!p || !s) return;
    if (s->resource_id != 0u) {
        flux_gpu_present_virgl_destroy_resource(p, &s->resource_id);
    }

    s->state = 2u;
    s->key = 0u;
    s->width = 0u;
    s->height = 0u;
    s->stride_bytes = 0u;
    s->shm_size_bytes = 0u;
    s->shm_fd = -1;
    s->commit_gen = 0u;
    s->epoch = 0u;
}

static int flux_gpu_present_virgl_slots_ensure(flux_gpu_present_t* p, uint32_t want_cap) {
    if (!p) return -1;
    if (want_cap < 64u) want_cap = 64u;

    uint32_t cap = 1u;
    while (cap < want_cap) cap <<= 1u;

    if (p->virgl_surfaces && p->virgl_surfaces_cap >= cap) return 0;

    flux_gpu_present_surface_slot_t* slots = (flux_gpu_present_surface_slot_t*)calloc((size_t)cap, sizeof(*slots));
    if (!slots) return -1;

    if (p->virgl_surfaces && p->virgl_surfaces_cap) {
        for (uint32_t i = 0; i < p->virgl_surfaces_cap; i++) {
            const flux_gpu_present_surface_slot_t* old = &p->virgl_surfaces[i];
            if (old->state != 1u) continue;

            uint32_t mask = cap - 1u;
            uint32_t pos = (uint32_t)flux_gpu_present_hash_u64(old->key) & mask;
            for (uint32_t step = 0; step < cap; step++) {
                flux_gpu_present_surface_slot_t* cur = &slots[pos];
                if (cur->state == 0u) {
                    *cur = *old;
                    cur->state = 1u;
                    break;
                }
                pos = (pos + 1u) & mask;
            }
        }
        free(p->virgl_surfaces);
    }

    p->virgl_surfaces = slots;
    p->virgl_surfaces_cap = cap;
    return 0;
}

static flux_gpu_present_surface_slot_t* flux_gpu_present_virgl_slot_get(flux_gpu_present_t* p, uint64_t key) {
    if (!p || key == 0u) return 0;
    if (!p->virgl_surfaces || p->virgl_surfaces_cap == 0u) return 0;

    uint32_t mask = p->virgl_surfaces_cap - 1u;
    uint32_t pos = (uint32_t)flux_gpu_present_hash_u64(key) & mask;
    flux_gpu_present_surface_slot_t* tomb = 0;

    for (uint32_t step = 0; step < p->virgl_surfaces_cap; step++) {
        flux_gpu_present_surface_slot_t* s = &p->virgl_surfaces[pos];
        if (s->state == 0u) {
            return tomb ? tomb : s;
        }
        if (s->state == 1u && s->key == key) {
            return s;
        }
        if (!tomb && s->state == 2u) tomb = s;
        pos = (pos + 1u) & mask;
    }

    return tomb;
}

static void flux_gpu_present_virgl_gc(flux_gpu_present_t* p, uint32_t keep_frames) {
    if (!p || !p->virgl_surfaces || p->virgl_surfaces_cap == 0u) return;
    const uint32_t now = p->virgl_surfaces_epoch;

    for (uint32_t i = 0; i < p->virgl_surfaces_cap; i++) {
        flux_gpu_present_surface_slot_t* s = &p->virgl_surfaces[i];
        if (s->state != 1u) continue;
        if ((uint32_t)(now - s->epoch) <= keep_frames) continue;
        flux_gpu_present_virgl_slot_destroy(p, s);
    }
}

static int flux_gpu_present_virgl_fill_and_upload(flux_gpu_present_t* p,
                                                 uint32_t resource_id,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t color) {
    if (!p || p->fd < 0 || resource_id == 0u) return -1;
    if (width == 0u || height == 0u) return -1;

    const uint64_t bytes64 = (uint64_t)width * (uint64_t)height * 4ull;
    if (bytes64 == 0ull || bytes64 > 0xFFFFFFFFu) return -1;
    const uint32_t size_bytes = (uint32_t)bytes64;

    int fd = shm_create(size_bytes);
    if (fd < 0) return -1;

    uint32_t* px = (uint32_t*)mmap(fd, size_bytes, MAP_SHARED);
    if (!px) {
        close(fd);
        return -1;
    }

    const uint32_t n = size_bytes / 4u;
    for (uint32_t i = 0; i < n; i++) {
        px[i] = color;
    }

    int ok = 0;
    if (flux_gpu_present_virgl_attach_shm(p, resource_id, fd, size_bytes) == 0) {
        ok = (flux_gpu_present_virgl_transfer_box(p, resource_id, width, height, width * 4u, 0u, 0u, width, height) == 0);
    }

    (void)munmap((void*)px, size_bytes);
    close(fd);
    return ok ? 0 : -1;
}

static void flux_gpu_present_virgl_shutdown_state(flux_gpu_present_t* p) {
    if (!p) return;

    if (p->virgl_surfaces && p->virgl_surfaces_cap) {
        for (uint32_t i = 0; i < p->virgl_surfaces_cap; i++) {
            flux_gpu_present_surface_slot_t* s = &p->virgl_surfaces[i];
            if (s->state != 1u) continue;
            flux_gpu_present_virgl_slot_destroy(p, s);
        }
        free(p->virgl_surfaces);
    }

    p->virgl_surfaces = 0;
    p->virgl_surfaces_cap = 0u;
    p->virgl_surfaces_epoch = 0u;

    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_front_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_bg_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_preview_h_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_preview_v_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_cursor_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_solid_white_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_solid_black_resource_id);
    flux_gpu_present_virgl_destroy_resource(p, &p->virgl_solid_red_resource_id);
}

static int flux_gpu_present_virgl_init_state(flux_gpu_present_t* p) {
    if (!p || p->fd < 0) return -1;
    if (p->width == 0u || p->height == 0u) return -1;

    const char* stage = "";

    stage = "slots";
    if (flux_gpu_present_virgl_slots_ensure(p, 64u) != 0) goto fail;
    p->virgl_surfaces_epoch = 1u;

    p->virgl_front_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_bg_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_preview_h_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_preview_v_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_cursor_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_solid_white_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_solid_black_resource_id = flux_gpu_present_choose_resource_id();
    p->virgl_solid_red_resource_id = flux_gpu_present_choose_resource_id();

    const uint32_t front_bind = FLUX_VIRGL_PIPE_BIND_RENDER_TARGET | FLUX_VIRGL_PIPE_BIND_SCANOUT;
    const uint32_t src_bind = FLUX_VIRGL_PIPE_BIND_SAMPLER_VIEW;

    stage = "create";
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_front_resource_id, p->width, p->height, front_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_bg_resource_id, p->width, p->height, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_preview_h_resource_id, p->width, 2u, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_preview_v_resource_id, 2u, p->height, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_cursor_resource_id, (uint32_t)COMP_CURSOR_SAVE_W, (uint32_t)COMP_CURSOR_SAVE_H, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_solid_white_resource_id, 32u, 32u, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_solid_black_resource_id, 32u, 32u, src_bind) != 0) goto fail;
    if (flux_gpu_present_virgl_create_3d(p, p->virgl_solid_red_resource_id, 32u, 32u, src_bind) != 0) goto fail;

    stage = "upload";
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_bg_resource_id, p->width, p->height, 0x101010u) != 0) goto fail;
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_preview_h_resource_id, p->width, 2u, 0x007ACCu) != 0) goto fail;
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_preview_v_resource_id, 2u, p->height, 0x007ACCu) != 0) goto fail;
    if (flux_gpu_present_virgl_upload_cursor(p, p->virgl_cursor_resource_id) != 0) goto fail;
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_solid_white_resource_id, 32u, 32u, 0xFFFFFFu) != 0) goto fail;
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_solid_black_resource_id, 32u, 32u, 0x000000u) != 0) goto fail;
    if (flux_gpu_present_virgl_fill_and_upload(p, p->virgl_solid_red_resource_id, 32u, 32u, 0xFF0000u) != 0) goto fail;

    yos_gpu_set_scanout_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.scanout_id = p->scanout_id;
    sc.resource_id = p->virgl_front_resource_id;
    sc.x = 0u;
    sc.y = 0u;
    sc.width = p->width;
    sc.height = p->height;
    stage = "scanout";
    if (ioctl(p->fd, YOS_GPU_SET_SCANOUT, &sc) != 0) goto fail;

    p->mode = (uint32_t)FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE;
    dbg_write("flux: gpu present: VIRGL_COMPOSE\n");
    return 0;

fail:
    {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "flux: virgl init failed at %s\n", stage ? stage : "?");
        dbg_write(tmp);
    }
    flux_gpu_present_virgl_shutdown_state(p);
    p->mode = (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;
    return -1;
}

int flux_gpu_present_init(flux_gpu_present_t* p, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!p || width == 0u || height == 0u || pitch == 0u) return -1;
    if (pitch != width * 4u) return -1;

    flux_gpu_present_reset(p);

    const uint64_t size64 = (uint64_t)pitch * (uint64_t)height;
    if (size64 == 0ull || size64 > 0xFFFFFFFFu) return -1;
    p->size_bytes = (uint32_t)size64;

    int resource_created = 0;

    p->fd = open("/dev/gpu0", 0);
    if (p->fd < 0) goto fail;

    yos_gpu_info_t info;
    memset(&info, 0, sizeof(info));
    if (ioctl(p->fd, YOS_GPU_GET_INFO, &info) != 0) goto fail;
    if (info.abi_version != YOS_GPU_ABI_VERSION) goto fail;
    if ((info.flags & YOS_GPU_INFO_FLAG_ACTIVE) == 0u) goto fail;
    if (info.width != width || info.height != height) goto fail;

    {
        char tmp[128];
        (void)snprintf(tmp,
                       sizeof(tmp),
                       "flux: gpu0 info: w=%u h=%u scanout=%u flags=0x%X\n",
                       (unsigned)info.width,
                       (unsigned)info.height,
                       (unsigned)info.scanout_id,
                       (unsigned)info.flags);
        dbg_write(tmp);
    }

    p->width = width;
    p->height = height;
    p->pitch = pitch;
    p->scanout_id = info.scanout_id;

    const int virgl_supported = (info.flags & YOS_GPU_INFO_FLAG_VIRGL) != 0u;
    if (virgl_supported) {
        if (flux_gpu_present_virgl_init_state(p) == 0) {
            return 0;
        }
        dbg_write("flux: virgl supported but init failed, falling back to 2d upload\n");
    } else {
        dbg_write("flux: virgl not supported, using 2d upload\n");
    }

    p->mode = (uint32_t)FLUX_GPU_PRESENT_MODE_2D_UPLOAD;
    dbg_write("flux: gpu present: 2D_UPLOAD\n");
    p->resource_id = flux_gpu_present_choose_resource_id();

    p->shm_fd = shm_create(p->size_bytes);
    if (p->shm_fd < 0) goto fail;

    p->pixels = (uint32_t*)mmap(p->shm_fd, p->size_bytes, MAP_SHARED);
    if (!p->pixels) goto fail;

    memset(p->pixels, 0, p->size_bytes);

    yos_gpu_resource_create_2d_t cr;
    cr.resource_id = p->resource_id;
    cr.format = YOS_GPU_FORMAT_B8G8R8X8_UNORM;
    cr.width = p->width;
    cr.height = p->height;
    if (ioctl(p->fd, YOS_GPU_RESOURCE_CREATE_2D, &cr) != 0) goto fail;
    resource_created = 1;

    yos_gpu_resource_attach_shm_t at;
    at.resource_id = p->resource_id;
    at.shm_fd = p->shm_fd;
    at.shm_offset = 0u;
    at.size_bytes = p->size_bytes;
    if (ioctl(p->fd, YOS_GPU_RESOURCE_ATTACH_SHM, &at) != 0) goto fail;

    yos_gpu_set_scanout_t sc;
    sc.scanout_id = p->scanout_id;
    sc.resource_id = p->resource_id;
    sc.x = 0u;
    sc.y = 0u;
    sc.width = p->width;
    sc.height = p->height;
    if (ioctl(p->fd, YOS_GPU_SET_SCANOUT, &sc) != 0) goto fail;

    int ok = 1;
    if (!ok) goto fail;
    return 0;

fail:
    flux_gpu_present_virgl_shutdown_state(p);
    if (resource_created && p->fd >= 0 && p->resource_id != 0u) {
        uint32_t rid = p->resource_id;
        (void)ioctl(p->fd, YOS_GPU_RESOURCE_UNREF, &rid);
        p->resource_id = 0u;
    }
    if (p->pixels && p->size_bytes) {
        (void)munmap((void*)p->pixels, p->size_bytes);
        p->pixels = 0;
    }
    if (p->shm_fd >= 0) {
        close(p->shm_fd);
        p->shm_fd = -1;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    flux_gpu_present_reset(p);
    return -1;
}

void flux_gpu_present_shutdown(flux_gpu_present_t* p) {
    if (!p) return;

    if (p->mode == (uint32_t)FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE) {
        flux_gpu_present_virgl_shutdown_state(p);
    }

    if (p->fd >= 0 && p->resource_id != 0u) {
        uint32_t rid = p->resource_id;
        (void)ioctl(p->fd, YOS_GPU_RESOURCE_DETACH_BACKING, &rid);
        (void)ioctl(p->fd, YOS_GPU_RESOURCE_UNREF, &rid);
        p->resource_id = 0u;
    }

    if (p->pixels && p->size_bytes) {
        (void)munmap((void*)p->pixels, p->size_bytes);
        p->pixels = 0;
    }

    if (p->shm_fd >= 0) {
        close(p->shm_fd);
        p->shm_fd = -1;
    }

    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }

    if (p->virgl_surfaces) {
        free(p->virgl_surfaces);
        p->virgl_surfaces = 0;
        p->virgl_surfaces_cap = 0u;
        p->virgl_surfaces_epoch = 0u;
    }

    flux_gpu_present_reset(p);
}

int flux_gpu_present_present(flux_gpu_present_t* p, const fb_rect_t* rects, uint32_t rect_count) {
    if (!p || p->fd < 0 || p->resource_id == 0u) return -1;
    if (rect_count == 0u) return 0;
    if (!rects) return -1;

    for (uint32_t i = 0; i < rect_count; i++) {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        if (!flux_gpu_present_clip_rect(p, &rects[i], &x, &y, &w, &h)) continue;

        if (flux_gpu_present_transfer_and_flush(p, x, y, w, h) != 0) {
            return -1;
        }
    }

    return 0;
}
