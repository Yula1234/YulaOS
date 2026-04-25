/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_SERIAL_NS16550_H
#define DRIVERS_SERIAL_NS16550_H

#include <drivers/serial/core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS16550_COM1_BASE 0x3F8u
#define NS16550_COM2_BASE 0x2F8u

const uart_ops_t* ns16550_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif