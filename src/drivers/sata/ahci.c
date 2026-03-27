/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/block/bdev.h>
#include <drivers/pci/pci.h>
#include <drivers/driver.h>

#include <mm/iomem.h>
#include <mm/heap.h>
#include <mm/dma.h>

#include <kernel/workqueue.h>
#include <kernel/smp/cpu.h>

#include <lib/string.h>
#include <lib/dlist.h>

#include <hal/irq.h>

#include "ahci.h"

#define AHCI_HBA_CAP  0x00u
#define AHCI_HBA_GHC  0x04u
#define AHCI_HBA_IS   0x08u
#define AHCI_HBA_PI   0x0Cu
#define AHCI_HBA_VS   0x10u

#define AHCI_PORT_BASE(p) (0x100u + ((p) * 0x80u))
#define AHCI_PORT_CLB(p)  (AHCI_PORT_BASE(p) + 0x00u)
#define AHCI_PORT_CLBU(p) (AHCI_PORT_BASE(p) + 0x04u)
#define AHCI_PORT_FB(p)   (AHCI_PORT_BASE(p) + 0x08u)
#define AHCI_PORT_FBU(p)  (AHCI_PORT_BASE(p) + 0x0Cu)
#define AHCI_PORT_IS(p)   (AHCI_PORT_BASE(p) + 0x10u)
#define AHCI_PORT_IE(p)   (AHCI_PORT_BASE(p) + 0x14u)
#define AHCI_PORT_CMD(p)  (AHCI_PORT_BASE(p) + 0x18u)
#define AHCI_PORT_TFD(p)  (AHCI_PORT_BASE(p) + 0x20u)
#define AHCI_PORT_SIG(p)  (AHCI_PORT_BASE(p) + 0x24u)
#define AHCI_PORT_SSTS(p) (AHCI_PORT_BASE(p) + 0x28u)
#define AHCI_PORT_SCTL(p) (AHCI_PORT_BASE(p) + 0x2Cu)
#define AHCI_PORT_SERR(p) (AHCI_PORT_BASE(p) + 0x30u)
#define AHCI_PORT_SACT(p) (AHCI_PORT_BASE(p) + 0x34u)
#define AHCI_PORT_CI(p)   (AHCI_PORT_BASE(p) + 0x38u)

#define HBA_PxCMD_ST 0x0001
#define HBA_PxCMD_FRE 0x0010
#define HBA_PxCMD_FR 0x4000
#define HBA_PxCMD_CR 0x8000

#define HBA_GHC_AE (1 << 31)
#define HBA_GHC_IE (1 << 1)

#define AHCI_DEV_BUSY (1 << 7)
#define AHCI_DEV_DRQ (1 << 3)
#define AHCI_DEV_ERR (1 << 0)

#define ATA_CMD_READ_DMA_EX 0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY 0xEC

#define AHCI_SECTOR_SIZE 512u
#define AHCI_MAX_IO_SECTORS 2048u
#define AHCI_MAX_PRDT_ENTRIES 248u

#define AHCI_MAX_PORTS 32

typedef struct ahci_hba_s ahci_hba_t;

typedef struct {
    ahci_port_state_t base;

    void* identify_buf_virt;
    uint32_t identify_buf_phys;

    int port_no;
    uint64_t sector_count;

    block_device_t* bdev;
    char* bdev_name;

    ahci_hba_t* hba;
} ahci_port_extended_t;

struct ahci_hba_s {
    __iomem* iomem;

    pci_device_t* pdev;

    uint8_t irq_line;
    int msi_enabled;
    uint8_t msi_vector;

    ahci_port_extended_t* ports[AHCI_MAX_PORTS];
    uint32_t port_mask;

    volatile uint32_t port_active_slots[AHCI_MAX_PORTS];

    dlist_head_t list_node;

    workqueue_t* wq;
    work_struct_t irq_work;
};

static dlist_head_t g_ahci_hba_list;
static spinlock_t g_ahci_hba_list_lock;

static int g_ahci_async_mode = 0;

static uint32_t g_ahci_disk_seq = 0;
static block_device_t* g_ahci_first_bdev = 0;

static void stop_cmd(ahci_hba_t* hba, int port_no);
static void start_cmd(ahci_hba_t* hba, int port_no);

static inline uint32_t ahci_read(ahci_hba_t* hba, uint32_t offset) {
    return ioread32(hba->iomem, offset);
}

static inline void ahci_write(ahci_hba_t* hba, uint32_t offset, uint32_t value) {
    iowrite32(hba->iomem, offset, value);
}

static inline uint32_t ahci_hba_port_read(ahci_hba_t* hba, int port_no, uint32_t offset) {
    (void)port_no;
    return ioread32(hba->iomem, offset);
}

static inline void ahci_hba_port_write(ahci_hba_t* hba, int port_no, uint32_t offset, uint32_t value) {
    (void)port_no;
    iowrite32(hba->iomem, offset, value);
}

static ahci_port_extended_t* ahci_first_port(void) {
    if (!g_ahci_first_bdev || !g_ahci_first_bdev->private_data) {
        return 0;
    }

    return (ahci_port_extended_t*)g_ahci_first_bdev->private_data;
}

