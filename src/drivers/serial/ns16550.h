#pragma once

#include <stdint.h>

#ifdef __cplusplus

class NS16550 {
public:
    enum class Port : uint16_t {
        COM1 = 0x3F8,
        COM2 = 0x2F8,
    };

    static void init(Port port);

    static bool can_read(Port port);
    static bool can_write(Port port);

    static void putc(Port port, char c);
    static void puts(Port port, const char* s);

    static char getc(Port port);

private:
    enum class RegisterOffset : uint16_t {
        DATA = 0,
        IER = 1,
        IIR = 2,
        FCR = 2,
        LCR = 3,
        MCR = 4,
        LSR = 5,
        MSR = 6,
        SCR = 7,
    };

    enum class LcrBits : uint8_t {
        Dlab = 0x80,
        EightBits = 0x03,
        OneStop = 0x00,
        NoParity = 0x00,
    };

    enum class McrBits : uint8_t {
        Dtr = 0x01,
        Rts = 0x02,
        Out2 = 0x08,
        Loopback = 0x10,
    };

    enum class LsrBits : uint8_t {
        DataReady = 0x01,
        ThrEmpty = 0x20,
    };

    static uint16_t reg(Port port, RegisterOffset reg);

    static uint8_t read8(Port port, RegisterOffset reg);
    static void write8(Port port, RegisterOffset reg, uint8_t value);

    static void set_baud_divisor(Port port, uint16_t divisor);
    static bool loopback_self_test(Port port);
};

#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NS16550_COM1 = 0x3F8,
    NS16550_COM2 = 0x2F8,
};

void ns16550_init(uint16_t port);

int ns16550_can_read(uint16_t port);
int ns16550_can_write(uint16_t port);

void ns16550_putc(uint16_t port, char c);
char ns16550_getc(uint16_t port);

#ifdef __cplusplus
}
#endif
