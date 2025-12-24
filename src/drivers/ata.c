#include "ata.h"
#include "pci.h"
#include "../hal/io.h"
#include "../hal/irq.h"
#include "../kernel/proc.h"
#include "../kernel/sched.h"
#include "../arch/i386/paging.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../drivers/vga.h"

#define ATA_DATA        0x1F0
#define ATA_SEC_COUNT   0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7
#define ATA_CONTROL     0x3F6

#define BM_COMMAND      0x00
#define BM_STATUS       0x02
#define BM_PRDT_ADDR    0x04

#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_READ_DMA  0xC8
#define ATA_CMD_WRITE_DMA 0xCA

typedef struct {
    uint32_t phys_addr;
    uint16_t byte_count;
    uint16_t reserved;
} __attribute__((packed)) prd_t;

static uint32_t ide_bar4 = 0;
static volatile int ata_irq_fired = 0;

static prd_t*    prdt_virt = 0;
static uint32_t  prdt_phys = 0;
static uint8_t*  dma_buf_virt = 0;
static uint32_t  dma_buf_phys = 0;

void ata_irq_handler(registers_t* regs) {
    (void)regs;
    __attribute__((unused)) uint8_t status = inb(ATA_STATUS);
    if (ide_bar4) {
        uint8_t bm_status = inb(ide_bar4 + BM_STATUS);
        outb(ide_bar4 + BM_STATUS, bm_status | 4);
    }
    ata_irq_fired = 1;
    proc_wake_up_waiters(0);
}

void ata_init() {
    ide_bar4 = pci_find_ide_bar4();
    outb(ATA_CONTROL, 0x00);
    irq_install_handler(14, ata_irq_handler);

    if (ide_bar4) {
        prdt_virt = (prd_t*)kmalloc_a(4096);
        dma_buf_virt = (uint8_t*)kmalloc_a(4096);
        prdt_phys = paging_get_phys(kernel_page_directory, (uint32_t)prdt_virt);
        dma_buf_phys = paging_get_phys(kernel_page_directory, (uint32_t)dma_buf_virt);
        memset(prdt_virt, 0, 4096);
        memset(dma_buf_virt, 0, 4096);
    }
}

static int ata_wait_busy() {
    int timeout = 100000;
    while (timeout--) {
        uint8_t status = inb(ATA_STATUS);
        if ((status & 0x80) == 0) return 1; // BSY clear
        if (status & 0x01) return 0; // ERR
        if (status & 0x20) return 0; // DF
        __asm__ volatile("pause");
    }
    return 0;
}

static int ata_wait_drq() {
    int timeout = 100000;
    while (timeout--) {
        uint8_t status = inb(ATA_STATUS);
        if (status & 0x08) return 1; // DRQ set
        __asm__ volatile("pause");
    }
    return 0;
}

static void ata_pio_read(uint32_t lba, uint8_t* buf) {
    ata_wait_busy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_COUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_drq()) {
        __asm__ volatile("cli");
        __asm__ volatile ("cld; rep insw" : : "d"(ATA_DATA), "D"(buf), "c"(256) : "memory");
        __asm__ volatile("sti");
    }
}

static void ata_pio_write(uint32_t lba, const uint8_t* buf) {
    ata_wait_busy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_COUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    if (ata_wait_drq()) {
        __asm__ volatile("cli");
        __asm__ volatile ("cld; rep outsw" : : "d"(ATA_DATA), "S"(buf), "c"(256) : "memory");
        __asm__ volatile("sti");
        outb(ATA_COMMAND, 0xE7);
        ata_wait_busy();
    }
}

static int ata_dma_rw_sector(uint32_t lba, int is_write) {
    if (!ide_bar4) return 0;

    prdt_virt[0].phys_addr = dma_buf_phys;
    prdt_virt[0].byte_count = 512;
    prdt_virt[0].reserved = 0x8000;

    outl(ide_bar4 + BM_PRDT_ADDR, prdt_phys);
    outb(ide_bar4 + BM_COMMAND, is_write ? 0x00 : 0x08);
    outb(ide_bar4 + BM_STATUS, 0x06);

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SEC_COUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));

    ata_irq_fired = 0;
    outb(ATA_COMMAND, is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);
    outb(ide_bar4 + BM_COMMAND, (is_write ? 0x00 : 0x08) | 0x01);

    int timeout = 500000; 
    while (!ata_irq_fired && timeout-- > 0) {
        sched_yield();
    }

    outb(ide_bar4 + BM_COMMAND, is_write ? 0x00 : 0x08);

    if (timeout <= 0) {
        return 0; // Fail
    }
    
    return 1; // Success
}

void ata_read_sector(uint32_t lba, uint8_t* buf) {
    if (ata_dma_rw_sector(lba, 0)) {
        memcpy(buf, dma_buf_virt, 512);
        return;
    }
    ata_pio_read(lba, buf);
}

void ata_write_sector(uint32_t lba, const uint8_t* buf) {
    memcpy(dma_buf_virt, buf, 512);
    if (ata_dma_rw_sector(lba, 1)) {
        return;
    }
    ata_pio_write(lba, buf);
}