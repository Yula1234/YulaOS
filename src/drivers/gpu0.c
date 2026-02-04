#include <drivers/gpu0.h>
#include <drivers/virtio_gpu.h>

#include <fs/vfs.h>
#include <hal/lock.h>
#include <kernel/proc.h>
#include <kernel/shm.h>
#include <lib/string.h>
#include <mm/heap.h>

#include <yos/gpu.h>

typedef struct {
    uint32_t resource_id;
    uint8_t state;
    uint8_t pad[3];
    vfs_node_t* shm_node;
} gpu0_slot_t;

typedef struct {
    semaphore_t mutex;
    gpu0_slot_t* slots;
    uint32_t cap;
    uint32_t len;
} gpu0_ctx_t;

static uint32_t gpu0_hash_u32(uint32_t x);
static int gpu0_ctx_init(gpu0_ctx_t* ctx);
static void gpu0_ctx_destroy(gpu0_ctx_t* ctx);

static int gpu0_map_grow(gpu0_ctx_t* ctx, uint32_t new_cap);
static gpu0_slot_t* gpu0_map_get(gpu0_ctx_t* ctx, uint32_t resource_id);
static gpu0_slot_t* gpu0_map_put(gpu0_ctx_t* ctx, uint32_t resource_id);
static void gpu0_map_del(gpu0_ctx_t* ctx, uint32_t resource_id);

static int gpu0_vfs_open(vfs_node_t* node);
static int gpu0_vfs_close(vfs_node_t* node);
static int gpu0_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg);

static vfs_ops_t gpu0_ops = {
    .open = gpu0_vfs_open,
    .close = gpu0_vfs_close,
    .ioctl = gpu0_vfs_ioctl,
};

static vfs_node_t gpu0_node = {
    .name = "gpu0",
    .ops = &gpu0_ops,
};

static gpu0_ctx_t* gpu0_ctx_from_node(vfs_node_t* node) {
    if (!node) return 0;
    return (gpu0_ctx_t*)node->private_data;
}

static vfs_node_t* gpu0_fd_to_node(int32_t fd) {
    task_t* curr = proc_current();
    if (!curr) return 0;

    file_t* f = proc_fd_get(curr, (int)fd);
    if (!f || !f->used) return 0;
    if (!f->node) return 0;
    return f->node;
}

void gpu0_vfs_init(void) {
    devfs_register(&gpu0_node);
}

static uint32_t gpu0_hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int gpu0_ctx_init(gpu0_ctx_t* ctx) {
    if (!ctx) return 0;

    memset(ctx, 0, sizeof(*ctx));
    sem_init(&ctx->mutex, 1);

    ctx->cap = 8u;
    ctx->slots = (gpu0_slot_t*)kmalloc((size_t)ctx->cap * sizeof(gpu0_slot_t));
    if (!ctx->slots) {
        ctx->cap = 0u;
        return 0;
    }

    memset(ctx->slots, 0, (size_t)ctx->cap * sizeof(gpu0_slot_t));
    ctx->len = 0u;
    return 1;
}

static void gpu0_ctx_destroy(gpu0_ctx_t* ctx) {
    if (!ctx) return;

    sem_wait(&ctx->mutex);

    if (ctx->slots && ctx->cap != 0u) {
        for (uint32_t i = 0; i < ctx->cap; i++) {
            gpu0_slot_t* s = &ctx->slots[i];
            if (s->state != 1u) continue;

            if (s->shm_node) {
                (void)virtio_gpu_resource_detach_backing(s->resource_id);
                vfs_node_release(s->shm_node);
                s->shm_node = 0;
            }

            (void)virtio_gpu_resource_unref(s->resource_id);
            s->resource_id = 0u;
            s->state = 0u;
        }

        kfree(ctx->slots);
        ctx->slots = 0;
    }

    ctx->cap = 0u;
    ctx->len = 0u;

    sem_signal(&ctx->mutex);
}

