#include <arch/i386/paging.h>
#include <drivers/vga.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <kernel/sched.h>
#include <hal/irq.h>
#include <hal/io.h>

#include "ahci.h"
#include "pci.h"

extern void kernel_panic(const char* message, const char* file, uint32_t line, void* regs);

#define HBA_PxCMD_ST    0x0001
#define HBA_PxCMD_FRE   0x0010
#define HBA_PxCMD_FR    0x4000
#define HBA_PxCMD_CR    0x8000

#define HBA_GHC_AE      (1 << 31)
#define HBA_GHC_IE      (1 << 1)

#define AHCI_DEV_BUSY   (1 << 7)
#define AHCI_DEV_DRQ    (1 << 3)

static volatile uint32_t primary_disk_sectors = 0;
static volatile HBA_MEM* ahci_base_virt = 0;
static ahci_port_state_t ports[32];
static int primary_port_idx = -1;
static int g_ahci_async_mode = 0;

void ahci_set_async_mode(int enable) {
    g_ahci_async_mode = enable;
}

static int find_cmdslot(volatile HBA_PORT *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) return i;
        slots >>= 1;
    }
    return -1;
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
    volatile HBA_MEM* mmio = ahci_base_virt;
    uint32_t is_glob = mmio->is;
    
    if (is_glob == 0) return;

    for (int i = 0; i < 32; i++) {
        if (is_glob & (1 << i)) {
            volatile HBA_PORT* port = &mmio->ports[i];
            
            uint32_t port_is = port->is;
            if (port_is) {
                ports[i].last_is = port_is;
                port->is = port_is; // Write-1-to-Clear
                
                if (ports[i].active && g_ahci_async_mode) {
                    sem_signal(&ports[i].sem_complete);
                }
            }
            if (port->serr) port->serr = 0xFFFFFFFF;
        }
    }
}

static void port_init(int port_no) {
    ahci_port_state_t* state = &ports[port_no];
    volatile HBA_PORT* port = &ahci_base_virt->ports[port_no];

    stop_cmd(port);

    state->clb_virt = kmalloc_a(1024);
    memset(state->clb_virt, 0, 1024);
    uint32_t clb_phys = paging_get_phys(kernel_page_directory, (uint32_t)state->clb_virt);
    port->clb = clb_phys;
    port->clbu = 0; 

    state->fb_virt = kmalloc_a(256);
    memset(state->fb_virt, 0, 256);
    uint32_t fb_phys = paging_get_phys(kernel_page_directory, (uint32_t)state->fb_virt);
    port->fb = fb_phys;
    port->fbu = 0;

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)state->clb_virt;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;
        void* ctba_virt = kmalloc_a(256);
        memset(ctba_virt, 0, 256);
        state->ctba_virt[i] = ctba_virt;
        
        uint32_t ctba_phys = paging_get_phys(kernel_page_directory, (uint32_t)ctba_virt);
        cmdheader[i].ctba = ctba_phys;
        cmdheader[i].ctbau = 0;
    }

    sem_init(&state->sem_complete, 0);
    sem_init(&state->port_mutex, 1); 

    spinlock_init(&state->lock);
    state->last_is = 0;

    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;
    
    port->ie = 0x7800002F; 

    start_cmd(port);
    state->port_mmio = (HBA_PORT*)port;
    state->active = 1;
}

static int ahci_identify_device(int port_no) {
    ahci_port_state_t* state = &ports[port_no];
    volatile HBA_PORT* port = state->port_mmio;

    port->is = 0xFFFFFFFF;
    int slot = find_cmdslot(port);
    if(slot == -1) return 0;
    
    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;
    cmdheader->cfl = sizeof(FIS_REG_H2D)/4;
    cmdheader->w   = 0; 
    cmdheader->prdtl = 1; 
    cmdheader->c   = 0; // Clear Busy must be 0

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    uint16_t* identify_buf = (uint16_t*)kmalloc_a(512);
    memset(identify_buf, 0, 512);
    
    uint32_t phys_buf = paging_get_phys(kernel_page_directory, (uint32_t)identify_buf);
    cmdtbl->prdt_entry[0].dba = phys_buf;
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = 511; 
    cmdtbl->prdt_entry[0].i   = 1; // Interrupt on completion

    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c        = 1; 
    cmdfis->command  = 0xEC; 
    cmdfis->device   = 0; 

    // Polling wait for busy
    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 1000000) {
        spin++; __asm__ volatile("pause");
    }
    
    port->ci = 1 << slot;

    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) { kfree(identify_buf); return 0; }
    }
    
    uint32_t sectors = (uint32_t)identify_buf[60] | ((uint32_t)identify_buf[61] << 16);
    kfree(identify_buf);
    return sectors;
}

