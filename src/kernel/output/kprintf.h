#pragma once

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus

#include <drivers/serial/ns16550.h>

namespace kernel::output {

using SerialPort = NS16550::Port;

void set_serial_port(SerialPort port);
SerialPort serial_port(void);

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
