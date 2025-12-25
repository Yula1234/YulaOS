#include <arch/i386/paging.h>
#include <drivers/vga.h>
#include <lib/string.h>
#include <mm/heap.h>

#include "ahci.h"
#include "pci.h"

extern void kernel_panic(const char* message, const char* file, uint32_t line, void* regs);

#define HBA_PxCMD_ST    0x0001
#define HBA_PxCMD_FRE   0x0010
#define HBA_PxCMD_FR    0x4000
#define HBA_PxCMD_CR    0x8000

#define HBA_GHC_AE      (1 << 31) 
#define HBA_GHC_IE      (1 << 1) 
#define HBA_GHC_HR      (1 << 0)  

#define SATA_SIG_ATA    0x00000101
#define SATA_SIG_ATAPI  0xEB140101
#define SATA_SIG_SEMB   0xC33C0101
#define SATA_SIG_PM     0x96690101

#define AHCI_DEV_BUSY   (1 << 7)
#define AHCI_DEV_DRQ    (1 << 3)

static volatile HBA_MEM* ahci_base_virt = 0;

static ahci_port_state_t ports[32];
static int primary_port_idx = -1;

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
    int timeout = 1000000;
    while ((port->cmd & HBA_PxCMD_CR) && timeout--);
    
    if (timeout <= 0) {
        kernel_panic("AHCI Port Hung (Start CMD)", "ahci.c", 0, 0);
    }

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static void port_init(int port_no) {
    volatile HBA_PORT* port = &ahci_base_virt->ports[port_no];
    ahci_port_state_t* state = &ports[port_no];

    stop_cmd(port);

    // 1. Command List (1KB aligned)
    state->clb_virt = kmalloc_a(1024);
    if (!state->clb_virt) kernel_panic("AHCI OOM (CLB)", "ahci.c", port_no, 0);
    memset(state->clb_virt, 0, 1024);
    
    port->clb = paging_get_phys(kernel_page_directory, (uint32_t)state->clb_virt);
    port->clbu = 0;

    // 2. FIS (256B aligned)
    state->fb_virt = kmalloc_a(256);
    if (!state->fb_virt) kernel_panic("AHCI OOM (FB)", "ahci.c", port_no, 0);
    memset(state->fb_virt, 0, 256);
    
    port->fb = paging_get_phys(kernel_page_directory, (uint32_t)state->fb_virt);
    port->fbu = 0;

    // 3. Command Tables
    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)state->clb_virt;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;
        
        void* ctba_virt = kmalloc_a(256);
        if (!ctba_virt) kernel_panic("AHCI OOM (CTBA)", "ahci.c", port_no, 0);
        memset(ctba_virt, 0, 256);
        
        state->ctba_virt[i] = ctba_virt;
        cmdheader[i].ctba = paging_get_phys(kernel_page_directory, (uint32_t)ctba_virt);
        cmdheader[i].ctbau = 0;
    }

    spinlock_init(&state->lock);

    start_cmd(port);
    state->port_mmio = (HBA_PORT*)port;
    state->active = 1;
}

static int check_type(volatile HBA_PORT *port) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3) return 0;
    if (ipm != 1) return 0;

    switch (port->sig) {
        case SATA_SIG_ATAPI: return 2;
        case SATA_SIG_SEMB:  return 0;
        case SATA_SIG_PM:    return 0;
        default: return 1;
    }
}

static void ahci_reset_controller(volatile HBA_MEM* abar) {
    abar->ghc |= HBA_GHC_AE;  
    
    abar->ghc |= HBA_GHC_HR;  
    
    int timeout = 1000000;
    
    while (timeout--) {
        if (abar->ghc == 0xFFFFFFFF) {
            kernel_panic("AHCI Read Error (0xFFFFFFFF)", "ahci.c", 0, 0);
        }

        if ((abar->ghc & HBA_GHC_HR) == 0) {
            break;
        }
    }
    
    if (timeout <= 0) {
        kernel_panic("AHCI Controller Reset Timeout", "ahci.c", 0, 0);
    }
    
    abar->ghc |= HBA_GHC_AE; 
}

void ahci_init(void) {
    memset(ports, 0, sizeof(ports));

    uint8_t bus, slot, func;
    if (!pci_find_ahci_device(&bus, &slot, &func)) {
        vga_print("[AHCI] No Controller Found.\n");
        return;
    }

    uint32_t bar5 = pci_get_bar5(bus, slot, func);
    if (bar5 == 0) {
        vga_print("[AHCI] Error: Invalid BAR5.\n");
        return;
    }
    
    uint32_t pci_cmd = pci_read(bus, slot, func, 0x04);
    if (!(pci_cmd & 0x04)) {
        pci_write(bus, slot, func, 0x04, pci_cmd | 0x04);
    }

    paging_map(kernel_page_directory, bar5, bar5, 3);
    paging_map(kernel_page_directory, bar5 + 4096, bar5 + 4096, 3);

    ahci_base_virt = (volatile HBA_MEM*)bar5;

    ahci_reset_controller(ahci_base_virt);
    
    ahci_base_virt->ghc &= ~HBA_GHC_IE;

    uint32_t pi = ahci_base_virt->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & 1) {
            int dt = check_type(&ahci_base_virt->ports[i]);
            if (dt == 1) {
                port_init(i);
                if (primary_port_idx == -1) primary_port_idx = i;
            }
        }
        pi >>= 1;
    }
}