static void ahci_hba_list_add(ahci_hba_t* hba) {
    spinlock_acquire(&g_ahci_hba_list_lock);

    dlist_add_tail(&hba->list_node, &g_ahci_hba_list);
    
    spinlock_release(&g_ahci_hba_list_lock);
}

static void ahci_hba_list_remove(ahci_hba_t* hba) {
    spinlock_acquire(&g_ahci_hba_list_lock);

    if (hba->list_node.next && hba->list_node.prev) {
        dlist_del(&hba->list_node);

        hba->list_node.next = 0;
        hba->list_node.prev = 0;
    }

    spinlock_release(&g_ahci_hba_list_lock);
}

static ahci_hba_t* ahci_hba_from_pdev(pci_device_t* pdev) {
    ahci_hba_t* found = 0;

    spinlock_acquire(&g_ahci_hba_list_lock);

    ahci_hba_t* hba;
    dlist_for_each_entry(hba, &g_ahci_hba_list, list_node) {
        if (hba->pdev == pdev) {
            found = hba;
            break;
        }
    }

    spinlock_release(&g_ahci_hba_list_lock);

    return found;
}

static int ahci_bdev_read_sectors(block_device_t* dev, uint64_t lba, uint32_t count, void* buf);
static int ahci_bdev_write_sectors(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf);

static int ahci_bdev_flush(block_device_t* dev);

static const block_ops_t g_ahci_bdev_ops = {
    .read_sectors = ahci_bdev_read_sectors,
    .write_sectors = ahci_bdev_write_sectors,

    .flush = ahci_bdev_flush,
};

static int ahci_send_command(
    ahci_port_extended_t* ex, uint32_t lba,
    uint8_t* buf, int is_write, uint32_t count
);

static uint64_t ahci_identify_device(ahci_port_extended_t* ex);

static int ahci_create_and_register_bdev(ahci_port_extended_t* ex);
static void ahci_unregister_and_destroy_bdev(ahci_port_extended_t* ex);

static void ahci_port_destroy(ahci_port_extended_t* ex);
static ahci_port_extended_t* ahci_port_create(ahci_hba_t* hba, int port_no);

static void ahci_hba_destroy(ahci_hba_t* hba);
static int ahci_port_comreset(ahci_port_extended_t* ex);

static int ahci_rw_sectors(
    ahci_port_extended_t* ex, uint64_t lba,
    uint32_t count, uint8_t* buf, int is_write
);

static char* ahci_alloc_disk_name(uint32_t index) {
    char tmp[16];
    uint32_t v = index;
    uint32_t n = 0;

    tmp[n++] = 's';
    tmp[n++] = 'd';

    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        char digits[10];
        uint32_t d = 0;

        while (v != 0u && d < (uint32_t)sizeof(digits)) {
            digits[d++] = (char)('0' + (v % 10u));
            v /= 10u;
        }

        while (d > 0u) {
            tmp[n++] = digits[--d];
        }
    }

    tmp[n++] = '\0';

    char* out = (char*)kmalloc((size_t)n);
    if (!out) {
        return 0;
    }

    memcpy(out, tmp, (size_t)n);
    return out;
}

static int ahci_create_and_register_bdev(ahci_port_extended_t* ex) {
    if (!ex) {
        return 0;
    }

    if (ex->bdev || ex->bdev_name) {
        return 0;
    }

    if (ex->sector_count == 0u) {
        return 0;
    }

    block_device_t* bdev = (block_device_t*)kzalloc(sizeof(*bdev));
    if (!bdev) {
        return 0;
    }

    uint32_t disk_index = __sync_fetch_and_add(&g_ahci_disk_seq, 1u);
    char* name = ahci_alloc_disk_name(disk_index);
    if (!name) {
        kfree(bdev);
        return 0;
    }

    bdev->name = name;
    bdev->sector_size = 512;
    bdev->sector_count = ex->sector_count;
    bdev->ops = &g_ahci_bdev_ops;
    bdev->private_data = ex;

    if (bdev_register(bdev) != 0) {
        kfree(name);
        kfree(bdev);
        return 0;
    }

    ex->bdev = bdev;
    ex->bdev_name = name;

    if (!g_ahci_first_bdev) {
        g_ahci_first_bdev = bdev;
    }

    return 1;
}

static void ahci_unregister_and_destroy_bdev(ahci_port_extended_t* ex) {
    if (!ex || !ex->bdev) {
        return;
    }

    if (ex->bdev_name) {
        (void)bdev_unregister(ex->bdev_name);
    }

    if (g_ahci_first_bdev == ex->bdev) {
        g_ahci_first_bdev = 0;
    }

    if (ex->bdev_name) {
        kfree(ex->bdev_name);
        ex->bdev_name = 0;
    }

    kfree(ex->bdev);
    ex->bdev = 0;
}