static int gpu0_map_grow(gpu0_ctx_t* ctx, uint32_t new_cap) {
    if (!ctx) return 0;
    if (new_cap < 8u) new_cap = 8u;
    if ((new_cap & (new_cap - 1u)) != 0u) return 0;

    gpu0_slot_t* new_slots = (gpu0_slot_t*)kmalloc((size_t)new_cap * sizeof(gpu0_slot_t));
    if (!new_slots) return 0;
    memset(new_slots, 0, (size_t)new_cap * sizeof(gpu0_slot_t));

    uint32_t old_cap = ctx->cap;
    uint32_t old_len = ctx->len;
    gpu0_slot_t* old_slots = ctx->slots;

    ctx->slots = new_slots;
    ctx->cap = new_cap;
    ctx->len = 0u;

    if (old_slots && old_cap != 0u) {
        uint32_t mask = new_cap - 1u;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old_slots[i].state != 1u) continue;

            uint32_t id = old_slots[i].resource_id;
            uint32_t pos = gpu0_hash_u32(id) & mask;
            for (uint32_t step = 0; step < new_cap; step++) {
                gpu0_slot_t* dst = &new_slots[pos];
                if (dst->state == 0u) {
                    *dst = old_slots[i];
                    ctx->len++;
                    break;
                }
                pos = (pos + 1u) & mask;
            }
        }
        kfree(old_slots);
    }

    return ctx->len == old_len;
}

static gpu0_slot_t* gpu0_map_get(gpu0_ctx_t* ctx, uint32_t resource_id) {
    if (!ctx || !ctx->slots || ctx->cap == 0u) return 0;
    if (resource_id == 0u) return 0;

    uint32_t mask = ctx->cap - 1u;
    uint32_t pos = gpu0_hash_u32(resource_id) & mask;

    for (uint32_t step = 0; step < ctx->cap; step++) {
        gpu0_slot_t* s = &ctx->slots[pos];
        if (s->state == 0u) return 0;
        if (s->state == 1u && s->resource_id == resource_id) return s;
        pos = (pos + 1u) & mask;
    }

    return 0;
}

static int gpu0_ctx_owns_resource_locked(gpu0_ctx_t* ctx, uint32_t resource_id) {
    return gpu0_map_get(ctx, resource_id) != 0;
}

