#include <drivers/virtio_gpu.h>
#include <drivers/virtio_pci.h>
#include <drivers/virtqueue.h>
#include <hal/lock.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <arch/i386/paging.h>

extern volatile uint32_t timer_ticks;

#define VIRTIO_GPU_PCI_DEVICE_ID 0x1050u
#define VIRTIO_GPU_MSI_VECTOR 0xA2u
#define VIRTIO_GPU_QUEUE_CTRL 0u

#define VIRTIO_GPU_F_VIRGL 0u

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107u

#define VIRTIO_GPU_CMD_CTX_CREATE 0x0200u
#define VIRTIO_GPU_CMD_CTX_DESTROY 0x0201u
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202u
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D 0x0204u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205u
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206u
#define VIRTIO_GPU_CMD_SUBMIT_3D 0x0207u

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101u

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} virtio_gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_one_t;

#define VIRTIO_GPU_MAX_SCANOUTS 16u

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} virtio_gpu_resource_attach_backing_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_id_cmd_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_resource_attach_backing_t req;
    virtio_gpu_mem_entry_t entry;
} virtio_gpu_resource_attach_backing_1_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
} virtio_gpu_box_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} virtio_gpu_transfer_host_3d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} virtio_gpu_resource_create_3d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} virtio_gpu_ctx_create_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
} virtio_gpu_ctx_destroy_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_ctx_resource_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t size;
    uint32_t padding;
} virtio_gpu_cmd_submit_3d_t;

#define VIRGL_CCMD_RESOURCE_COPY_REGION 17u
#define VIRGL_CMD_RCR_PAYLOAD_DWORDS 13u
#define VIRGL_CMD0(cmd, obj, len) ((uint32_t)((cmd) | ((obj) << 8) | ((len) << 16)))

typedef struct {
    uint32_t resource_id;
    uint8_t state;
    uint8_t pad[3];
} vgpu_attached_slot_t;

#define VGPU_VIRGL_ATTACHED_CAP 1024u

typedef struct {
    vgpu_attached_slot_t slots[VGPU_VIRGL_ATTACHED_CAP];
    uint32_t len;
    uint32_t tombs;
} vgpu_attached_set_t;

typedef struct {
    int active;
    int virgl_supported;
    int virgl_ctx_ready;
    virtio_pci_dev_t dev;
    virtqueue_t ctrlq;
    spinlock_t lock;
    uint32_t scanout_id;
    uint32_t scanout_bound_resource_id;
    virtio_gpu_rect_t scanout_bound_rect;
    uint32_t resource_id;
    uint32_t virgl_ctx_id;
    uint32_t backing_order;
    void* ctrl_cmd;
    void* ctrl_resp;
    uint32_t ctrl_cmd_phys;
    uint32_t ctrl_resp_phys;
    virtio_gpu_fb_t fb;
    vgpu_attached_set_t attached;
} virtio_gpu_state_t;

static virtio_gpu_state_t g_vgpu;

#define VGPU_CTRLQ_QSZ 64u
#define VGPU_CTRLQ_TIMEOUT_TICKS 30000u
#define VGPU_CTRLQ_TIMEOUT_SPINS 20000000u

static void vgpu_mark_inactive_locked(void);
static int vgpu_ctrlq_submit_locked(uint32_t cmd_len, uint32_t resp_len, uint32_t expected_resp_type);

