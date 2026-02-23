#include "ns16550.h"

#include <hal/io.h>

namespace {

constexpr uint32_t uart_clock_hz = 1843200u;
constexpr uint32_t default_baud = 115200u;

constexpr uint8_t bit_lsr_data_ready = 0x01u;
constexpr uint8_t bit_lsr_thr_empty = 0x20u;

constexpr uint8_t bit_lcr_dlab = 0x80u;
constexpr uint8_t lcr_8n1 = 0x03u;

constexpr uint8_t bit_mcr_dtr = 0x01u;
constexpr uint8_t bit_mcr_rts = 0x02u;
constexpr uint8_t bit_mcr_out2 = 0x08u;
constexpr uint8_t bit_mcr_loopback = 0x10u;

constexpr uint8_t fcr_enable_clear_14b = 0xC7u;

}

uint16_t NS16550::reg(Port port, RegisterOffset r) {
    return static_cast<uint16_t>(port) + static_cast<uint16_t>(r);
}

uint8_t NS16550::read8(Port port, RegisterOffset r) {
    return inb(reg(port, r));
}

void NS16550::write8(Port port, RegisterOffset r, uint8_t value) {
    outb(reg(port, r), value);
}

bool NS16550::can_read(Port port) {
    return (read8(port, RegisterOffset::LSR) & bit_lsr_data_ready) != 0u;
}

bool NS16550::can_write(Port port) {
    return (read8(port, RegisterOffset::LSR) & bit_lsr_thr_empty) != 0u;
}

void NS16550::set_baud_divisor(Port port, uint16_t divisor) {
    const uint8_t lcr = read8(port, RegisterOffset::LCR);

    write8(port, RegisterOffset::LCR, lcr | bit_lcr_dlab);
    write8(port, RegisterOffset::DATA, static_cast<uint8_t>(divisor & 0xFFu));
    write8(port, RegisterOffset::IER, static_cast<uint8_t>((divisor >> 8) & 0xFFu));
    write8(port, RegisterOffset::LCR, lcr & static_cast<uint8_t>(~bit_lcr_dlab));
}

bool NS16550::loopback_self_test(Port port) {
    const uint8_t saved_mcr = read8(port, RegisterOffset::MCR);

    write8(port, RegisterOffset::MCR, bit_mcr_loopback);

    write8(port, RegisterOffset::DATA, 0xAEu);
    io_wait();

    const uint8_t got = read8(port, RegisterOffset::DATA);

    write8(port, RegisterOffset::MCR, saved_mcr);

    return got == 0xAEu;
}

void NS16550::init(Port port) {
    write8(port, RegisterOffset::IER, 0u);

    write8(port, RegisterOffset::LCR, lcr_8n1);

    const uint16_t divisor = static_cast<uint16_t>(uart_clock_hz / (16u * default_baud));
    set_baud_divisor(port, divisor);

    write8(port, RegisterOffset::FCR, fcr_enable_clear_14b);

    write8(port, RegisterOffset::MCR, bit_mcr_dtr | bit_mcr_rts | bit_mcr_out2);

    (void)loopback_self_test(port);

    (void)read8(port, RegisterOffset::DATA);
    (void)read8(port, RegisterOffset::LSR);
    (void)read8(port, RegisterOffset::MSR);
}

void NS16550::putc(Port port, char c) {
    while (!can_write(port)) {
    }

    if (c == '\n') {
        putc(port, '\r');
    }

    write8(port, RegisterOffset::DATA, static_cast<uint8_t>(c));
}

void NS16550::puts(Port port, const char* s) {
    if (!s) {
        return;
    }

    for (const char* p = s; *p != '\0'; ++p) {
        putc(port, *p);
    }
}

char NS16550::getc(Port port) {
    while (!can_read(port)) {
    }

    return static_cast<char>(read8(port, RegisterOffset::DATA));
}

extern "C" void ns16550_init(uint16_t port) {
    NS16550::init(static_cast<NS16550::Port>(port));
}

extern "C" int ns16550_can_read(uint16_t port) {
    return NS16550::can_read(static_cast<NS16550::Port>(port)) ? 1 : 0;
}

extern "C" int ns16550_can_write(uint16_t port) {
    return NS16550::can_write(static_cast<NS16550::Port>(port)) ? 1 : 0;
}

extern "C" void ns16550_putc(uint16_t port, char c) {
    NS16550::putc(static_cast<NS16550::Port>(port), c);
}

extern "C" char ns16550_getc(uint16_t port) {
    return NS16550::getc(static_cast<NS16550::Port>(port));
}
