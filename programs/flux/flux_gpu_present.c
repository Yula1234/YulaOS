#include "flux_gpu_present.h"

static void flux_gpu_present_reset(flux_gpu_present_t* p) {
    if (!p) return;

    p->fd = -1;
    p->width = 0;
    p->height = 0;
    p->pitch = 0;
    p->size_bytes = 0;
    p->scanout_id = 0;
    p->resource_id = 0;
    p->shm_fd = -1;
    p->pixels = 0;
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

    p->width = width;
    p->height = height;
    p->pitch = pitch;
    p->scanout_id = info.scanout_id;
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