static uint32_t vgpu_hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void vgpu_attached_reset(vgpu_attached_set_t* s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

static int vgpu_attached_contains(const vgpu_attached_set_t* s, uint32_t resource_id) {
    if (!s || resource_id == 0u) return 0;

    uint32_t mask = VGPU_VIRGL_ATTACHED_CAP - 1u;
    uint32_t pos = vgpu_hash_u32(resource_id) & mask;
    for (uint32_t probe = 0; probe < VGPU_VIRGL_ATTACHED_CAP; probe++) {
        const vgpu_attached_slot_t* slot = &s->slots[pos];
        if (slot->state == 0u) return 0;
        if (slot->state == 1u && slot->resource_id == resource_id) return 1;
        pos = (pos + 1u) & mask;
    }

    return 0;
}

static void vgpu_attached_remove(vgpu_attached_set_t* s, uint32_t resource_id) {
    if (!s || resource_id == 0u) return;

    uint32_t mask = VGPU_VIRGL_ATTACHED_CAP - 1u;
    uint32_t pos = vgpu_hash_u32(resource_id) & mask;
    for (uint32_t probe = 0; probe < VGPU_VIRGL_ATTACHED_CAP; probe++) {
        vgpu_attached_slot_t* slot = &s->slots[pos];
        if (slot->state == 0u) return;
        if (slot->state == 1u && slot->resource_id == resource_id) {
            slot->state = 2u;
            slot->resource_id = 0u;
            if (s->len) s->len--;
            s->tombs++;
            return;
        }
        pos = (pos + 1u) & mask;
    }
}

static void vgpu_attached_insert(vgpu_attached_set_t* s, uint32_t resource_id) {
    if (!s || resource_id == 0u) return;
    if (vgpu_attached_contains(s, resource_id)) return;

    uint32_t mask = VGPU_VIRGL_ATTACHED_CAP - 1u;
    uint32_t pos = vgpu_hash_u32(resource_id) & mask;
    vgpu_attached_slot_t* tomb = 0;

    for (uint32_t probe = 0; probe < VGPU_VIRGL_ATTACHED_CAP; probe++) {
        vgpu_attached_slot_t* slot = &s->slots[pos];
        if (slot->state == 0u) {
            vgpu_attached_slot_t* dst = tomb ? tomb : slot;
            dst->state = 1u;
            dst->resource_id = resource_id;
            s->len++;
            return;
        }

        if (slot->state == 2u && !tomb) {
            tomb = slot;
        }
        pos = (pos + 1u) & mask;
    }
}

static int vgpu_virgl_ctx_ensure_locked(void) {
    if (!g_vgpu.active || !g_vgpu.virgl_supported) return 0;
    if (g_vgpu.virgl_ctx_ready) return 1;

    if (g_vgpu.virgl_ctx_id == 0u) {
        g_vgpu.virgl_ctx_id = 1u;
    }

    virtio_gpu_ctx_create_t* cmd = (virtio_gpu_ctx_create_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = g_vgpu.virgl_ctx_id;
    cmd->nlen = 0u;
    cmd->context_init = 0u;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        return 0;
    }

    g_vgpu.virgl_ctx_ready = 1;
    return 1;
}

static int vgpu_virgl_attach_resource_locked(uint32_t resource_id) {
    if (resource_id == 0u) return 0;
    if (!vgpu_virgl_ctx_ensure_locked()) return 0;

    if (vgpu_attached_contains(&g_vgpu.attached, resource_id)) {
        return 1;
    }

    virtio_gpu_ctx_resource_t* cmd = (virtio_gpu_ctx_resource_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd->hdr.ctx_id = g_vgpu.virgl_ctx_id;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        return 0;
    }

    vgpu_attached_insert(&g_vgpu.attached, resource_id);
    return 1;
}

static int vgpu_virgl_detach_resource_locked(uint32_t resource_id) {
    if (resource_id == 0u) return 0;
    if (!g_vgpu.active || !g_vgpu.virgl_supported) return 0;

    if (!vgpu_attached_contains(&g_vgpu.attached, resource_id)) {
        return 1;
    }

    if (!g_vgpu.virgl_ctx_ready) {
        vgpu_attached_remove(&g_vgpu.attached, resource_id);
        return 1;
    }

    virtio_gpu_ctx_resource_t* cmd = (virtio_gpu_ctx_resource_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    cmd->hdr.ctx_id = g_vgpu.virgl_ctx_id;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        return 0;
    }

    vgpu_attached_remove(&g_vgpu.attached, resource_id);
    return 1;
}

static uint32_t vgpu_pages_order_for_bytes(uint32_t bytes) {
    uint32_t pages = (bytes + 4095u) >> 12;
    if (pages == 0) return 0;

    uint32_t order = 0;
    uint32_t pow2 = 1;
    while (pow2 < pages && order < 31) {
        pow2 <<= 1;
        order++;
    }

    return order;
}

static void vgpu_mark_inactive_locked(void) {
    g_vgpu.active = 0;
    g_vgpu.scanout_bound_resource_id = 0;
    memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
}

static void vgpu_cleanup_state(void) {
    vgpu_mark_inactive_locked();

    vgpu_attached_reset(&g_vgpu.attached);
    g_vgpu.virgl_supported = 0;
    g_vgpu.virgl_ctx_ready = 0;
    g_vgpu.virgl_ctx_id = 0u;

    if (g_vgpu.ctrlq.ring_mem) {
        virtqueue_destroy(&g_vgpu.ctrlq);
    }

    if (g_vgpu.ctrl_cmd) {
        pmm_free_block(g_vgpu.ctrl_cmd);
        g_vgpu.ctrl_cmd = 0;
        g_vgpu.ctrl_cmd_phys = 0;
    }

    if (g_vgpu.ctrl_resp) {
        pmm_free_block(g_vgpu.ctrl_resp);
        g_vgpu.ctrl_resp = 0;
        g_vgpu.ctrl_resp_phys = 0;
    }

    if (g_vgpu.fb.fb_ptr) {
        pmm_free_pages((void*)(uintptr_t)g_vgpu.fb.fb_phys, g_vgpu.backing_order);
        g_vgpu.fb.fb_ptr = 0;
        g_vgpu.fb.fb_phys = 0;
        g_vgpu.backing_order = 0;
    }

    memset(&g_vgpu.fb, 0, sizeof(g_vgpu.fb));
    g_vgpu.scanout_id = 0;
    g_vgpu.scanout_bound_resource_id = 0;
    memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
    g_vgpu.resource_id = 0;
}

static int vgpu_ctrlq_submit_locked(uint32_t cmd_len, uint32_t resp_len, uint32_t expected_resp_type) {
    if (!g_vgpu.ctrlq.ring_mem) return 0;
    if (!g_vgpu.ctrl_cmd || !g_vgpu.ctrl_resp) return 0;
    if (g_vgpu.ctrl_cmd_phys == 0 || g_vgpu.ctrl_resp_phys == 0) return 0;
    if (cmd_len == 0 || resp_len == 0) return 0;

    memset(g_vgpu.ctrl_resp, 0, resp_len);

    uint64_t addrs[2] = {
        (uint64_t)g_vgpu.ctrl_cmd_phys,
        (uint64_t)g_vgpu.ctrl_resp_phys,
    };

    uint32_t lens[2] = {
        cmd_len,
        resp_len,
    };

    uint16_t flags[2] = {
        0,
        VRING_DESC_F_WRITE,
    };

    virtqueue_token_t* token = 0;
    if (!virtqueue_submit(&g_vgpu.ctrlq, addrs, lens, flags, 2, 0, &token) || !token) {
        return 0;
    }

    uint32_t start_ticks = timer_ticks;
    for (uint32_t spins = 0;; spins++) {
        virtqueue_handle_irq(&g_vgpu.ctrlq);

        if (sem_try_acquire(&token->sem)) {
            break;
        }

        if ((uint32_t)(timer_ticks - start_ticks) > VGPU_CTRLQ_TIMEOUT_TICKS || spins > VGPU_CTRLQ_TIMEOUT_SPINS) {
            virtqueue_destroy(&g_vgpu.ctrlq);
            virtqueue_token_destroy(token);
            return 0;
        }

        __asm__ volatile("pause");
    }

    virtqueue_token_destroy(token);
    const virtio_gpu_ctrl_hdr_t* rhdr = (const virtio_gpu_ctrl_hdr_t*)g_vgpu.ctrl_resp;
    return rhdr->type == expected_resp_type;
}

static int vgpu_ctrlq_submit_sg_locked(const uint64_t* addrs,
                                      const uint32_t* lens,
                                      const uint16_t* flags,
                                      uint16_t count,
                                      uint32_t expected_resp_type) {
    if (!g_vgpu.ctrlq.ring_mem) return 0;
    if (!g_vgpu.ctrl_cmd || !g_vgpu.ctrl_resp) return 0;
    if (g_vgpu.ctrl_cmd_phys == 0 || g_vgpu.ctrl_resp_phys == 0) return 0;

    if (!addrs || !lens || !flags) return 0;
    if (count < 2u) return 0;

    uint16_t resp_i = (uint16_t)(count - 1u);
    if (addrs[resp_i] != (uint64_t)g_vgpu.ctrl_resp_phys) return 0;
    if ((flags[resp_i] & VRING_DESC_F_WRITE) == 0u) return 0;
    if (lens[resp_i] == 0u) return 0;

    memset(g_vgpu.ctrl_resp, 0, lens[resp_i]);

    virtqueue_token_t* token = 0;
    if (!virtqueue_submit(&g_vgpu.ctrlq, addrs, lens, flags, count, 0, &token) || !token) {
        return 0;
    }

    uint32_t start_ticks = timer_ticks;
    for (uint32_t spins = 0;; spins++) {
        virtqueue_handle_irq(&g_vgpu.ctrlq);

        if (sem_try_acquire(&token->sem)) {
            break;
        }

        if ((uint32_t)(timer_ticks - start_ticks) > VGPU_CTRLQ_TIMEOUT_TICKS || spins > VGPU_CTRLQ_TIMEOUT_SPINS) {
            virtqueue_destroy(&g_vgpu.ctrlq);
            virtqueue_token_destroy(token);
            return 0;
        }

        __asm__ volatile("pause");
    }

    virtqueue_token_destroy(token);
    const virtio_gpu_ctrl_hdr_t* rhdr = (const virtio_gpu_ctrl_hdr_t*)g_vgpu.ctrl_resp;
    return rhdr->type == expected_resp_type;
}

static int vgpu_ctrlq_submit(uint32_t cmd_len, uint32_t resp_len, uint32_t expected_resp_type) {
    spinlock_acquire(&g_vgpu.lock);
    int ok = vgpu_ctrlq_submit_locked(cmd_len, resp_len, expected_resp_type);
    spinlock_release(&g_vgpu.lock);
    return ok;
}

static int vgpu_get_display_info(uint32_t* out_w, uint32_t* out_h, uint32_t* out_scanout_id) {
    if (!out_w || !out_h || !out_scanout_id) return 0;
    *out_w = 0;
    *out_h = 0;
    *out_scanout_id = 0;

    virtio_gpu_ctrl_hdr_t* cmd = (virtio_gpu_ctrl_hdr_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (!vgpu_ctrlq_submit(sizeof(*cmd), sizeof(virtio_gpu_resp_display_info_t), VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) {
        return 0;
    }

    const virtio_gpu_resp_display_info_t* info = (const virtio_gpu_resp_display_info_t*)g_vgpu.ctrl_resp;

    uint32_t best_scanout_any = 0;
    uint32_t best_w_any = 0;
    uint32_t best_h_any = 0;
    uint64_t best_area_any = 0;

    uint32_t best_scanout_land = 0;
    uint32_t best_w_land = 0;
    uint32_t best_h_land = 0;
    uint64_t best_area_land = 0;

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!info->pmodes[i].enabled) continue;

        uint32_t w = info->pmodes[i].r.width;
        uint32_t h = info->pmodes[i].r.height;
        if (w == 0 || h == 0) continue;

        uint64_t area = (uint64_t)w * (uint64_t)h;
        const int landscape = (w >= h);

        if (area > best_area_any || (area == best_area_any && best_area_any != 0 && i < best_scanout_any)) {
            best_scanout_any = i;
            best_w_any = w;
            best_h_any = h;
            best_area_any = area;
        }

        if (landscape) {
            if (area > best_area_land || (area == best_area_land && best_area_land != 0 && i < best_scanout_land)) {
                best_scanout_land = i;
                best_w_land = w;
                best_h_land = h;
                best_area_land = area;
            }
        }
    }

    if (best_area_land != 0) {
        *out_scanout_id = best_scanout_land;
        *out_w = best_w_land;
        *out_h = best_h_land;
        return 1;
    }

    if (best_area_any == 0) return 0;
    *out_scanout_id = best_scanout_any;
    *out_w = best_w_any;
    *out_h = best_h_any;
    return 1;
}

int virtio_gpu_init(void) {
    memset(&g_vgpu, 0, sizeof(g_vgpu));
    spinlock_init(&g_vgpu.lock);
    vgpu_attached_reset(&g_vgpu.attached);

    if (!virtio_pci_find_device(VIRTIO_PCI_VENDOR_ID, VIRTIO_GPU_PCI_DEVICE_ID, &g_vgpu.dev)) {
        vgpu_cleanup_state();
        return 0;
    }

    if (!virtio_pci_map_modern_caps(&g_vgpu.dev)) {
        vgpu_cleanup_state();
        return 0;
    }

    virtio_pci_reset(&g_vgpu.dev);

    uint64_t accepted = 0;
    uint64_t wanted = VIRTIO_F_VERSION_1 | (1ull << VIRTIO_GPU_F_VIRGL);
    if (!virtio_pci_negotiate_features(&g_vgpu.dev, wanted, &accepted)) {
        vgpu_cleanup_state();
        return 0;
    }
    g_vgpu.virgl_supported = (accepted & (1ull << VIRTIO_GPU_F_VIRGL)) != 0ull;

    (void)virtio_pci_enable_msi(&g_vgpu.dev, VIRTIO_GPU_MSI_VECTOR);
    if (!g_vgpu.dev.msi_enabled) {
        (void)virtio_pci_enable_intx(&g_vgpu.dev, virtio_pci_irq_handler);
    }

    if (!virtio_pci_queue_init(&g_vgpu.dev, &g_vgpu.ctrlq, VIRTIO_GPU_QUEUE_CTRL, (uint16_t)VGPU_CTRLQ_QSZ)) {
        vgpu_cleanup_state();
        return 0;
    }

    g_vgpu.ctrl_cmd = pmm_alloc_block();
    g_vgpu.ctrl_resp = pmm_alloc_block();
    if (!g_vgpu.ctrl_cmd || !g_vgpu.ctrl_resp) {
        vgpu_cleanup_state();
        return 0;
    }

    memset(g_vgpu.ctrl_cmd, 0, PAGE_SIZE);
    memset(g_vgpu.ctrl_resp, 0, PAGE_SIZE);

    g_vgpu.ctrl_cmd_phys = (uint32_t)(uintptr_t)g_vgpu.ctrl_cmd;
    g_vgpu.ctrl_resp_phys = (uint32_t)(uintptr_t)g_vgpu.ctrl_resp;

    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t scanout = 0;
    if (!vgpu_get_display_info(&w, &h, &scanout)) {
        vgpu_cleanup_state();
        return 0;
    }

    uint64_t pitch64 = (uint64_t)w * 4ull;
    uint64_t size64 = pitch64 * (uint64_t)h;
    if (pitch64 == 0 || pitch64 > 0xFFFFFFFFu || size64 == 0 || size64 > 0xFFFFFFFFu) {
        vgpu_cleanup_state();
        return 0;
    }

    g_vgpu.scanout_id = scanout;
    g_vgpu.resource_id = 1u;

    g_vgpu.fb.width = w;
    g_vgpu.fb.height = h;
    g_vgpu.fb.pitch = (uint32_t)pitch64;
    g_vgpu.fb.size_bytes = (uint32_t)size64;

    uint32_t order = vgpu_pages_order_for_bytes(g_vgpu.fb.size_bytes);
    if (order > PMM_MAX_ORDER) {
        vgpu_cleanup_state();
        return 0;
    }

    void* fb_phys = pmm_alloc_pages(order);
    if (!fb_phys) {
        vgpu_cleanup_state();
        return 0;
    }

    if (paging_pat_is_supported()) {
        uint64_t bytes64 = (uint64_t)PAGE_SIZE << order;
        if (bytes64 != 0 && bytes64 <= 0xFFFFFFFFu) {
            uint32_t bytes = (uint32_t)bytes64;
            uint32_t start = (uint32_t)(uintptr_t)fb_phys;
            if (start <= 0xFFFFFFFFu - bytes) {
                uint32_t flags = PTE_PRESENT | PTE_RW | PTE_PAT;
                uint32_t end = start + bytes;
                for (uint32_t p = start; p < end; p += 4096u) {
                    paging_map(kernel_page_directory, p, p, flags);
                    if (p + 4096u < p) break;
                }
            }
        }
    }

    memset(fb_phys, 0, (size_t)PAGE_SIZE << order);

    g_vgpu.backing_order = order;
    g_vgpu.fb.fb_phys = (uint32_t)(uintptr_t)fb_phys;
    g_vgpu.fb.fb_ptr = (uint32_t*)fb_phys;

    spinlock_acquire(&g_vgpu.lock);

    {
        virtio_gpu_resource_create_2d_t* cmd = (virtio_gpu_resource_create_2d_t*)g_vgpu.ctrl_cmd;
        memset(cmd, 0, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cmd->resource_id = g_vgpu.resource_id;
        cmd->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        cmd->width = g_vgpu.fb.width;
        cmd->height = g_vgpu.fb.height;

        if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
            spinlock_release(&g_vgpu.lock);
            vgpu_cleanup_state();
            return 0;
        }
    }

    {
        virtio_gpu_resource_attach_backing_1_t* cmd = (virtio_gpu_resource_attach_backing_1_t*)g_vgpu.ctrl_cmd;
        memset(cmd, 0, sizeof(*cmd));
        cmd->req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        cmd->req.resource_id = g_vgpu.resource_id;
        cmd->req.nr_entries = 1;
        cmd->entry.addr = (uint64_t)g_vgpu.fb.fb_phys;
        cmd->entry.length = g_vgpu.fb.size_bytes;
        cmd->entry.padding = 0;

        if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
            spinlock_release(&g_vgpu.lock);
            vgpu_cleanup_state();
            return 0;
        }
    }

    spinlock_release(&g_vgpu.lock);

    g_vgpu.scanout_bound_resource_id = 0;
    memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
    g_vgpu.active = 1;

    virtio_pci_add_status(&g_vgpu.dev, VIRTIO_STATUS_DRIVER_OK);
    return 1;
}