static int ahci_send_command(int port_no, uint32_t lba, uint8_t* buf, int is_write) {
    ahci_port_state_t* state = &ports[port_no];

    uint32_t flags = spinlock_acquire_safe(&state->lock);

    volatile HBA_PORT* port = (volatile HBA_PORT*)state->port_mmio;

    port->is = (uint32_t)-1;
    
    int slot = find_cmdslot(port);
    if (slot == -1) {
        return 0;
        spinlock_release_safe(&state->lock, flags);
    }

    HBA_CMD_HEADER* cmdheader = (HBA_CMD_HEADER*)(state->clb_virt);
    cmdheader += slot;
    
    cmdheader->cfl = sizeof(FIS_REG_H2D)/4;
    cmdheader->w   = is_write ? 1 : 0;
    
    int use_bounce = 0;
    uint8_t* dma_target = buf;
    uint8_t* bounce_buffer = 0;
    uint32_t addr = (uint32_t)buf;

    if ((addr & 3) != 0 || ((addr & 0xFFFFF000) != ((addr + 512 - 1) & 0xFFFFF000))) {
        use_bounce = 1;
        
        bounce_buffer = (uint8_t*)kmalloc(512); 
        if (!bounce_buffer) {
            vga_print("[AHCI] Critical: OOM for bounce buffer\n");
            spinlock_release_safe(&state->lock, flags);
            return 0;
        }
        dma_target = bounce_buffer;

        if (is_write) {
            memcpy(bounce_buffer, buf, 512);
        }
    }

    HBA_CMD_TBL* cmdtbl = (HBA_CMD_TBL*)(state->ctba_virt[slot]);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    int prdt_idx = 0;
    uint32_t bytes_left = 512;
    uint32_t virt_curr = (uint32_t)dma_target;
    
    while (bytes_left > 0) {
        uint32_t phys_addr = paging_get_phys(kernel_page_directory, virt_curr);
        
        if (!phys_addr) {
            if (use_bounce) kfree(bounce_buffer);
            kernel_panic("AHCI DMA Fault: Page not mapped", "ahci.c", virt_curr, 0);
            spinlock_release_safe(&state->lock, flags);
            return 0;
        }

        uint32_t page_offset = virt_curr & 0xFFF;
        uint32_t bytes_in_page = 4096 - page_offset;
        
        uint32_t chunk_size = (bytes_left < bytes_in_page) ? bytes_left : bytes_in_page;

        cmdtbl->prdt_entry[prdt_idx].dba = phys_addr;
        cmdtbl->prdt_entry[prdt_idx].dbc = chunk_size - 1;
        cmdtbl->prdt_entry[prdt_idx].i   = 0;

        virt_curr += chunk_size;
        bytes_left -= chunk_size;
        prdt_idx++;
        
        if (prdt_idx >= 8) break; 
    }
    
    cmdheader->prdtl = prdt_idx;
    cmdtbl->prdt_entry[prdt_idx - 1].i = 1;


    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c        = 1; // Command bit
    cmdfis->command  = is_write ? 0x35 : 0x25; // DMA EXT Write / Read
    
    cmdfis->lba0     = (uint8_t)lba;
    cmdfis->lba1     = (uint8_t)(lba >> 8);
    cmdfis->lba2     = (uint8_t)(lba >> 16);
    cmdfis->device   = 1 << 6; // LBA Mode
    
    cmdfis->lba3     = (uint8_t)(lba >> 24);
    cmdfis->lba4     = 0;
    cmdfis->lba5     = 0;
    
    cmdfis->countl   = 1; 
    cmdfis->counth   = 0;

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 100000000) {
        spin++;
        __asm__ volatile("pause"); 
    }
    
    if (spin == 100000000) {
        if (use_bounce) kfree(bounce_buffer);
        vga_print("[AHCI] Error: Port Hung (BUSY timeout)\n");
        spinlock_release_safe(&state->lock, flags);
        return 0;
    }

    port->ci = 1 << slot;

    int timeout = 0;
    int success = 0;
    
    while (1) {
        if ((port->ci & (1 << slot)) == 0) {
            success = 1;
            break;
        }
        
        if (port->is & (1 << 30)) { 
             success = 0;
             vga_print("[AHCI] I/O Disk Error\n");
             break;
        }
        
        if (timeout++ > 200000000) {
            success = 0;
            vga_print("[AHCI] I/O Timeout\n");
            break;
        }
        __asm__ volatile("pause");
    }

    if (use_bounce) {
        if (success && !is_write) {
            memcpy(buf, bounce_buffer, 512);
        }
        kfree(bounce_buffer);
    }

    spinlock_release_safe(&state->lock, flags);

    return success;
}

int ahci_read_sector(uint32_t lba, uint8_t* buf) {
    if (primary_port_idx != -1) {
        return ahci_send_command(primary_port_idx, lba, buf, 0);
    }
    return 0;
}

int ahci_write_sector(uint32_t lba, const uint8_t* buf) {
    if (primary_port_idx != -1) {
        ahci_send_command(primary_port_idx, lba, (uint8_t*)buf, 1);
    }
    return 0;
}