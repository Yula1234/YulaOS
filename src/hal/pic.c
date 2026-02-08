// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/io.h>

#include "pic.h"

#define PIC_MASTER_PORT 0x21u
#define PIC_SLAVE_PORT  0xA1u
#define PIC_MASK_KEYBOARD (1u << 1)
#define PIC_MASK_CASCADE  (1u << 2)
#define PIC_MASK_MOUSE    (1u << 4)
#define PIC_MASK_HDD      (1u << 6)

void pic_configure_legacy(void) {
    outb(PIC_MASTER_PORT, (uint8_t)~(PIC_MASK_KEYBOARD | PIC_MASK_CASCADE));
    outb(PIC_SLAVE_PORT, (uint8_t)~(PIC_MASK_MOUSE | PIC_MASK_HDD));
}