int virtio_gpu_is_active(void) {
    return g_vgpu.active;
}

const virtio_gpu_fb_t* virtio_gpu_get_fb(void) {
    return g_vgpu.active ? &g_vgpu.fb : 0;
}

uint32_t virtio_gpu_get_scanout_id(void) {
    return g_vgpu.active ? g_vgpu.scanout_id : 0u;
}

uint32_t virtio_gpu_get_primary_resource_id(void) {
    return g_vgpu.active ? g_vgpu.resource_id : 0u;
}

int virtio_gpu_virgl_is_supported(void) {
    return g_vgpu.active && g_vgpu.virgl_supported;
}

int virtio_gpu_flush_rect(int x, int y, int w, int h) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active || !g_vgpu.fb.fb_ptr) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (g_vgpu.scanout_bound_resource_id != g_vgpu.resource_id ||
        g_vgpu.scanout_bound_rect.x != 0u ||
        g_vgpu.scanout_bound_rect.y != 0u ||
        g_vgpu.scanout_bound_rect.width != g_vgpu.fb.width ||
        g_vgpu.scanout_bound_rect.height != g_vgpu.fb.height) {
        virtio_gpu_set_scanout_t* cmd = (virtio_gpu_set_scanout_t*)g_vgpu.ctrl_cmd;
        memset(cmd, 0, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        cmd->r.x = 0;
        cmd->r.y = 0;
        cmd->r.width = g_vgpu.fb.width;
        cmd->r.height = g_vgpu.fb.height;
        cmd->scanout_id = g_vgpu.scanout_id;
        cmd->resource_id = g_vgpu.resource_id;

        if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
            g_vgpu.active = 0;
            g_vgpu.scanout_bound_resource_id = 0;
            memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
            spinlock_release(&g_vgpu.lock);
            return -1;
        }

        g_vgpu.scanout_bound_resource_id = g_vgpu.resource_id;
        g_vgpu.scanout_bound_rect.x = 0u;
        g_vgpu.scanout_bound_rect.y = 0u;
        g_vgpu.scanout_bound_rect.width = g_vgpu.fb.width;
        g_vgpu.scanout_bound_rect.height = g_vgpu.fb.height;
    }

    if (w <= 0 || h <= 0) {
        spinlock_release(&g_vgpu.lock);
        return 0;
    }

    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)g_vgpu.fb.width) x2 = (int)g_vgpu.fb.width;
    if (y2 > (int)g_vgpu.fb.height) y2 = (int)g_vgpu.fb.height;

    if (x1 >= x2 || y1 >= y2) {
        spinlock_release(&g_vgpu.lock);
        return 0;
    }

    __asm__ volatile("sfence" ::: "memory");

    uint64_t offset64 = (uint64_t)(uint32_t)y1 * (uint64_t)g_vgpu.fb.pitch + (uint64_t)(uint32_t)x1 * 4ull;
    if (offset64 > (uint64_t)g_vgpu.fb.size_bytes) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_transfer_to_host_2d_t* t = (virtio_gpu_transfer_to_host_2d_t*)g_vgpu.ctrl_cmd;
    memset(t, 0, sizeof(*t));
    t->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t->r.x = (uint32_t)x1;
    t->r.y = (uint32_t)y1;
    t->r.width = (uint32_t)(x2 - x1);
    t->r.height = (uint32_t)(y2 - y1);
    t->offset = offset64;
    t->resource_id = g_vgpu.resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*t), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        g_vgpu.active = 0;
        g_vgpu.scanout_bound_resource_id = 0;
        memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_resource_flush_t* f = (virtio_gpu_resource_flush_t*)g_vgpu.ctrl_cmd;
    memset(f, 0, sizeof(*f));
    f->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    f->r.x = (uint32_t)x1;
    f->r.y = (uint32_t)y1;
    f->r.width = (uint32_t)(x2 - x1);
    f->r.height = (uint32_t)(y2 - y1);
    f->resource_id = g_vgpu.resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*f), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        g_vgpu.active = 0;
        g_vgpu.scanout_bound_resource_id = 0;
        memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

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
                                  uint32_t flags) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active || !g_vgpu.virgl_supported) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_resource_create_3d_t* cmd = (virtio_gpu_resource_create_3d_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd->resource_id = resource_id;
    cmd->target = target;
    cmd->format = format;
    cmd->bind = bind;
    cmd->width = width;
    cmd->height = height;
    cmd->depth = depth;
    cmd->array_size = array_size;
    cmd->last_level = last_level;
    cmd->nr_samples = nr_samples;
    cmd->flags = flags;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

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
                                   uint64_t offset) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active || !g_vgpu.virgl_supported) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_transfer_host_3d_t* cmd = (virtio_gpu_transfer_host_3d_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd->box.x = x;
    cmd->box.y = y;
    cmd->box.z = z;
    cmd->box.w = w;
    cmd->box.h = h;
    cmd->box.d = d;
    cmd->offset = offset;
    cmd->resource_id = resource_id;
    cmd->level = level;
    cmd->stride = stride;
    cmd->layer_stride = layer_stride;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_virgl_resource_attach(uint32_t resource_id) {
    spinlock_acquire(&g_vgpu.lock);
    int ok = vgpu_virgl_attach_resource_locked(resource_id);
    spinlock_release(&g_vgpu.lock);
    return ok ? 0 : -1;
}

int virtio_gpu_virgl_resource_detach(uint32_t resource_id) {
    spinlock_acquire(&g_vgpu.lock);
    int ok = vgpu_virgl_detach_resource_locked(resource_id);
    spinlock_release(&g_vgpu.lock);
    return ok ? 0 : -1;
}

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
                                 uint32_t depth) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active || !g_vgpu.virgl_supported) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (!vgpu_virgl_attach_resource_locked(dst_resource_id) || !vgpu_virgl_attach_resource_locked(src_resource_id)) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_cmd_submit_3d_t* cmd = (virtio_gpu_cmd_submit_3d_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = g_vgpu.virgl_ctx_id;

    uint32_t* stream = (uint32_t*)((uint8_t*)g_vgpu.ctrl_cmd + sizeof(*cmd));
    stream[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0u, VIRGL_CMD_RCR_PAYLOAD_DWORDS);
    stream[1] = dst_resource_id;
    stream[2] = dst_level;
    stream[3] = dst_x;
    stream[4] = dst_y;
    stream[5] = dst_z;
    stream[6] = src_resource_id;
    stream[7] = src_level;
    stream[8] = src_x;
    stream[9] = src_y;
    stream[10] = src_z;
    stream[11] = width;
    stream[12] = height;
    stream[13] = depth;

    cmd->size = (1u + VIRGL_CMD_RCR_PAYLOAD_DWORDS) * 4u;

    uint32_t total_len = (uint32_t)sizeof(*cmd) + cmd->size;
    if (total_len > PAGE_SIZE) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (!vgpu_ctrlq_submit_locked(total_len, sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_resource_attach_phys_pages(uint32_t resource_id,
                                         const uint32_t* phys_pages,
                                         uint32_t page_count,
                                         uint32_t page_offset,
                                         uint32_t size_bytes) {
    if (!phys_pages || page_count == 0u || size_bytes == 0u) return -1;

    uint64_t off = (uint64_t)page_offset;
    uint64_t end = off + (uint64_t)size_bytes;
    if (end < off) return -1;

    uint64_t total_bytes = (uint64_t)page_count * (uint64_t)PAGE_SIZE;
    if (end > total_bytes) return -1;

    uint32_t start_i = (uint32_t)(off >> PAGE_SHIFT);
    uint32_t in_page = (uint32_t)(off & (uint64_t)(PAGE_SIZE - 1u));
    if (start_i >= page_count) return -1;

    uint64_t span64 = ((uint64_t)in_page + (uint64_t)size_bytes + (uint64_t)PAGE_SIZE - 1ull) >> PAGE_SHIFT;
    if (span64 == 0ull) return -1;
    if (span64 > (uint64_t)page_count - (uint64_t)start_i) return -1;

    uint32_t span_pages = (uint32_t)span64;
    uint64_t entries_bytes64 = (uint64_t)span_pages * (uint64_t)sizeof(virtio_gpu_mem_entry_t);
    if (entries_bytes64 == 0ull || entries_bytes64 > 0xFFFFFFFFull) return -1;

    uint32_t entries_order = vgpu_pages_order_for_bytes((uint32_t)entries_bytes64);
    if (entries_order > PMM_MAX_ORDER) return -1;

    virtio_gpu_mem_entry_t* entries = (virtio_gpu_mem_entry_t*)pmm_alloc_pages(entries_order);
    if (!entries) return -1;

    memset(entries, 0, (size_t)PAGE_SIZE << entries_order);

    uint32_t end_i = start_i + span_pages;
    if (phys_pages[start_i] == 0u || (phys_pages[start_i] & (PAGE_SIZE - 1u)) != 0u) {
        pmm_free_pages(entries, entries_order);
        return -1;
    }

    uint32_t idx = start_i;
    uint32_t remaining = size_bytes;
    uint32_t entry_count = 0;

    uint64_t seg_addr = (uint64_t)phys_pages[idx] + (uint64_t)in_page;
    uint32_t seg_len = PAGE_SIZE - in_page;
    if (seg_len > remaining) seg_len = remaining;

    remaining -= seg_len;
    idx++;

    while (remaining > 0u) {
        if (idx >= end_i) {
            pmm_free_pages(entries, entries_order);
            return -1;
        }

        if (phys_pages[idx] == 0u || (phys_pages[idx] & (PAGE_SIZE - 1u)) != 0u) {
            pmm_free_pages(entries, entries_order);
            return -1;
        }

        uint64_t next_addr = (uint64_t)phys_pages[idx];
        uint32_t next_len = remaining > PAGE_SIZE ? PAGE_SIZE : remaining;

        uint64_t seg_end = seg_addr + (uint64_t)seg_len;
        if (seg_end == next_addr && (uint64_t)seg_len + (uint64_t)next_len <= 0xFFFFFFFFull) {
            seg_len += next_len;
        } else {
            if (entry_count >= span_pages) {
                pmm_free_pages(entries, entries_order);
                return -1;
            }
            entries[entry_count].addr = seg_addr;
            entries[entry_count].length = seg_len;
            entries[entry_count].padding = 0;
            entry_count++;

            seg_addr = next_addr;
            seg_len = next_len;
        }

        remaining -= next_len;
        idx++;
    }

    if (entry_count >= span_pages) {
        pmm_free_pages(entries, entries_order);
        return -1;
    }
    entries[entry_count].addr = seg_addr;
    entries[entry_count].length = seg_len;
    entries[entry_count].padding = 0;
    entry_count++;

    uint64_t entries_len64 = (uint64_t)entry_count * (uint64_t)sizeof(virtio_gpu_mem_entry_t);
    if (entries_len64 == 0ull || entries_len64 > 0xFFFFFFFFull) {
        pmm_free_pages(entries, entries_order);
        return -1;
    }

    spinlock_acquire(&g_vgpu.lock);
    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        pmm_free_pages(entries, entries_order);
        return -1;
    }

    virtio_gpu_resource_attach_backing_t* cmd = (virtio_gpu_resource_attach_backing_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = resource_id;
    cmd->nr_entries = entry_count;

    uint64_t addrs[3] = {
        (uint64_t)g_vgpu.ctrl_cmd_phys,
        (uint64_t)(uint32_t)(uintptr_t)entries,
        (uint64_t)g_vgpu.ctrl_resp_phys,
    };

    uint32_t lens[3] = {
        (uint32_t)sizeof(*cmd),
        (uint32_t)entries_len64,
        (uint32_t)sizeof(virtio_gpu_ctrl_hdr_t),
    };

    uint16_t flags[3] = {
        0,
        0,
        VRING_DESC_F_WRITE,
    };

    int ok = vgpu_ctrlq_submit_sg_locked(addrs, lens, flags, 3u, VIRTIO_GPU_RESP_OK_NODATA);
    if (!ok) {
        vgpu_mark_inactive_locked();
    }

    spinlock_release(&g_vgpu.lock);
    pmm_free_pages(entries, entries_order);
    return ok ? 0 : -1;
}

int virtio_gpu_set_scanout(uint32_t scanout_id,
                           uint32_t resource_id,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_set_scanout_t* cmd = (virtio_gpu_set_scanout_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->scanout_id = scanout_id;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    g_vgpu.scanout_bound_resource_id = resource_id;
    g_vgpu.scanout_bound_rect.x = x;
    g_vgpu.scanout_bound_rect.y = y;
    g_vgpu.scanout_bound_rect.width = width;
    g_vgpu.scanout_bound_rect.height = height;

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_transfer_to_host_2d(uint32_t resource_id,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t width,
                                   uint32_t height,
                                   uint64_t offset) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_transfer_to_host_2d_t* cmd = (virtio_gpu_transfer_to_host_2d_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->offset = offset;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_resource_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_resource_flush_t* cmd = (virtio_gpu_resource_flush_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_resource_detach_backing(uint32_t resource_id) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (g_vgpu.virgl_supported) {
        (void)vgpu_virgl_detach_resource_locked(resource_id);
    }

    virtio_gpu_resource_id_cmd_t* cmd = (virtio_gpu_resource_id_cmd_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (g_vgpu.scanout_bound_resource_id == resource_id) {
        g_vgpu.scanout_bound_resource_id = 0;
        memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_resource_unref(uint32_t resource_id) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (g_vgpu.virgl_supported) {
        (void)vgpu_virgl_detach_resource_locked(resource_id);
    }

    virtio_gpu_resource_id_cmd_t* cmd = (virtio_gpu_resource_id_cmd_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->resource_id = resource_id;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    if (g_vgpu.scanout_bound_resource_id == resource_id) {
        g_vgpu.scanout_bound_resource_id = 0;
        memset(&g_vgpu.scanout_bound_rect, 0, sizeof(g_vgpu.scanout_bound_rect));
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}

int virtio_gpu_resource_create_2d(uint32_t resource_id, uint32_t format, uint32_t width, uint32_t height) {
    spinlock_acquire(&g_vgpu.lock);

    if (!g_vgpu.active) {
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    virtio_gpu_resource_create_2d_t* cmd = (virtio_gpu_resource_create_2d_t*)g_vgpu.ctrl_cmd;
    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id = resource_id;
    cmd->format = format;
    cmd->width = width;
    cmd->height = height;

    if (!vgpu_ctrlq_submit_locked(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        vgpu_mark_inactive_locked();
        spinlock_release(&g_vgpu.lock);
        return -1;
    }

    spinlock_release(&g_vgpu.lock);
    return 0;
}