static int gpu0_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    if (!node) return -1;

    gpu0_ctx_t* ctx = gpu0_ctx_from_node(node);
    if (!ctx) return -1;

    switch (req) {
        case YOS_GPU_GET_INFO:
        {
            if (!arg) return -1;
            yos_gpu_info_t* info = (yos_gpu_info_t*)arg;

            const int active = virtio_gpu_is_active();
            const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();

            memset(info, 0, sizeof(*info));
            info->abi_version = YOS_GPU_ABI_VERSION;
            info->flags = active ? YOS_GPU_INFO_FLAG_ACTIVE : 0u;
            info->width = fb ? fb->width : 0u;
            info->height = fb ? fb->height : 0u;
            info->scanout_id = virtio_gpu_get_scanout_id();
            return 0;
        }

        case YOS_GPU_RESOURCE_CREATE_2D:
        {
            if (!arg) return -1;
            const yos_gpu_resource_create_2d_t* a = (const yos_gpu_resource_create_2d_t*)arg;
            if (a->resource_id == 0u) return -1;
            if (a->width == 0u || a->height == 0u) return -1;

            sem_wait(&ctx->mutex);
            int exists = gpu0_map_get(ctx, a->resource_id) != 0;
            sem_signal(&ctx->mutex);
            if (exists) return -1;

            if (virtio_gpu_resource_create_2d(a->resource_id, a->format, a->width, a->height) != 0) {
                return -1;
            }

            sem_wait(&ctx->mutex);
            gpu0_slot_t* s = gpu0_map_put(ctx, a->resource_id);
            if (!s) {
                sem_signal(&ctx->mutex);
                (void)virtio_gpu_resource_unref(a->resource_id);
                return -1;
            }
            s->shm_node = 0;
            sem_signal(&ctx->mutex);
            return 0;
        }

        case YOS_GPU_RESOURCE_ATTACH_SHM:
        {
            if (!arg) return -1;
            const yos_gpu_resource_attach_shm_t* a = (const yos_gpu_resource_attach_shm_t*)arg;
            if (a->resource_id == 0u) return -1;
            if (a->shm_fd < 0) return -1;
            if (a->size_bytes == 0u) return -1;

            sem_wait(&ctx->mutex);
            gpu0_slot_t* s = gpu0_map_get(ctx, a->resource_id);
            vfs_node_t* old_shm = s ? s->shm_node : 0;
            if (s) s->shm_node = 0;
            sem_signal(&ctx->mutex);
            if (!s) return -1;

            if (old_shm) {
                (void)virtio_gpu_resource_detach_backing(a->resource_id);
                vfs_node_release(old_shm);
            }

            vfs_node_t* shm_node = gpu0_fd_to_node(a->shm_fd);
            if (!shm_node) return -1;
            if ((shm_node->flags & VFS_FLAG_SHM) == 0u) return -1;

            uint64_t off = (uint64_t)a->shm_offset;
            uint64_t end = off + (uint64_t)a->size_bytes;
            if (end < off) return -1;
            if (end > (uint64_t)shm_node->size) return -1;

            vfs_node_retain(shm_node);

            const uint32_t* pages = 0;
            uint32_t page_count = 0;
            if (!shm_get_phys_pages(shm_node, &pages, &page_count)) {
                vfs_node_release(shm_node);
                return -1;
            }

            if (virtio_gpu_resource_attach_phys_pages(a->resource_id, pages, page_count, a->shm_offset, a->size_bytes) != 0) {
                vfs_node_release(shm_node);
                return -1;
            }

            sem_wait(&ctx->mutex);
            s = gpu0_map_get(ctx, a->resource_id);
            if (!s) {
                sem_signal(&ctx->mutex);
                (void)virtio_gpu_resource_detach_backing(a->resource_id);
                vfs_node_release(shm_node);
                return -1;
            }
            s->shm_node = shm_node;
            sem_signal(&ctx->mutex);
            return 0;
        }

        case YOS_GPU_RESOURCE_DETACH_BACKING:
        {
            if (!arg) return -1;
            uint32_t resource_id = *(const uint32_t*)arg;
            if (resource_id == 0u) return -1;

            sem_wait(&ctx->mutex);
            gpu0_slot_t* s = gpu0_map_get(ctx, resource_id);
            vfs_node_t* shm_node = s ? s->shm_node : 0;
            if (s) s->shm_node = 0;
            sem_signal(&ctx->mutex);

            if (!s) return -1;

            int rc = virtio_gpu_resource_detach_backing(resource_id);
            if (shm_node) vfs_node_release(shm_node);
            return rc == 0 ? 0 : -1;
        }

        case YOS_GPU_RESOURCE_UNREF:
        {
            if (!arg) return -1;
            uint32_t resource_id = *(const uint32_t*)arg;
            if (resource_id == 0u) return -1;

            sem_wait(&ctx->mutex);
            gpu0_slot_t* s = gpu0_map_get(ctx, resource_id);
            vfs_node_t* shm_node = s ? s->shm_node : 0;
            if (s) {
                s->shm_node = 0;
                gpu0_map_del(ctx, resource_id);
            }
            sem_signal(&ctx->mutex);

            if (!s) return -1;

            if (shm_node) {
                (void)virtio_gpu_resource_detach_backing(resource_id);
                vfs_node_release(shm_node);
            }

            return virtio_gpu_resource_unref(resource_id) == 0 ? 0 : -1;
        }

        case YOS_GPU_SET_SCANOUT:
        {
            if (!arg) return -1;
            const yos_gpu_set_scanout_t* a = (const yos_gpu_set_scanout_t*)arg;
            if (a->resource_id == 0u) return -1;
            if (a->scanout_id >= YOS_GPU_MAX_SCANOUTS) return -1;

            sem_wait(&ctx->mutex);
            int ok = gpu0_ctx_owns_resource_locked(ctx, a->resource_id);
            sem_signal(&ctx->mutex);
            if (!ok) return -1;

            return virtio_gpu_set_scanout(a->scanout_id,
                                         a->resource_id,
                                         a->x,
                                         a->y,
                                         a->width,
                                         a->height) == 0 ? 0 : -1;
        }

        case YOS_GPU_TRANSFER_TO_HOST_2D:
        {
            if (!arg) return -1;
            const yos_gpu_transfer_to_host_2d_t* a = (const yos_gpu_transfer_to_host_2d_t*)arg;
            if (a->resource_id == 0u) return -1;
            if (a->width == 0u || a->height == 0u) return -1;

            sem_wait(&ctx->mutex);
            int ok = gpu0_ctx_owns_resource_locked(ctx, a->resource_id);
            sem_signal(&ctx->mutex);
            if (!ok) return -1;

            return virtio_gpu_transfer_to_host_2d(a->resource_id,
                                                 a->x,
                                                 a->y,
                                                 a->width,
                                                 a->height,
                                                 a->offset) == 0 ? 0 : -1;
        }

        case YOS_GPU_RESOURCE_FLUSH:
        {
            if (!arg) return -1;
            const yos_gpu_rect_t* a = (const yos_gpu_rect_t*)arg;
            if (a->resource_id == 0u) return -1;
            if (a->width == 0u || a->height == 0u) return -1;

            sem_wait(&ctx->mutex);
            int ok = gpu0_ctx_owns_resource_locked(ctx, a->resource_id);
            sem_signal(&ctx->mutex);
            if (!ok) return -1;

            return virtio_gpu_resource_flush(a->resource_id,
                                            a->x,
                                            a->y,
                                            a->width,
                                            a->height) == 0 ? 0 : -1;
        }
        default:
            break;
    }

    return -1;
}