static void ahci_port_destroy(ahci_port_extended_t* ex) {
    if (!ex) {
        return;
    }

    ahci_hba_t* hba = ex->hba;
    int port_no = ex->port_no;

    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    if (state->active && hba) {
        stop_cmd(hba, port_no);
    }

    for (int i = 0; i < 32; i++) {
        if (state->ctba_virt[i]) {
            dma_free_coherent(state->ctba_virt[i], 4096, dma_virt_to_phys(state->ctba_virt[i]));

            state->ctba_virt[i] = 0;
        }
    }

    if (ex->identify_buf_virt) {
        dma_free_coherent(ex->identify_buf_virt, AHCI_SECTOR_SIZE, ex->identify_buf_phys);

        ex->identify_buf_virt = 0;
    }

    ex->identify_buf_phys = 0;

    if (state->fb_virt) {
        dma_free_coherent(state->fb_virt, 256, dma_virt_to_phys(state->fb_virt));

        state->fb_virt = 0;
    }

    if (state->clb_virt) {
        dma_free_coherent(state->clb_virt, 1024, dma_virt_to_phys(state->clb_virt));

        state->clb_virt = 0;
    }

    state->active = 0;

    if (hba && port_no >= 0 && port_no < AHCI_MAX_PORTS) {
        hba->ports[port_no] = 0;
        hba->port_mask &= ~(1u << port_no);
        hba->port_active_slots[port_no] = 0;
    }

    kfree(ex);
}

static int ahci_bdev_read_sectors(block_device_t* dev, uint64_t lba, uint32_t count, void* buf) {
    if (!buf) {
        return 0;
    }

    if (lba > 0xFFFFFFFFull) {
        return 0;
    }

    if (!dev || !dev->private_data) {
        return 0;
    }

    ahci_port_extended_t* ex = (ahci_port_extended_t*)dev->private_data;
    return ahci_rw_sectors(ex, lba, count, (uint8_t*)buf, 0) != 0;
}

static int ahci_bdev_write_sectors(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    if (!buf) {
        return 0;
    }

    if (lba > 0xFFFFFFFFull) {
        return 0;
    }

    if (!dev || !dev->private_data) {
        return 0;
    }

    ahci_port_extended_t* ex = (ahci_port_extended_t*)dev->private_data;
    return ahci_rw_sectors(ex, lba, count, (uint8_t*)buf, 1) != 0;
}

static int ahci_bdev_flush(block_device_t* dev) {
    (void)dev;
    return 1;
}

static int ahci_rw_sectors(
    ahci_port_extended_t* ex, uint64_t lba,
    uint32_t count, uint8_t* buf, int is_write
) {
    if (!ex || !buf) {
        return 0;
    }

    if (count == 0u) {
        return 1;
    }

    if (lba > 0xFFFFFFFFull) {
        return 0;
    }

    uint64_t end_lba = lba + (uint64_t)count;
    if (end_lba > ex->sector_count) {
        return 0;
    }

    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    uint8_t* cur_buf = buf;

    while (remaining > 0u) {
        uint32_t chunk = remaining;
        if (chunk > AHCI_MAX_IO_SECTORS) {
            chunk = AHCI_MAX_IO_SECTORS;
        }

        uint32_t ok = (uint32_t)ahci_send_command(
            ex,
            (uint32_t)cur_lba,
            cur_buf,
            is_write,
            chunk
        );

        if (!ok) {
            return 0;
        }

        cur_lba += (uint64_t)chunk;
        cur_buf += chunk * AHCI_SECTOR_SIZE;
        remaining -= chunk;
    }

    return 1;
}

void ahci_set_async_mode(int enable) {
    g_ahci_async_mode = enable;
}

int ahci_msi_configure_cpu(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= cpu_count) {
        return 0;
    }

    if (cpus[cpu_index].id < 0) {
        return 0;
    }

    spinlock_acquire(&g_ahci_hba_list_lock);
    
    ahci_hba_t* hba = 0;
    
    if (!dlist_empty(&g_ahci_hba_list)) {
        hba = container_of(g_ahci_hba_list.next, ahci_hba_t, list_node);
    }

    spinlock_release(&g_ahci_hba_list_lock);

    if (!hba || !hba->pdev) {
        return 0;
    }

    const int ok = pci_dev_enable_msi(
        hba->pdev, hba->msi_vector,
        (uint8_t)cpus[cpu_index].id
    );

    if (ok) {
        hba->msi_enabled = 1;
    }

    return ok;
}

static int find_cmdslot(ahci_hba_t* hba, int port_no) {
    const uint32_t sact = ahci_hba_port_read(hba, port_no, AHCI_PORT_SACT(port_no));
    const uint32_t ci = ahci_hba_port_read(hba, port_no, AHCI_PORT_CI(port_no));
    const uint32_t active = hba->port_active_slots[port_no];

    const uint32_t slots = sact | ci | active;

    if (slots == 0xFFFFFFFFu) {
        return -1;
    }

    const uint32_t free_mask = ~slots;
    uint32_t first_free_bit = 0u;

    __asm__ volatile("bsf %1, %0" : "=r"(first_free_bit) : "r"(free_mask));

    return (int)first_free_bit;
}

