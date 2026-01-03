// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/paging.h>
#include <drivers/vga.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <drivers/acpi.h>
#include <hal/irq.h>
#include <hal/io.h>
#include <hal/ioapic.h>

#include "ahci.h"
#include "pci.h"

#define HBA_PxCMD_ST    0x0001
#define HBA_PxCMD_FRE   0x0010
#define HBA_PxCMD_FR    0x4000
#define HBA_PxCMD_CR    0x8000
#define HBA_GHC_AE      (1 << 31)
#define HBA_GHC_IE      (1 << 1)
#define AHCI_DEV_BUSY   (1 << 7)
#define AHCI_DEV_DRQ    (1 << 3)
#define AHCI_DEV_ERR    (1 << 0)
#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EX    0x35
#define ATA_CMD_IDENTIFY        0xEC

static volatile uint32_t primary_disk_sectors = 0;
static volatile HBA_MEM* ahci_base_virt = 0;

#define AHCI_DMA_BUF_SIZE 4096

typedef struct {
    ahci_port_state_t base; 
    void*     dma_buf_virt[32];
    uint32_t  dma_buf_phys[32];
} ahci_port_extended_t;

static ahci_port_extended_t ports_ex[32]; 

static int primary_port_idx = -1;
static int g_ahci_async_mode = 0;

#define AHCI_MSI_VECTOR 0xA1

static uint8_t g_ahci_pci_bus = 0;
static uint8_t g_ahci_pci_slot = 0;
static uint8_t g_ahci_pci_func = 0;
static uint8_t g_ahci_legacy_irq_line = 0;
static int g_ahci_has_device = 0;
static int g_ahci_msi_enabled = 0;

static volatile uint32_t port_active_slots[32] = {0};

void ahci_set_async_mode(int enable) { g_ahci_async_mode = enable; }

int ahci_msi_configure_cpu(int cpu_index) {
    if (!g_ahci_has_device) return 0;
    if (cpu_index < 0 || cpu_index >= cpu_count) return 0;
    if (cpus[cpu_index].id < 0) return 0;

    int ok = pci_msi_configure(g_ahci_pci_bus, g_ahci_pci_slot, g_ahci_pci_func,
                               AHCI_MSI_VECTOR, (uint8_t)cpus[cpu_index].id);
    if (ok) g_ahci_msi_enabled = 1;
    return ok;
}

static int find_cmdslot(volatile HBA_PORT *port, int port_no) {
    uint32_t slots = (port->sact | port->ci | port_active_slots[port_no]);

    if (slots == 0xFFFFFFFF) return -1;

    uint32_t free_mask = ~slots;
    uint32_t first_free_bit;

    __asm__ volatile("bsf %1, %0" : "=r"(first_free_bit) : "r"(free_mask));

    return (int)first_free_bit;
}

static void stop_cmd(volatile HBA_PORT *port) {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    int timeout = 1000000;

    while (timeout--) {
        if (port->cmd & HBA_PxCMD_FR) continue;
        if (port->cmd & HBA_PxCMD_CR) continue;
        break;
    }
}

static void start_cmd(volatile HBA_PORT *port) {
    while (port->cmd & HBA_PxCMD_CR);

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

void ahci_irq_handler(registers_t* regs) {
    (void)regs;

    if (!ahci_base_virt) return;

    volatile HBA_MEM* mmio = ahci_base_virt;
    uint32_t is_glob = mmio->is;

    if (is_glob == 0) return;

    for (int i = 0; i < 32; i++) {
        if (is_glob & (1 << i)) {
            volatile HBA_PORT* port = &mmio->ports[i];

            ahci_port_state_t* state = (ahci_port_state_t*)&ports_ex[i];
            port->is = port->is;

            if (state->active && g_ahci_async_mode) {
                uint32_t active = port_active_slots[i];
                uint32_t finished = active & ~port->ci;

                if (finished) { 
                    for (int s = 0; s < 32; s++) {
                        if (finished & (1 << s)) sem_signal(&state->slot_sem[s]);
                    }
                }
            }
            if (port->serr) port->serr = 0xFFFFFFFF;
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

    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;
    port->ie = 0x7800002F; 

    start_cmd(port);

    state->port_mmio = (HBA_PORT*)port;
    state->active = 1;
}

static int ahci_identify_device(int port_no) {
    ahci_port_extended_t* ex = &ports_ex[port_no];
    ahci_port_state_t* state = (ahci_port_state_t*)ex;
    volatile HBA_PORT* port = state->port_mmio;

    port->is = 0xFFFFFFFF;

    int slot = find_cmdslot(port, port_no);
    if(slot == -1) return 0;
    
    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);

    cmdheader += slot;
    cmdheader->cfl = sizeof(FIS_REG_H2D)/4;
    cmdheader->w   = 0; 
    cmdheader->prdtl = 1; 
    cmdheader->c   = 0; 

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    cmdtbl->prdt_entry[0].dba = ex->dma_buf_phys[slot];
    cmdtbl->prdt_entry[0].dbc = 511; 
    cmdtbl->prdt_entry[0].i   = 1; 

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c        = 1; 
    cmdfis->command  = ATA_CMD_IDENTIFY; 
    cmdfis->device   = 0; 

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }
    port->ci = 1 << slot;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) return 0;
    }
    
    uint16_t* identify_buf = (uint16_t*)ex->dma_buf_virt[slot];
    uint32_t sectors = (uint32_t)identify_buf[60] | ((uint32_t)identify_buf[61] << 16);

    return sectors;
}

int ahci_send_command(int port_no, uint32_t lba, uint8_t* buf, int is_write, uint32_t count) {
    if (port_no < 0 || port_no >= 32) return 0;
    if (!buf && is_write) return 0;
    if (count == 0 || count > 8) return 0;
    
    if (count > 8) count = 8;
    
    uint32_t byte_count = count * 512;
    
    if (byte_count > AHCI_DMA_BUF_SIZE) {
        byte_count = AHCI_DMA_BUF_SIZE;
        count = byte_count / 512;
    }

    ahci_port_extended_t* ex = &ports_ex[port_no];
    ahci_port_state_t* state = (ahci_port_state_t*)ex;
    
    if (!state->active || !ex->dma_buf_virt[0]) return 0;
    
    spinlock_acquire(&state->lock);
    volatile HBA_PORT* port = (volatile HBA_PORT*)state->port_mmio;

    if (port->is & (1 << 30)) {
        port->is = 0xFFFFFFFF;
        port->serr = 0xFFFFFFFF;
    }

    int slot = find_cmdslot(port, port_no);
    if (slot == -1 || slot < 0 || slot >= 32) {
        spinlock_release(&state->lock);
        return 0;
    }

    uint8_t* dma_virt = (uint8_t*)ex->dma_buf_virt[slot];
    
    if (!dma_virt) {
        spinlock_release(&state->lock);
        return 0;
    }
    
    if (is_write) {
        if (byte_count <= AHCI_DMA_BUF_SIZE) {
            memcpy(dma_virt, buf, byte_count);
        } else {
            spinlock_release(&state->lock);
            return 0;
        }
    } else {
        memset(dma_virt, 0, byte_count);
    }

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;
    cmdheader->cfl = sizeof(FIS_REG_H2D)/4;
    cmdheader->w   = is_write ? 1 : 0;
    cmdheader->c   = 0;
    cmdheader->p   = 1; 
    cmdheader->prdtl = 1;

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, 128);

    cmdtbl->prdt_entry[0].dba = ex->dma_buf_phys[slot];
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = byte_count - 1; 
    cmdtbl->prdt_entry[0].i   = 1;

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c        = 1; 
    cmdfis->command  = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX; 
    cmdfis->lba0     = (uint8_t)lba;
    cmdfis->lba1     = (uint8_t)(lba >> 8);
    cmdfis->lba2     = (uint8_t)(lba >> 16);
    cmdfis->device   = 1 << 6; 
    cmdfis->lba3     = (uint8_t)(lba >> 24);
    cmdfis->countl   = (uint8_t)count; 

    state->slot_sem[slot].count = 0;

    spinlock_release(&state->lock);

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 1000000) {
        spin++;
        __asm__ volatile("pause");
    }

    __sync_fetch_and_or(&port_active_slots[port_no], (1 << slot));
    port->ci = 1 << slot;

    int success = 1;

    if (g_ahci_async_mode) {
        sem_wait(&state->slot_sem[slot]);
    } else {
        while (port->ci & (1 << slot)) {
             __asm__ volatile("pause");
        }
    }

    __sync_fetch_and_and(&port_active_slots[port_no], ~(1 << slot));

    if ((port->is & (1 << 30)) || (port->tfd & AHCI_DEV_ERR)) {
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

int ahci_read_sectors(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (primary_port_idx == -1 || !buf || count == 0) return 0;
    
    if (count > 8) count = 8;
    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;
    if (total_lba > primary_disk_sectors) {
        if (lba >= primary_disk_sectors) return 0;
        count = primary_disk_sectors - lba;
        if (count > 8) count = 8;
    }
    
    return ahci_send_command(primary_port_idx, lba, buf, 0, count);
}

int ahci_write_sectors(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (primary_port_idx == -1 || !buf || count == 0) return 0;
    
    if (count > 8) count = 8;
    uint64_t total_lba = (uint64_t)lba + (uint64_t)count;
    if (total_lba > primary_disk_sectors) {
        if (lba >= primary_disk_sectors) return 0;
        count = primary_disk_sectors - lba;
        if (count > 8) count = 8;
    }
    
    return ahci_send_command(primary_port_idx, lba, (uint8_t*)buf, 1, count);
}

uint32_t ahci_get_capacity(void) { 
    return primary_disk_sectors; 
}

static int check_type(volatile HBA_PORT *port) {
    uint32_t ssts = port->ssts;

    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3 || ipm != 1) return 0;
    if (port->sig == 0xEB140101) return 2; 
    if (port->sig == 0xC33C0101 || port->sig == 0x96690101) return 0; 
    return 1; 
}

static void ahci_reset_controller(volatile HBA_MEM* abar) {
    abar->ghc |= HBA_GHC_AE;
    abar->ghc |= 1; 

    while (abar->ghc & 1);

    abar->ghc |= HBA_GHC_AE;
    abar->ghc |= HBA_GHC_IE; 
}

void ahci_init(void) {
    uint8_t bus, slot, func;
    if (!pci_find_ahci_device(&bus, &slot, &func)) return;

    g_ahci_pci_bus = bus;
    g_ahci_pci_slot = slot;
    g_ahci_pci_func = func;
    g_ahci_has_device = 1;

    uint32_t pci_cmd = pci_read(bus, slot, func, 0x04);
    if (!(pci_cmd & 0x04) || (pci_cmd & 0x400)) {
        pci_cmd |= 0x04;
        pci_cmd &= ~0x400; 
        pci_write(bus, slot, func, 0x04, pci_cmd);
    }

    uint32_t pci_irq_info = pci_read(bus, slot, func, 0x3C);
    uint8_t irq_line = pci_irq_info & 0xFF;

    g_ahci_legacy_irq_line = irq_line;

    int msi_ok = 0;
    if (cpu_count > 0 && cpus[0].id >= 0) {
        msi_ok = pci_msi_configure(bus, slot, func, AHCI_MSI_VECTOR, (uint8_t)cpus[0].id);
    }

    if (msi_ok) {
        irq_install_vector_handler(AHCI_MSI_VECTOR, ahci_irq_handler);
        g_ahci_msi_enabled = 1;
    } else {
        irq_install_handler(irq_line, ahci_irq_handler);

        if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
            uint32_t gsi;
            int active_low;
            int level_trigger;
            if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
                gsi = (uint32_t)irq_line;
                active_low = 0;
                level_trigger = 0;
            }
            ioapic_route_gsi(gsi, (uint8_t)(32 + irq_line), (uint8_t)cpus[0].id, active_low, level_trigger);
        } else {
            if (irq_line < 8) outb(0x21, inb(0x21) & ~(1 << irq_line));
            else {
                outb(0xA1, inb(0xA1) & ~(1 << (irq_line - 8)));
                outb(0x21, inb(0x21) & ~(1 << 2));
            }
        }
    }

    uint32_t bar5 = pci_get_bar5(bus, slot, func);
    
    paging_map(kernel_page_directory, bar5, bar5, 0x13);
    paging_map(kernel_page_directory, bar5 + 4096, bar5 + 4096, 0x13);
    
    ahci_base_virt = (volatile HBA_MEM*)bar5;
    ahci_reset_controller(ahci_base_virt);

    uint32_t pi = ahci_base_virt->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            if (check_type(&ahci_base_virt->ports[i]) == 1) {
                port_init(i);
                if (primary_port_idx == -1) {
                    primary_port_idx = i;
                    primary_disk_sectors = ahci_identify_device(i);
                }
            }
        }
    }
}