/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/block/bdev.h>
#include <drivers/pci/pci.h>
#include <drivers/driver.h>
#include <drivers/acpi.h>
#include <drivers/vga.h>

#include <hal/ioapic.h>
#include <hal/mmio.h>
#include <hal/irq.h>
#include <hal/io.h>

#include <kernel/smp/cpu.h>
#include <kernel/sched.h>

#include <arch/i386/paging.h>

#include <lib/string.h>

#include <mm/heap.h>

#include "ahci.h"

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

#define AHCI_DMA_BUF_SIZE 4096
#define AHCI_MSI_VECTOR 0xA1

typedef struct {
    ahci_port_state_t base;

    void* dma_buf_virt[32];
    uint32_t dma_buf_phys[32];

    int port_no;
    uint64_t sector_count;

    block_device_t* bdev;
    char* bdev_name;
} ahci_port_extended_t;

static volatile HBA_MEM* ahci_base_virt = 0;

static mmio_region_t* ahci_mmio_region = 0;

static ahci_port_extended_t ports_ex[32];

static int g_ahci_async_mode = 0;

static pci_device_t* g_ahci_pdev = 0;

static uint8_t g_ahci_legacy_irq_line = 0;
static int g_ahci_msi_enabled = 0;

static volatile uint32_t port_active_slots[32] = { 0 };

static uint32_t g_ahci_disk_seq = 0;
static block_device_t* g_ahci_first_bdev = 0;

static void stop_cmd(volatile HBA_PORT* port);
static void start_cmd(volatile HBA_PORT* port);

static ahci_port_extended_t* ahci_first_port(void) {
    if (!g_ahci_first_bdev || !g_ahci_first_bdev->private_data) {
        return 0;
    }

    return (ahci_port_extended_t*)g_ahci_first_bdev->private_data;
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
    ahci_port_extended_t* ex,
    uint32_t lba,
    uint8_t* buf,
    int is_write,
    uint32_t count
);

static uint32_t ahci_identify_device(ahci_port_extended_t* ex);
static int ahci_create_and_register_bdev(ahci_port_extended_t* ex);
static void ahci_unregister_and_destroy_bdev(ahci_port_extended_t* ex);
static void ahci_port_destroy(ahci_port_extended_t* ex);
static void ahci_remove_all_devices(void);

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

    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    if (state->active && state->port_mmio) {
        stop_cmd((volatile HBA_PORT*)state->port_mmio);
    }

    for (int i = 0; i < 32; i++) {
        if (state->ctba_virt[i]) {
            kfree(state->ctba_virt[i]);
            state->ctba_virt[i] = 0;
        }

        if (ex->dma_buf_virt[i]) {
            kfree(ex->dma_buf_virt[i]);
            ex->dma_buf_virt[i] = 0;
        }

        ex->dma_buf_phys[i] = 0;
    }

    if (state->fb_virt) {
        kfree(state->fb_virt);
        state->fb_virt = 0;
    }

    if (state->clb_virt) {
        kfree(state->clb_virt);
        state->clb_virt = 0;
    }

    state->port_mmio = 0;
    state->active = 0;

    ex->port_no = -1;
    ex->sector_count = 0;
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
    return ahci_send_command(ex, (uint32_t)lba, (uint8_t*)buf, 0, count) != 0;
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
    return ahci_send_command(ex, (uint32_t)lba, (uint8_t*)buf, 1, count) != 0;
}

static int ahci_bdev_flush(block_device_t* dev) {
    (void)dev;
    return 1;
}

void ahci_set_async_mode(int enable) {
    g_ahci_async_mode = enable;
}

int ahci_msi_configure_cpu(int cpu_index) {
    if (!g_ahci_pdev) {
        return 0;
    }

    if (cpu_index < 0 || cpu_index >= cpu_count) {
        return 0;
    }

    if (cpus[cpu_index].id < 0) {
        return 0;
    }

    const int ok = pci_dev_enable_msi(
        g_ahci_pdev, AHCI_MSI_VECTOR,
        (uint8_t)cpus[cpu_index].id
    );

    if (ok) {
        g_ahci_msi_enabled = 1;
    }

    return ok;
}

static int find_cmdslot(volatile HBA_PORT* port, int port_no) {
    uint32_t slots = (port->sact | port->ci | port_active_slots[port_no]);

    if (slots == 0xFFFFFFFFu) {
        return -1;
    }

    uint32_t free_mask = ~slots;
    uint32_t first_free_bit;

    __asm__ volatile("bsf %1, %0" : "=r"(first_free_bit) : "r"(free_mask));

    return (int)first_free_bit;
}

static void stop_cmd(volatile HBA_PORT* port) {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    int timeout = 1000000;

    while (timeout--) {
        if ((port->cmd & HBA_PxCMD_FR) != 0u) {
            continue;
        }

        if ((port->cmd & HBA_PxCMD_CR) != 0u) {
            continue;
        }

        break;
    }
}