static void stop_cmd(ahci_hba_t* hba, int port_no) {
    uint32_t cmd = ahci_hba_port_read(hba, port_no, AHCI_PORT_CMD(port_no));
    cmd &= ~HBA_PxCMD_ST;
    cmd &= ~HBA_PxCMD_FRE;
    ahci_hba_port_write(hba, port_no, AHCI_PORT_CMD(port_no), cmd);

    int timeout = 1000000;

    while (timeout--) {
        const uint32_t curr_cmd = ahci_hba_port_read(hba, port_no, AHCI_PORT_CMD(port_no));
        
        if ((curr_cmd & HBA_PxCMD_FR) != 0u || (curr_cmd & HBA_PxCMD_CR) != 0u) {
            continue;
        }

        break;
    }
}

static void start_cmd(ahci_hba_t* hba, int port_no) {
    while ((ahci_hba_port_read(hba, port_no, AHCI_PORT_CMD(port_no)) & HBA_PxCMD_CR) != 0u) {
        __asm__ volatile("pause" ::: "memory");
    }

    uint32_t cmd = ahci_hba_port_read(hba, port_no, AHCI_PORT_CMD(port_no));
    cmd |= HBA_PxCMD_FRE;
    cmd |= HBA_PxCMD_ST;
    ahci_hba_port_write(hba, port_no, AHCI_PORT_CMD(port_no), cmd);
}

static void ahci_irq_bh(work_struct_t* work) {
    ahci_hba_t* hba = container_of(work, ahci_hba_t, irq_work);

    if (!hba->iomem) return;

    uint32_t is_glob = ioread32(hba->iomem, AHCI_HBA_IS);

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((is_glob & (1u << i)) == 0u) {
            continue;
        }

        ahci_port_extended_t* ex = hba->ports[i];
        if (!ex) {
            uint32_t p_is = ioread32(hba->iomem, AHCI_PORT_IS(i));
            iowrite32(hba->iomem, AHCI_PORT_IS(i), p_is);

            uint32_t p_serr = ioread32(hba->iomem, AHCI_PORT_SERR(i));
            if (p_serr) {
                iowrite32(hba->iomem, AHCI_PORT_SERR(i), 0xFFFFFFFFu);
            }
            continue;
        }

        ahci_port_state_t* state = (ahci_port_state_t*)ex;
        
        uint32_t p_is = ioread32(hba->iomem, AHCI_PORT_IS(i));
        iowrite32(hba->iomem, AHCI_PORT_IS(i), p_is);

        if (state->active && g_ahci_async_mode) {
            uint32_t active = hba->port_active_slots[i];
            uint32_t ci = ioread32(hba->iomem, AHCI_PORT_CI(i));
            uint32_t finished = active & ~ci;

            if (finished != 0u) {
                for (int s = 0; s < 32; s++) {
                    if ((finished & (1u << s)) != 0u) {
                        sem_signal(&state->slot_sem[s]);
                    }
                }
            }
        }

        uint32_t p_serr = ioread32(hba->iomem, AHCI_PORT_SERR(i));
        if (p_serr) {
            iowrite32(hba->iomem, AHCI_PORT_SERR(i), 0xFFFFFFFFu);
        }
    }

    uint32_t ghc = ioread32(hba->iomem, AHCI_HBA_GHC);
    iowrite32(hba->iomem, AHCI_HBA_GHC, ghc | HBA_GHC_IE);
}

void ahci_irq_handler(registers_t* regs, void* ctx) {
    (void)regs;

    ahci_hba_t* hba = (ahci_hba_t*)ctx;

    if (!hba || !hba->iomem) {
        return;
    }

    uint32_t is_glob = ioread32(hba->iomem, AHCI_HBA_IS);

    if (is_glob == 0u) {
        return;
    }

    uint32_t ghc = ioread32(hba->iomem, AHCI_HBA_GHC);

    iowrite32(hba->iomem, AHCI_HBA_GHC, ghc & ~HBA_GHC_IE);

    queue_work(hba->wq, &hba->irq_work);
}

