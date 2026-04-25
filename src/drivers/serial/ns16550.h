/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_SERIAL_NS16550_H
#define DRIVERS_SERIAL_NS16550_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS16550_COM1 0x3F8u
#define NS16550_COM2 0x2F8u

void ns16550_init(uint16_t port);

int ns16550_can_read(uint16_t port);
int ns16550_can_write(uint16_t port);

void ns16550_putc(uint16_t port, char c);
char ns16550_getc(uint16_t port);

#ifdef __cplusplus
}
#endif

#endif