static void start_cmd(volatile HBA_PORT* port) {
    while ((port->cmd & HBA_PxCMD_CR) != 0u) {
        /* Wait for port to become free */
    }

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

void ahci_irq_handler(registers_t* regs) {
    (void)regs;

    if (!ahci_base_virt) {
        return;
    }

    volatile HBA_MEM* mmio = ahci_base_virt;
    uint32_t is_glob = mmio->is;

    if (is_glob == 0u) {
        return;
    }

    for (int i = 0; i < 32; i++) {
        if ((is_glob & (1u << i)) != 0u) {
            volatile HBA_PORT* port = &mmio->ports[i];
            ahci_port_state_t* state = (ahci_port_state_t*)&ports_ex[i];

            port->is = port->is;

            if (state->active && g_ahci_async_mode) {
                uint32_t active = port_active_slots[i];
                uint32_t finished = active & ~port->ci;

                if (finished != 0u) {
                    for (int s = 0; s < 32; s++) {
                        if ((finished & (1u << s)) != 0u) {
                            sem_signal(&state->slot_sem[s]);
                        }
                    }
                }
            }

            if (port->serr) {
                port->serr = 0xFFFFFFFFu;
            }
        }
    }
}

static void port_init(int port_no) {
    ahci_port_extended_t* ex = &ports_ex[port_no];
    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    volatile HBA_PORT* port = &ahci_base_virt->ports[port_no];

    stop_cmd(port);

    state->clb_virt = kmalloc_a(1024);
    memset(state->clb_virt, 0, 1024);

    port->clb = paging_get_phys(kernel_page_directory, (uint32_t)state->clb_virt);
    port->clbu = 0;

    state->fb_virt = kmalloc_a(256);
    memset(state->fb_virt, 0, 256);

    port->fb = paging_get_phys(kernel_page_directory, (uint32_t)state->fb_virt);
    port->fbu = 0;

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)state->clb_virt;

    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;

        void* ctba_virt = kmalloc_a(256);
        memset(ctba_virt, 0, 256);

        state->ctba_virt[i] = ctba_virt;

        cmdheader[i].ctba = paging_get_phys(kernel_page_directory, (uint32_t)ctba_virt);
        cmdheader[i].ctbau = 0;

        sem_init(&state->slot_sem[i], 0);

        ex->dma_buf_virt[i] = kmalloc_a(AHCI_DMA_BUF_SIZE);
        memset(ex->dma_buf_virt[i], 0, AHCI_DMA_BUF_SIZE);

        ex->dma_buf_phys[i] = paging_get_phys(kernel_page_directory, (uint32_t)ex->dma_buf_virt[i]);
    }

    spinlock_init(&state->lock);
    port_active_slots[port_no] = 0;

    port->serr = 0xFFFFFFFFu;
    port->is = 0xFFFFFFFFu;
    port->ie = 0x7800002Fu;

    start_cmd(port);

    state->port_mmio = (HBA_PORT*)port;
    state->active = 1;

    ex->port_no = port_no;
}

static uint32_t ahci_identify_device(ahci_port_extended_t* ex) {
    if (!ex) {
        return 0;
    }

    ahci_port_state_t* state = (ahci_port_state_t*)ex;
    volatile HBA_PORT* port = state->port_mmio;

    port->is = 0xFFFFFFFFu;

    int port_no = ex->port_no;
    int slot = find_cmdslot(port, port_no);
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
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    cmdtbl->prdt_entry[0].dba = ex->dma_buf_phys[slot];
    cmdtbl->prdt_entry[0].dbc = 511;
    cmdtbl->prdt_entry[0].i = 1;

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_IDENTIFY;
    cmdfis->device = 0;

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) != 0u
        && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }

    port->ci = 1u << slot;

    while (1) {
        if ((port->ci & (1u << slot)) == 0u) {
            break;
        }

        if ((port->is & (1u << 30)) != 0u) {
            return 0;
        }
    }

    uint16_t* identify_buf = (uint16_t*)ex->dma_buf_virt[slot];
    uint32_t sectors = (uint32_t)identify_buf[60] | ((uint32_t)identify_buf[61] << 16);

    return sectors;
}