static ahci_port_extended_t* ahci_port_create(ahci_hba_t* hba, int port_no) {
    if (!hba || port_no < 0 || port_no >= AHCI_MAX_PORTS) {
        return 0;
    }

    if (hba->ports[port_no]) {
        return 0;
    }

    ahci_port_extended_t* ex = (ahci_port_extended_t*)kzalloc(sizeof(*ex));
    if (!ex) {
        return 0;
    }

    ahci_port_state_t* state = (ahci_port_state_t*)ex;
    stop_cmd(hba, port_no);

    uint32_t clb_phys = 0;

    state->clb_virt = dma_alloc_coherent(1024, &clb_phys);
    
    if (!state->clb_virt) {
        kfree(ex);
        return 0;
    }
    
    iowrite32(hba->iomem, AHCI_PORT_CLB(port_no), clb_phys);
    iowrite32(hba->iomem, AHCI_PORT_CLBU(port_no), 0u);

    uint32_t fb_phys = 0;
    
    state->fb_virt = dma_alloc_coherent(256, &fb_phys);
    
    if (!state->fb_virt) {
        dma_free_coherent(state->clb_virt, 1024, dma_virt_to_phys(state->clb_virt));
        
        kfree(ex);
        return 0;
    }

    iowrite32(hba->iomem, AHCI_PORT_FB(port_no), fb_phys);
    iowrite32(hba->iomem, AHCI_PORT_FBU(port_no), 0u);

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)state->clb_virt;

    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 0;

        uint32_t ctba_phys = 0;
        
        void* ctba_virt = dma_alloc_coherent(4096, &ctba_phys);
        
        if (!ctba_virt) {
            for (int j = 0; j < i; j++) {
                dma_free_coherent(state->ctba_virt[j], 4096, dma_virt_to_phys(state->ctba_virt[j]));
            }

            dma_free_coherent(state->fb_virt, 256, dma_virt_to_phys(state->fb_virt));
            dma_free_coherent(state->clb_virt, 1024, dma_virt_to_phys(state->clb_virt));
            
            kfree(ex);
            return 0;
        }

        state->ctba_virt[i] = ctba_virt;
        
        cmdheader[i].ctba = ctba_phys;
        cmdheader[i].ctbau = 0;

        sem_init(&state->slot_sem[i], 0);
    }

    ex->identify_buf_virt = dma_alloc_coherent(AHCI_SECTOR_SIZE, &ex->identify_buf_phys);

    if (!ex->identify_buf_virt) {
        for (int i = 0; i < 32; i++) {
            dma_free_coherent(state->ctba_virt[i], 4096, dma_virt_to_phys(state->ctba_virt[i]));
        }

        dma_free_coherent(state->fb_virt, 256, dma_virt_to_phys(state->fb_virt));
        dma_free_coherent(state->clb_virt, 1024, dma_virt_to_phys(state->clb_virt));
        
        kfree(ex);
        return 0;
    }

    spinlock_init(&state->lock);
    hba->port_active_slots[port_no] = 0;

    iowrite32(hba->iomem, AHCI_PORT_SERR(port_no), 0xFFFFFFFFu);
    iowrite32(hba->iomem, AHCI_PORT_IS(port_no), 0xFFFFFFFFu);
    iowrite32(hba->iomem, AHCI_PORT_IE(port_no), 0x7800002Fu);

    start_cmd(hba, port_no);

    state->active = 1;

    ex->port_no = port_no;
    ex->hba = hba;

    hba->ports[port_no] = ex;
    hba->port_mask |= (1u << port_no);

    return ex;
}

static uint64_t ahci_identify_device(ahci_port_extended_t* ex) {
    if (!ex) {
        return 0;
    }

    if (!ex->identify_buf_virt || ex->identify_buf_phys == 0u) {
        return 0;
    }

    ahci_hba_t* hba = ex->hba;
    const int port_no = ex->port_no;

    if (!hba || !hba->iomem) {
        return 0;
    }

    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    iowrite32(hba->iomem, AHCI_PORT_IS(port_no), 0xFFFFFFFFu);

    int slot = find_cmdslot(hba, port_no);
    if (slot == -1) {
        return 0;
    }

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);

    cmdheader += slot;
    cmdheader->cfl = sizeof(FIS_REG_H2D) / 4u;
    cmdheader->w = 0;
    cmdheader->prdtl = 1;
    cmdheader->c = 0;

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, 4096);

    cmdtbl->prdt_entry[0].dba = ex->identify_buf_phys;
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = 511;
    cmdtbl->prdt_entry[0].i = 1;

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_IDENTIFY;
    cmdfis->device = 0;

    int spin = 0;
    while ((ioread32(hba->iomem, AHCI_PORT_TFD(port_no)) & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) != 0u
        && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }

    iowrite32(hba->iomem, AHCI_PORT_CI(port_no), 1u << slot);

    while (1) {
        if ((ioread32(hba->iomem, AHCI_PORT_CI(port_no)) & (1u << slot)) == 0u) {
            break;
        }

        if ((ioread32(hba->iomem, AHCI_PORT_IS(port_no)) & (1u << 30)) != 0u) {
            return 0;
        }
    }

    uint16_t* identify_buf = (uint16_t*)ex->identify_buf_virt;

    uint64_t lba48_sectors = *(uint64_t*)&identify_buf[100];

    if (lba48_sectors != 0u) {
        return lba48_sectors;
    }

    uint32_t lba28_sectors = (uint32_t)identify_buf[60] | ((uint32_t)identify_buf[61] << 16);

    return (uint64_t)lba28_sectors;
}

static int ahci_send_command(
    ahci_port_extended_t* ex, uint32_t lba,
    uint8_t* buf, int is_write, uint32_t count
) {
    if (!ex) {
        return 0;
    }

    if (!buf) {
        return 0;
    }

    if (count == 0u
        || count > AHCI_MAX_IO_SECTORS) {
        return 0;
    }

    const int port_no = ex->port_no;

    if (port_no < 0
        || port_no >= 32) {
        return 0;
    }

    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    if (!state->active) {
        return 0;
    }

    ahci_hba_t* hba = ex->hba;
    if (!hba || !hba->iomem) {
        return 0;
    }

    const uint32_t byte_count = count * AHCI_SECTOR_SIZE;
    const uint32_t dma_dir = is_write ? DMA_DIR_TO_DEVICE : DMA_DIR_FROM_DEVICE;

    dma_sg_list_t* sg = dma_map_buffer(buf, byte_count, dma_dir);

    if (!sg) {
        return 0;
    }

    if (sg->count > AHCI_MAX_PRDT_ENTRIES) {
        dma_unmap_buffer(sg);

        return 0;
    }

    uint32_t eflags = get_eflags();

    const int can_wait_irq = ((eflags & 0x200u) != 0u);

    int result = 0;

    spinlock_acquire(&state->lock);

    if ((ioread32(hba->iomem, AHCI_PORT_IS(port_no)) & (1u << 30)) != 0u) {
        iowrite32(hba->iomem, AHCI_PORT_IS(port_no), 0xFFFFFFFFu);
        iowrite32(hba->iomem, AHCI_PORT_SERR(port_no), 0xFFFFFFFFu);
    }

    const int slot = find_cmdslot(hba, port_no);

    if (slot < 0
        || slot >= 32) {
        spinlock_release(&state->lock);
        dma_unmap_buffer(sg);

        return 0;
    }

    __sync_fetch_and_or(&ex->hba->port_active_slots[port_no], (1u << slot));

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;

    cmdheader->cfl = sizeof(FIS_REG_H2D) / 4u;
    cmdheader->w = is_write ? 1 : 0;
    cmdheader->c = 0;
    cmdheader->p = 1;
    cmdheader->prdtl = (uint16_t)sg->count;

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, 4096);

    for (uint32_t i = 0u; i < sg->count; i++) {
        const dma_sg_elem_t* elem = &sg->elems[i];

        cmdtbl->prdt_entry[i].dba = (uint32_t)(elem->phys_addr & 0xFFFFFFFFu);
        cmdtbl->prdt_entry[i].dbau = (uint32_t)(elem->phys_addr >> 32);
        cmdtbl->prdt_entry[i].dbc = elem->length - 1u;
        cmdtbl->prdt_entry[i].i = 0;
    }

    cmdtbl->prdt_entry[sg->count - 1u].i = 1;

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    
    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1u << 6;
    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->countl = (uint8_t)count;

    state->slot_sem[slot].count = 0;

    spinlock_release(&state->lock);

    int spin = 0;
    while ((ioread32(hba->iomem, AHCI_PORT_TFD(port_no)) & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) != 0u
           && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }

    if (spin >= 1000000) {
        __sync_fetch_and_and(&ex->hba->port_active_slots[port_no], ~(1u << slot));

        const int reset_ok = ahci_port_comreset(ex);

        if (!reset_ok) {
            dma_unmap_buffer(sg);

            return 0;
        }

        spinlock_acquire(&state->lock);

        const int new_slot = find_cmdslot(hba, port_no);

        if (new_slot == -1) {
            spinlock_release(&state->lock);
            dma_unmap_buffer(sg);

            return 0;
        }

        __sync_fetch_and_or(&ex->hba->port_active_slots[port_no], (1u << new_slot));

        HBA_CMD_HEADER* new_cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
        new_cmdheader += new_slot;

        new_cmdheader->cfl = sizeof(FIS_REG_H2D) / 4u;
        new_cmdheader->w = is_write ? 1 : 0;
        new_cmdheader->c = 0;
        new_cmdheader->p = 1;
        new_cmdheader->prdtl = cmdheader->prdtl;

        HBA_CMD_TBL* new_cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[new_slot]);
        memcpy(new_cmdtbl, cmdtbl, 4096);

        FIS_REG_H2D* new_cmdfis = (FIS_REG_H2D*)(&new_cmdtbl->cfis);
        new_cmdfis->fis_type = FIS_TYPE_REG_H2D;
        new_cmdfis->c = 1;
        new_cmdfis->command = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;

        new_cmdfis->lba0 = (uint8_t)lba;
        new_cmdfis->lba1 = (uint8_t)(lba >> 8);
        new_cmdfis->lba2 = (uint8_t)(lba >> 16);
        new_cmdfis->device = 1u << 6;
        new_cmdfis->lba3 = (uint8_t)(lba >> 24);
        new_cmdfis->countl = (uint8_t)count;

        state->slot_sem[new_slot].count = 0;

        spinlock_release(&state->lock);

        iowrite32(hba->iomem, AHCI_PORT_CI(port_no), 1u << new_slot);

        if (g_ahci_async_mode && can_wait_irq) {
            sem_wait(&state->slot_sem[new_slot]);
        } else {
            while ((ioread32(hba->iomem, AHCI_PORT_CI(port_no)) & (1u << new_slot)) != 0u) {
                __asm__ volatile("pause");
            }
        }

        __sync_fetch_and_and(&ex->hba->port_active_slots[port_no], ~(1u << new_slot));

        if ((ioread32(hba->iomem, AHCI_PORT_IS(port_no)) & (1u << 30)) == 0u
            && (ioread32(hba->iomem, AHCI_PORT_TFD(port_no)) & AHCI_DEV_ERR) == 0u) {
            result = 1;
        }

        dma_unmap_buffer(sg);

        return result;
    }

    iowrite32(hba->iomem, AHCI_PORT_CI(port_no), 1u << slot);

    if (g_ahci_async_mode && can_wait_irq) {
        sem_wait(&state->slot_sem[slot]);
    } else {
        while ((ioread32(hba->iomem, AHCI_PORT_CI(port_no)) & (1u << slot)) != 0u) {
            __asm__ volatile("pause");
        }
    }

    __sync_fetch_and_and(&ex->hba->port_active_slots[port_no], ~(1u << slot));

    result = 1;

    if ((ioread32(hba->iomem, AHCI_PORT_IS(port_no)) & (1u << 30)) != 0u
        || (ioread32(hba->iomem, AHCI_PORT_TFD(port_no)) & AHCI_DEV_ERR) != 0u) {
        
        result = 0;
        ahci_port_comreset(ex);
    }

    dma_unmap_buffer(sg);

    return result;
}

