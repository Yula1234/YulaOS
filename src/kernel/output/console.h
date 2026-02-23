#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*console_write_fn_t)(void* ctx, const char* data, size_t size);

void console_set_writer(console_write_fn_t writer, void* ctx);

void console_write(const char* data, size_t size);
void console_putc(char c);

#ifdef __cplusplus
}
#endif