static int ahci_send_command(
    ahci_port_extended_t* ex,
    uint32_t lba,
    uint8_t* buf,
    int is_write,
    uint32_t count
) {
    if (!ex) {
        return 0;
    }

    int port_no = ex->port_no;
    if (port_no < 0 || port_no >= 32) {
        return 0;
    }

    if (!buf && is_write) {
        return 0;
    }

    if (count == 0 || count > 8u) {
        return 0;
    }

    uint32_t eflags;
    __asm__ volatile("pushfl; popl %0" : "=r"(eflags) : : "memory");
    int can_wait_irq = ((eflags & 0x200u) != 0u);

    uint32_t byte_count = count * 512u;

    if (byte_count > AHCI_DMA_BUF_SIZE) {
        byte_count = AHCI_DMA_BUF_SIZE;
        count = byte_count / 512u;
    }

    ahci_port_state_t* state = (ahci_port_state_t*)ex;

    if (!state->active || !ex->dma_buf_virt[0]) {
        return 0;
    }

    spinlock_acquire(&state->lock);

    volatile HBA_PORT* port = (volatile HBA_PORT*)state->port_mmio;

    if ((port->is & (1u << 30)) != 0u) {
        port->is = 0xFFFFFFFFu;
        port->serr = 0xFFFFFFFFu;
    }

    int slot = find_cmdslot(port, port_no);
    if (slot == -1 || slot < 0 || slot >= 32) {
        spinlock_release(&state->lock);
        return 0;
    }

    __sync_fetch_and_or(&port_active_slots[port_no], (1u << slot));

    uint8_t* dma_virt = (uint8_t*)ex->dma_buf_virt[slot];

    if (!dma_virt) {
        __sync_fetch_and_and(&port_active_slots[port_no], ~(1u << slot));
        spinlock_release(&state->lock);
        return 0;
    }

    if (is_write) {
        if (byte_count <= AHCI_DMA_BUF_SIZE) {
            memcpy(dma_virt, buf, byte_count);
        } else {
            __sync_fetch_and_and(&port_active_slots[port_no], ~(1u << slot));
            spinlock_release(&state->lock);
            return 0;
        }
    } else {
        memset(dma_virt, 0, byte_count);
    }

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;

    cmdheader->cfl = sizeof(FIS_REG_H2D) / 4u;
    cmdheader->w = is_write ? 1 : 0;
    cmdheader->c = 0;
    cmdheader->p = 1;
    cmdheader->prdtl = 1;

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, 128);

    cmdtbl->prdt_entry[0].dba = ex->dma_buf_phys[slot];
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = byte_count - 1u;
    cmdtbl->prdt_entry[0].i = 1;

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
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) != 0u
        && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }

    if (spin >= 1000000) {
        __sync_fetch_and_and(&port_active_slots[port_no], ~(1u << slot));
        return 0;
    }

    port->ci = 1u << slot;

    int success = 1;

    if (g_ahci_async_mode && can_wait_irq) {
        sem_wait(&state->slot_sem[slot]);
    } else {
        while ((port->ci & (1u << slot)) != 0u) {
            __asm__ volatile("pause");
        }
    }

    __sync_fetch_and_and(&port_active_slots[port_no], ~(1u << slot));

    if ((port->is & (1u << 30)) != 0u
        || (port->tfd & AHCI_DEV_ERR) != 0u) {
        success = 0;
    }

    if (success && !is_write) {
        if (buf && dma_virt && byte_count <= AHCI_DMA_BUF_SIZE) {
            memcpy(buf, dma_virt, byte_count);
        } else {
            success = 0;
        }
    }

    return success;
}

static void ahci_remove_all_devices(void) {
    for (int i = 0; i < 32; i++) {
        ahci_port_extended_t* ex = &ports_ex[i];

        if (ex->bdev) {
            ahci_unregister_and_destroy_bdev(ex);
        }

        ahci_port_destroy(ex);
    }

    memset(ports_ex, 0, sizeof(ports_ex));
    memset((void*)port_active_slots, 0, sizeof(port_active_slots));

    g_ahci_disk_seq = 0;
    g_ahci_first_bdev = 0;
}

int ahci_read_sectors(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (!buf || count == 0u) {
        return 0;
    }

    ahci_port_extended_t* ex = ahci_first_port();
    if (!ex) {
        return 0;
    }

    if (count > 8u) {
        count = 8u;
    }

    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;

    if (total_lba > ex->sector_count) {
        if ((uint64_t)lba >= ex->sector_count) {
            return 0;
        }

        count = (uint32_t)(ex->sector_count - (uint64_t)lba);

        if (count > 8u) {
            count = 8u;
        }
    }

    return ahci_send_command(ex, lba, buf, 0, count);
}

int ahci_write_sectors(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (!buf || count == 0u) {
        return 0;
    }

    ahci_port_extended_t* ex = ahci_first_port();
    if (!ex) {
        return 0;
    }

    if (count > 8u) {
        count = 8u;
    }

    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;

    if (total_lba > ex->sector_count) {
        if ((uint64_t)lba >= ex->sector_count) {
            return 0;
        }

        count = (uint32_t)(ex->sector_count - (uint64_t)lba);

        if (count > 8u) {
            count = 8u;
        }
    }

    return ahci_send_command(ex, lba, (uint8_t*)buf, 1, count);
}