static int ahci_port_comreset(ahci_port_extended_t* ex) {
    if (!ex || !ex->hba) {
        return 0;
    }

    ahci_hba_t* hba = ex->hba;
    const int port_no = ex->port_no;

    if (!hba->iomem) {
        return 0;
    }

    stop_cmd(hba, port_no);

    iowrite32(hba->iomem, AHCI_PORT_SERR(port_no), 0xFFFFFFFFu);
    iowrite32(hba->iomem, AHCI_PORT_IS(port_no), 0xFFFFFFFFu);
    iowrite32(hba->iomem, AHCI_PORT_SCTL(port_no), 0u);

    int wait = 0;
    while ((ioread32(hba->iomem, AHCI_PORT_SSTS(port_no)) & 0xFu) != 0u && wait < 100000) {
        wait++;
        __asm__ volatile("pause");
    }

    iowrite32(hba->iomem, AHCI_PORT_SCTL(port_no), 1u);

    wait = 0;
    while (wait < 100000) {
        wait++;
        __asm__ volatile("pause");
    }

    iowrite32(hba->iomem, AHCI_PORT_SCTL(port_no), 0u);

    wait = 0;
    while ((ioread32(hba->iomem, AHCI_PORT_SSTS(port_no)) & 0xFu) != 3u && wait < 500000) {
        wait++;
        __asm__ volatile("pause");
    }

    if ((ioread32(hba->iomem, AHCI_PORT_SSTS(port_no)) & 0xFu) != 3u) {
        start_cmd(hba, port_no);
        return 0;
    }

    iowrite32(hba->iomem, AHCI_PORT_SERR(port_no), 0xFFFFFFFFu);
    iowrite32(hba->iomem, AHCI_PORT_IS(port_no), 0xFFFFFFFFu);

    start_cmd(hba, port_no);

    return 1;
}

static void ahci_hba_destroy(ahci_hba_t* hba) {
    if (!hba) {
        return;
    }

    if (hba->msi_enabled && hba->msi_vector > 0) {
        irq_free_vector(hba->msi_vector);
    }

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_extended_t* ex = hba->ports[i];
        if (!ex) {
            continue;
        }

        if (ex->bdev) {
            ahci_unregister_and_destroy_bdev(ex);
        }

        ahci_port_destroy(ex);
    }

    if (hba->iomem) {
        iomem_free(hba->iomem);
        hba->iomem = 0;
    }

    ahci_hba_list_remove(hba);

    if (hba->wq) {
        destroy_workqueue(hba->wq);

        hba->wq = 0;
    }

    kfree(hba);
}

int ahci_read_sectors(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (!buf || count == 0u) {
        return 0;
    }

    ahci_port_extended_t* ex = ahci_first_port();
    if (!ex) {
        return 0;
    }

    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;

    if (total_lba > ex->sector_count) {
        if ((uint64_t)lba >= ex->sector_count) {
            return 0;
        }

        count = (uint32_t)(ex->sector_count - (uint64_t)lba);
    }

    return ahci_rw_sectors(ex, (uint64_t)lba, count, buf, 0);
}

int ahci_write_sectors(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (!buf || count == 0u) {
        return 0;
    }

    ahci_port_extended_t* ex = ahci_first_port();
    if (!ex) {
        return 0;
    }

    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;

    if (total_lba > ex->sector_count) {
        if ((uint64_t)lba >= ex->sector_count) {
            return 0;
        }

        count = (uint32_t)(ex->sector_count - (uint64_t)lba);
    }

    return ahci_rw_sectors(ex, (uint64_t)lba, count, (uint8_t*)buf, 1);
}

uint32_t ahci_get_capacity(void) {
    if (!g_ahci_first_bdev || !g_ahci_first_bdev->private_data) {
        return 0;
    }

    ahci_port_extended_t* ex = (ahci_port_extended_t*)g_ahci_first_bdev->private_data;
    return (uint32_t)ex->sector_count;
}

