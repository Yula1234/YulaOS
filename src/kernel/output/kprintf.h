#pragma once

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus

namespace kernel::output {
int kvprintf(const char* fmt, va_list ap);
int kprintf(const char* fmt, ...);

}

extern "C" {

#endif

int kvprintf(const char* fmt, va_list ap);
int kprintf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