uint32_t ahci_get_capacity(void) {
    if (!g_ahci_first_bdev || !g_ahci_first_bdev->private_data) {
        return 0;
    }

    ahci_port_extended_t* ex = (ahci_port_extended_t*)g_ahci_first_bdev->private_data;
    return (uint32_t)ex->sector_count;
}

static int check_type(volatile HBA_PORT* port) {
    uint32_t ssts = port->ssts;

    uint8_t ipm = (ssts >> 8) & 0x0Fu;
    uint8_t det = ssts & 0x0Fu;

    if (det != 3 || ipm != 1) {
        return 0;
    }

    if (port->sig == 0xEB140101u) {
        return 2;
    }

    if (port->sig == 0xC33C0101u || port->sig == 0x96690101u) {
        return 0;
    }

    return 1;
}

static void ahci_reset_controller(volatile HBA_MEM* abar) {
    abar->ghc |= HBA_GHC_AE;
    abar->ghc |= 1u;

    while ((abar->ghc & 1u) != 0u) {
        /* Wait for reset to complete */
    }

    abar->ghc |= HBA_GHC_AE;
    abar->ghc |= HBA_GHC_IE;
}

static void ahci_remove(pci_device_t* pdev) {
    (void)pdev;

    ahci_remove_all_devices();

    if (ahci_mmio_region) {
        mmio_release_region(ahci_mmio_region);
        ahci_mmio_region = 0;
    }

    ahci_base_virt = 0;

    g_ahci_pdev = 0;
    g_ahci_legacy_irq_line = 0;
    g_ahci_msi_enabled = 0;
}

static int ahci_probe(pci_device_t* pdev) {
    if (!pdev) {
        return -1;
    }

    if (g_ahci_pdev) {
        /* Currently we support only one active AHCI controller in the system. */
        return -1;
    }

    g_ahci_pdev = pdev;

    pci_dev_enable_busmaster(pdev);

    const uint8_t irq_line = pdev->irq_line;
    g_ahci_legacy_irq_line = irq_line;

    int msi_ok = 0;

    if (cpu_count > 0 && cpus[0].id >= 0) {
        msi_ok = pci_dev_enable_msi(pdev, AHCI_MSI_VECTOR, (uint8_t)cpus[0].id);
    }

    if (msi_ok) {
        irq_install_vector_handler(AHCI_MSI_VECTOR, ahci_irq_handler);
        g_ahci_msi_enabled = 1;
    } else {
        irq_install_handler(irq_line, ahci_irq_handler);

        if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
            uint32_t gsi = 0u;
            int active_low = 0;
            int level_trigger = 0;

            if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
                gsi = (uint32_t)irq_line;
                active_low = 0;
                level_trigger = 0;
            }

            ioapic_route_gsi(
                gsi, (uint8_t)(32u + irq_line),
                (uint8_t)cpus[0].id, active_low,
                level_trigger
            );
        } else {
            if (irq_line < 8u) {
                outb(0x21, (uint8_t)(inb(0x21) & ~(1u << irq_line)));
            } else {
                outb(0xA1, (uint8_t)(inb(0xA1) & ~(1u << (irq_line - 8u))));
                outb(0x21, (uint8_t)(inb(0x21) & ~(1u << 2u)));
            }
        }
    }

    uint32_t bar5_phys = pdev->bars[5].base_addr;
    uint32_t bar5_size = pdev->bars[5].size;

    if (bar5_size == 0u) {
        bar5_size = sizeof(HBA_MEM);
    }

    ahci_mmio_region = mmio_request_region(bar5_phys, bar5_size, "ahci_abar");

    if (!ahci_mmio_region) {
        g_ahci_pdev = 0;
        return -1;
    }

    ahci_base_virt = (volatile HBA_MEM*)mmio_get_vaddr(ahci_mmio_region);

    ahci_reset_controller(ahci_base_virt);

    const uint32_t pi = ahci_base_virt->pi;

    for (int i = 0; i < 32; i++) {
        if ((pi & (1u << i)) != 0u) {
            if (check_type(&ahci_base_virt->ports[i]) == 1) {
                port_init(i);

                ahci_port_extended_t* ex = &ports_ex[i];
                ex->sector_count = (uint64_t)ahci_identify_device(ex);

                if (ex->sector_count != 0u) {
                    (void)ahci_create_and_register_bdev(ex);
                } else {
                    ahci_port_destroy(ex);
                }
            }
        }
    }

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
    return pci_register_driver(&g_ahci_pci_driver);
}

DRIVER_REGISTER(
    .name = "ahci",
    .klass = DRIVER_CLASS_BLOCK,
    .stage = DRIVER_STAGE_CORE,
    .init = ahci_driver_init,
    .shutdown = 0
);