#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <hal/lock.h>

typedef enum {
    FIS_TYPE_REG_H2D    = 0x27,
    FIS_TYPE_REG_D2H    = 0x34,
    FIS_TYPE_DMA_ACT    = 0x39,
    FIS_TYPE_DMA_SETUP  = 0x41,
    FIS_TYPE_DATA       = 0x46,
    FIS_TYPE_BIST       = 0x58,
    FIS_TYPE_PIO_SETUP  = 0x5F,
    FIS_TYPE_DEV_BITS   = 0xA1,
} FIS_TYPE;

typedef struct {
    uint32_t clb;       // Command List Base Address (Phys)
    uint32_t clbu;
    uint32_t fb;        // FIS Base Address (Phys)
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) HBA_PORT;

typedef struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0-0x2C];
    uint8_t  vendor[0x100-0xA0];
    HBA_PORT ports[32];
} __attribute__((packed)) HBA_MEM;

typedef struct {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;      // Phys Addr of Command Table
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) HBA_CMD_HEADER;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} __attribute__((packed)) HBA_PRDT_ENTRY;

typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    HBA_PRDT_ENTRY prdt_entry[1]; // We support 1 PRDT entry for simplicity (up to 4MB)
} __attribute__((packed)) HBA_CMD_TBL;

typedef struct {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} __attribute__((packed)) FIS_REG_H2D;

typedef struct {
    int active;
    HBA_PORT* port_mmio; // Pointer to MMIO (mapped virtual address)
    
    // Virtual pointers to memory structures we allocated
    // We need these because 'port_mmio' only stores physical addresses
    void* clb_virt;         // Command List (Virtual)
    void* fb_virt;          // FIS (Virtual)
    void* ctba_virt[32];    // Command Tables (Virtual), one per slot
    spinlock_t lock;

    semaphore_t slot_sem[32]; 
} ahci_port_state_t;

void ahci_init(void);
int ahci_read_sector(uint32_t lba, uint8_t* buf);
int ahci_write_sector(uint32_t lba, const uint8_t* buf);
uint32_t ahci_get_capacity(void);
void ahci_set_async_mode(int enable);

#endif