static int check_type(ahci_hba_t* hba, int port_no) {
    const uint32_t ssts = ahci_hba_port_read(hba, port_no, AHCI_PORT_SSTS(port_no));

    const uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0Fu);
    const uint8_t det = (uint8_t)(ssts & 0x0Fu);

    if (det != 3u || ipm != 1u) return 0;

    const uint32_t sig = ahci_hba_port_read(hba, port_no, AHCI_PORT_SIG(port_no));

    if (sig == 0xEB140101u) {
        return 2;
    }

    if (sig == 0xC33C0101u || sig == 0x96690101u) {
        return 0;
    }

    return 1;
}

static void ahci_reset_controller(ahci_hba_t* hba) {
    uint32_t ghc = ahci_read(hba, AHCI_HBA_GHC);
    ghc |= HBA_GHC_AE;
    ghc |= 1u;
    ahci_write(hba, AHCI_HBA_GHC, ghc);

    while ((ahci_read(hba, AHCI_HBA_GHC) & 1u) != 0u) {
        __asm__ volatile("pause" ::: "memory");
    }

    ghc = ahci_read(hba, AHCI_HBA_GHC);
    ghc |= HBA_GHC_AE;
    ghc |= HBA_GHC_IE;
    ahci_write(hba, AHCI_HBA_GHC, ghc);
}


static void ahci_remove(pci_device_t* pdev) {
    if (!pdev) {
        return;
    }

    ahci_hba_t* hba = ahci_hba_from_pdev(pdev);
    if (!hba) {
        return;
    }

    ahci_hba_destroy(hba);
}

static int ahci_probe(pci_device_t* pdev) {
    if (!pdev) {
        return -1;
    }

    ahci_hba_t* hba = (ahci_hba_t*)kzalloc(sizeof(*hba));
    if (!hba) {
        return -1;
    }

    hba->wq = create_workqueue("ahciwrk"); /* ahci worker */

    if (!hba->wq) {
        kfree(hba);

        return -1;
    }

    init_work(&hba->irq_work, ahci_irq_bh);

    hba->pdev = pdev;

    pci_dev_enable_busmaster(pdev);

    const uint8_t irq_line = pdev->irq_line;
    hba->irq_line = irq_line;

    int msi_ok = 0;

    int msi_vec = irq_alloc_vector();

    if (msi_vec < 0) {
        ahci_hba_destroy(hba);

        return -1;
    }

    if (cpu_count > 0 && cpus[0].id >= 0) {
        msi_ok = pci_dev_enable_msi(pdev, msi_vec, (uint8_t)cpus[0].id);
    }

    hba->msi_vector = (uint8_t)msi_vec;

    if (msi_ok) {
        irq_install_vector_handler(msi_vec, ahci_irq_handler, hba);
        hba->msi_enabled = 1;
    } else {
        irq_free_vector(hba->msi_vector);
        
        hba->msi_vector = 0;

        if (!pci_request_irq(pdev, ahci_irq_handler, hba)) {
            ahci_hba_destroy(hba);

            return -1;
        }
    }

    if (pdev->bars[5].size == 0u) {
        pdev->bars[5].size = 4096u; 
    }

    hba->iomem = pci_request_bar(pdev, 5u, "ahci_abar");

    if (!hba->iomem) {
        kfree(hba);
        return -1;
    }

    ahci_reset_controller(hba);

    const uint32_t pi = ahci_read(hba, AHCI_HBA_PI);

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((pi & (1u << i)) == 0u) {
            continue;
        }

        if (check_type(hba, i) != 1) {
            continue;
        }

        ahci_port_extended_t* ex = ahci_port_create(hba, i);
        if (!ex) {
            continue;
        }

        ex->sector_count = (uint64_t)ahci_identify_device(ex);

        if (ex->sector_count != 0u) {
            (void)ahci_create_and_register_bdev(ex);
        } else {
            ahci_port_destroy(ex);
        }
    }

    ahci_hba_list_add(hba);

    return 0;
}

static const pci_device_id_t g_ahci_pci_ids[] = {
    {
        .match_flags = PCI_MATCH_CLASS | PCI_MATCH_SUBCLASS | PCI_MATCH_PROG_IF,
        .vendor_id = 0u,
        .device_id = 0u,
        .class_code = 0x01u, /* Mass Storage */
        .subclass = 0x06u, /* SATA */
        .prog_if = 0x01u /* AHCI 1.0 */
    },
    { .match_flags = 0u }
};

static pci_driver_t g_ahci_pci_driver = {
    .base = {
        .name = "ahci",
        .klass = DRIVER_CLASS_BLOCK,
        .stage = DRIVER_STAGE_CORE,
        .init = 0,
        .shutdown = 0,
    },
    .id_table = g_ahci_pci_ids,
    .probe = ahci_probe,
    .remove = ahci_remove,
};

static int ahci_driver_init(void) {
    dlist_init(&g_ahci_hba_list);
    spinlock_init(&g_ahci_hba_list_lock);

    return pci_register_driver(&g_ahci_pci_driver);
}

DRIVER_REGISTER(
    .name = "ahci",
    .klass = DRIVER_CLASS_BLOCK,
    .stage = DRIVER_STAGE_CORE,
    .init = ahci_driver_init,
    .shutdown = 0
);