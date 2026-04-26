/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <kernel/locking/spinlock.h>
#include <kernel/locking/sem.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    uint32_t ctba;
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
    HBA_PRDT_ENTRY prdt_entry[1]; 
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
    void* clb_virt;
    void* fb_virt;
    void* ctba_virt[32];
    spinlock_t lock;
    semaphore_t slot_sem[32]; 
} ahci_port_state_t;

int ahci_read_sectors(uint32_t lba, uint32_t count, uint8_t* buf);
int ahci_write_sectors(uint32_t lba, uint32_t count, const uint8_t* buf);

static inline int ahci_read_sector(uint32_t lba, uint8_t* buf) {
    return ahci_read_sectors(lba, 1, buf);
}
static inline int ahci_write_sector(uint32_t lba, const uint8_t* buf) {
    return ahci_write_sectors(lba, 1, buf);
}

uint32_t ahci_get_capacity(void);
void ahci_set_async_mode(int enable);

int ahci_msi_configure_cpu(int cpu_index);

#ifdef __cplusplus
}
#endif

#endif