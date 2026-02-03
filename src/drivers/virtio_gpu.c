#include <drivers/virtio_gpu.h>
#include <drivers/virtio_pci.h>
#include <drivers/virtqueue.h>
#include <hal/lock.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <arch/i386/paging.h>

extern volatile uint32_t timer_ticks;

#define VIRTIO_GPU_PCI_DEVICE_ID 0x1050u
#define VIRTIO_GPU_MSI_VECTOR 0xA2u
#define VIRTIO_GPU_QUEUE_CTRL 0u

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u

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

typedef struct {
    int active;
    virtio_pci_dev_t dev;
    virtqueue_t ctrlq;
    spinlock_t lock;
    uint32_t scanout_id;
    int scanout_set;
    uint32_t resource_id;
    uint32_t backing_order;
    void* ctrl_cmd;
    void* ctrl_resp;
    uint32_t ctrl_cmd_phys;
    uint32_t ctrl_resp_phys;
    virtio_gpu_fb_t fb;
} virtio_gpu_state_t;

static virtio_gpu_state_t g_vgpu;

#define VGPU_CTRLQ_QSZ 64u
#define VGPU_CTRLQ_TIMEOUT_TICKS 30000u
#define VGPU_CTRLQ_TIMEOUT_SPINS 20000000u

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

static void vgpu_cleanup_state(void) {
    g_vgpu.active = 0;
    g_vgpu.scanout_set = 0;

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
    g_vgpu.resource_id = 0;
}

static int vgpu_ctrlq_submit(uint32_t cmd_len, uint32_t resp_len, uint32_t expected_resp_type) {
    if (!g_vgpu.ctrlq.ring_mem) return 0;
    if (!g_vgpu.ctrl_cmd || !g_vgpu.ctrl_resp) return 0;
    if (g_vgpu.ctrl_cmd_phys == 0 || g_vgpu.ctrl_resp_phys == 0) return 0;
    if (cmd_len == 0 || resp_len == 0) return 0;

    spinlock_acquire(&g_vgpu.lock);

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
        spinlock_release(&g_vgpu.lock);
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
            spinlock_release(&g_vgpu.lock);
            return 0;
        }

        __asm__ volatile("pause");
    }

    virtqueue_token_destroy(token);
    spinlock_release(&g_vgpu.lock);

    const virtio_gpu_ctrl_hdr_t* rhdr = (const virtio_gpu_ctrl_hdr_t*)g_vgpu.ctrl_resp;
    return rhdr->type == expected_resp_type;
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
    if (!virtio_pci_negotiate_features(&g_vgpu.dev, VIRTIO_F_VERSION_1, &accepted)) {
        vgpu_cleanup_state();
        return 0;
    }
    (void)accepted;

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

    {
        virtio_gpu_resource_create_2d_t* cmd = (virtio_gpu_resource_create_2d_t*)g_vgpu.ctrl_cmd;
        memset(cmd, 0, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cmd->resource_id = g_vgpu.resource_id;
        cmd->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        cmd->width = g_vgpu.fb.width;
        cmd->height = g_vgpu.fb.height;

        if (!vgpu_ctrlq_submit(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
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

        if (!vgpu_ctrlq_submit(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
            vgpu_cleanup_state();
            return 0;
        }
    }

    g_vgpu.scanout_set = 0;
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

int virtio_gpu_flush_rect(int x, int y, int w, int h) {
    if (!g_vgpu.active) return -1;
    if (!g_vgpu.fb.fb_ptr) return -1;

    if (!g_vgpu.scanout_set) {
        virtio_gpu_set_scanout_t* cmd = (virtio_gpu_set_scanout_t*)g_vgpu.ctrl_cmd;
        memset(cmd, 0, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        cmd->r.x = 0;
        cmd->r.y = 0;
        cmd->r.width = g_vgpu.fb.width;
        cmd->r.height = g_vgpu.fb.height;
        cmd->scanout_id = g_vgpu.scanout_id;
        cmd->resource_id = g_vgpu.resource_id;

        if (!vgpu_ctrlq_submit(sizeof(*cmd), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
            g_vgpu.active = 0;
            g_vgpu.scanout_set = 0;
            return -1;
        }

        g_vgpu.scanout_set = 1;
    }

    if (w <= 0 || h <= 0) return 0;

    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)g_vgpu.fb.width) x2 = (int)g_vgpu.fb.width;
    if (y2 > (int)g_vgpu.fb.height) y2 = (int)g_vgpu.fb.height;

    if (x1 >= x2 || y1 >= y2) return 0;

    __asm__ volatile("sfence" ::: "memory");

    uint64_t offset64 = (uint64_t)(uint32_t)y1 * (uint64_t)g_vgpu.fb.pitch + (uint64_t)(uint32_t)x1 * 4ull;
    if (offset64 > (uint64_t)g_vgpu.fb.size_bytes) {
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

    if (!vgpu_ctrlq_submit(sizeof(*t), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        g_vgpu.active = 0;
        g_vgpu.scanout_set = 0;
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

    if (!vgpu_ctrlq_submit(sizeof(*f), sizeof(virtio_gpu_ctrl_hdr_t), VIRTIO_GPU_RESP_OK_NODATA)) {
        g_vgpu.active = 0;
        g_vgpu.scanout_set = 0;
        return -1;
    }

    return 0;
}