static int gpu0_vfs_open(vfs_node_t* node) {
    if (!node) return -1;

    gpu0_ctx_t* ctx = (gpu0_ctx_t*)kmalloc(sizeof(*ctx));
    if (!ctx) return -1;

    if (!gpu0_ctx_init(ctx)) {
        kfree(ctx);
        return -1;
    }

    node->private_data = ctx;
    return 0;
}

static int gpu0_vfs_close(vfs_node_t* node) {
    if (!node) return -1;

    gpu0_ctx_t* ctx = gpu0_ctx_from_node(node);
    if (ctx) {
        gpu0_ctx_destroy(ctx);
        kfree(ctx);
        node->private_data = 0;
    }

    if ((node->flags & VFS_FLAG_DEVFS_ALLOC) != 0u) {
        kfree(node);
    }
    return 0;
}

static void gpu0_map_del(gpu0_ctx_t* ctx, uint32_t resource_id) {
    if (!ctx || !ctx->slots || ctx->cap == 0u) return;
    if (resource_id == 0u) return;

    uint32_t mask = ctx->cap - 1u;
    uint32_t pos = gpu0_hash_u32(resource_id) & mask;

    for (uint32_t step = 0; step < ctx->cap; step++) {
        gpu0_slot_t* s = &ctx->slots[pos];
        if (s->state == 0u) return;
        if (s->state == 1u && s->resource_id == resource_id) {
            s->state = 2u;
            s->resource_id = 0u;
            s->shm_node = 0;
            if (ctx->len > 0u) ctx->len--;
            return;
        }
        pos = (pos + 1u) & mask;
    }
}

static gpu0_slot_t* gpu0_map_put(gpu0_ctx_t* ctx, uint32_t resource_id) {
    if (!ctx || !ctx->slots || ctx->cap == 0u) return 0;
    if (resource_id == 0u) return 0;

    if ((ctx->len + 1u) * 4u > ctx->cap * 3u) {
        if (!gpu0_map_grow(ctx, ctx->cap * 2u)) return 0;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t mask = ctx->cap - 1u;
        uint32_t pos = gpu0_hash_u32(resource_id) & mask;
        gpu0_slot_t* first_tomb = 0;

        for (uint32_t step = 0; step < ctx->cap; step++) {
            gpu0_slot_t* s = &ctx->slots[pos];

            if (s->state == 1u) {
                if (s->resource_id == resource_id) return s;
            } else if (s->state == 2u) {
                if (!first_tomb) first_tomb = s;
            } else {
                gpu0_slot_t* dst = first_tomb ? first_tomb : s;
                memset(dst, 0, sizeof(*dst));
                dst->state = 1u;
                dst->resource_id = resource_id;
                ctx->len++;
                return dst;
            }

            pos = (pos + 1u) & mask;
        }

        if (!gpu0_map_grow(ctx, ctx->cap * 2u)) return 0;
    }

    return 0;
}
