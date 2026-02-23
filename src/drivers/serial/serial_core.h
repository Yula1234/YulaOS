#pragma once

#include <stddef.h>
#include <stdint.h>

#include <drivers/serial/ns16550.h>

#ifdef __cplusplus
extern "C" {
#endif

void serial_core_init(uint16_t port);

size_t serial_core_write(const void* data, size_t size);
size_t serial_core_read(void* data, size_t size);

size_t serial_core_rx_available(void);
size_t serial_core_tx_free(void);

void serial_core_poll(void);

void serial_core_console_write(void* ctx, const char* data, size_t size);

#ifdef __cplusplus
}
#endif