int ahci_send_command(int port_no, uint32_t lba, uint8_t* buf, int is_write) {
    ahci_port_state_t* state = &ports[port_no];
    uint32_t flags = spinlock_acquire_safe(&state->lock);
    volatile HBA_PORT* port = (volatile HBA_PORT*)state->port_mmio;

    port->is = 0xFFFFFFFF;
    
    int slot = find_cmdslot(port);
    if (slot == -1) {
        spinlock_release_safe(&state->lock, flags);
        return 0;
    }

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;
    cmdheader->cfl = sizeof(FIS_REG_H2D)/4;
    cmdheader->w   = is_write ? 1 : 0;
    cmdheader->c   = 0;
    cmdheader->p   = 1; 

    uint8_t* dma_target = (uint8_t*)kmalloc(512);
    if (!dma_target) {
        spinlock_release_safe(&state->lock, flags);
        return 0;
    }
    if (is_write) memcpy(dma_target, buf, 512);
    else memset(dma_target, 0, 512);

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    uint32_t phys_addr = paging_get_phys(kernel_page_directory, (uint32_t)dma_target);
    
    cmdtbl->prdt_entry[0].dba = phys_addr;
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = 511; 
    cmdtbl->prdt_entry[0].i   = 1;

    cmdheader->prdtl = 1;
    
    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c        = 1; 
    cmdfis->command  = is_write ? 0x35 : 0x25; 
    cmdfis->lba0     = (uint8_t)lba;
    cmdfis->lba1     = (uint8_t)(lba >> 8);
    cmdfis->lba2     = (uint8_t)(lba >> 16);
    cmdfis->device   = 1 << 6; 
    cmdfis->lba3     = (uint8_t)(lba >> 24);
    cmdfis->countl   = 1; 

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 1000000) {
        spin++;
        if(g_ahci_async_mode) {
            if(spin % 100 == 0) __asm__ volatile("int $0x80" : : "a"(11), "b"(200));
        }
        else {
            __asm__ volatile("pause");
        } 
    }

    if (g_ahci_async_mode) state->sem_complete.count = 0;

    __asm__ volatile("sfence" ::: "memory");

    port->ci = 1 << slot;

    volatile uint32_t flush = port->ci; (void)flush;

    int success = 1;

    if (g_ahci_async_mode) {
        spinlock_release_safe(&state->lock, flags);
        sem_wait(&state->sem_complete);
        
        if (state->last_is & (1<<30) || state->last_is & (1<<29) || state->last_is & (1<<27)) success = 0;
    } else {
        int spin = 0;
        while (1) {
            spin++;
            if ((port->ci & (1 << slot)) == 0) break;
            if (port->is & (1 << 30)) { success = 0; break; }
            if(g_ahci_async_mode) {
                if(spin % 100 == 0) __asm__ volatile("int $0x80" : : "a"(11), "b"(200));
            } else {
                __asm__ volatile("pause");
            }
        }
        port->is = 0xFFFFFFFF;
        spinlock_release_safe(&state->lock, flags);
    }

    if (success && !is_write) memcpy(buf, dma_target, 512);
    kfree(dma_target);

    return success;
}

int ahci_read_sector(uint32_t lba, uint8_t* buf) {
    if (primary_port_idx == -1) return 0;
    
    ahci_port_state_t* state = &ports[primary_port_idx];
    
    sem_wait(&state->port_mutex);
    
    int res = ahci_send_command(primary_port_idx, lba, buf, 0);
    
    sem_signal(&state->port_mutex);
    
    return res;
}

int ahci_write_sector(uint32_t lba, const uint8_t* buf) {
    if (primary_port_idx == -1) return 0;
    
    ahci_port_state_t* state = &ports[primary_port_idx];
    
    sem_wait(&state->port_mutex);
    
    int res = ahci_send_command(primary_port_idx, lba, (uint8_t*)buf, 1);
    
    sem_signal(&state->port_mutex);
    
    return res;
}

uint32_t ahci_get_capacity(void) { return primary_disk_sectors; }

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
    abar->ghc |= 1; // Reset
    while (abar->ghc & 1);
    abar->ghc |= HBA_GHC_AE;
    abar->ghc |= HBA_GHC_IE; // Enable Interrupts Globally
}

void ahci_init(void) {
    uint8_t bus, slot, func;
    if (!pci_find_ahci_device(&bus, &slot, &func)) return;

    uint32_t pci_cmd = pci_read(bus, slot, func, 0x04);
    if (!(pci_cmd & 0x04) || (pci_cmd & 0x400)) {
        pci_cmd |= 0x04;
        pci_cmd &= ~0x400; // Enable INTx
        pci_write(bus, slot, func, 0x04, pci_cmd);
    }

    uint32_t pci_irq_info = pci_read(bus, slot, func, 0x3C);
    uint8_t irq_line = pci_irq_info & 0xFF;
    
    irq_install_handler(irq_line, ahci_irq_handler);
    
    if (irq_line < 8) outb(0x21, inb(0x21) & ~(1 << irq_line));
    else {
        outb(0xA1, inb(0xA1) & ~(1 << (irq_line - 8)));
        outb(0x21, inb(0x21) & ~(1 << 2)); 
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