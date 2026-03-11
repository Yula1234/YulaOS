#include <drivers/serial/serial_core.h>

#include <lib/cpp/lock_guard.h>
#include <hal/io.h>
#include <hal/irq.h>

#include <stddef.h>
#include <stdint.h>

namespace {

static NS16550::Port g_port = NS16550::Port::COM1;

static __cacheline_aligned spinlock_t g_serial_core_lock;

static constexpr size_t kRxCap = 4096;
static constexpr size_t kTxCap = 4096;

struct Ring {
    uint8_t data[4096];
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;

    size_t capacity() const {
        return sizeof(data);
    }

    size_t free_space() const {
        return capacity() - count;
    }

    bool push(uint8_t b) {
        if (count == capacity()) {
            return false;
        }

        data[head] = b;
        head++;
        if (head == capacity()) {
            head = 0;
        }

        count++;
        return true;
    }

    bool pop(uint8_t& out) {
        if (count == 0) {
            return false;
        }

        out = data[tail];
        tail++;
        if (tail == capacity()) {
            tail = 0;
        }

        count--;
        return true;
    }
};

static Ring g_rx;
static Ring g_tx;

static constexpr uint16_t kRegData = 0;
static constexpr uint16_t kRegIer  = 1;
static constexpr uint16_t kRegIir  = 2;
static constexpr uint16_t kRegLsr  = 5;

static constexpr uint8_t kIerBitRda  = 0x01u;
static constexpr uint8_t kIerBitThre = 0x02u;

static constexpr uint8_t kLsrBitDataReady = 0x01u;
static constexpr uint8_t kLsrBitThrEmpty  = 0x20u;

static inline uint16_t port_reg(uint16_t ofs) {
    return static_cast<uint16_t>(g_port) + ofs;
}

static inline uint8_t uart_read8(uint16_t ofs) {
    return inb(port_reg(ofs));
}

static inline void uart_write8(uint16_t ofs, uint8_t v) {
    outb(port_reg(ofs), v);
}

static inline uint8_t uart_get_ier(void) {
    return uart_read8(kRegIer);
}

static inline void uart_set_ier(uint8_t v) {
    uart_write8(kRegIer, v);
}

static inline void uart_enable_thre_irq(void) {
    uart_set_ier(static_cast<uint8_t>(uart_get_ier() | kIerBitThre));
}

static inline void uart_disable_thre_irq(void) {
    uart_set_ier(static_cast<uint8_t>(uart_get_ier() & ~kIerBitThre));
}

static void pump_rx_unlocked(void) {
    while (NS16550::can_read(g_port)) {
        char c = NS16550::getc(g_port);
        (void)g_rx.push(static_cast<uint8_t>(c));
    }
}

static void pump_tx_irq_unlocked(void) {
    while (g_tx.count != 0) {
        const uint8_t lsr = uart_read8(kRegLsr);
        if ((lsr & kLsrBitThrEmpty) == 0u) {
            break;
        }

        uint8_t b = 0;
        if (!g_tx.pop(b)) {
            break;
        }

        uart_write8(kRegData, b);
    }

    if (g_tx.count == 0) {
        uart_disable_thre_irq();
    } else {
        uart_enable_thre_irq();
    }
}

static void serial_core_irq_handler(registers_t* regs) {
    (void)regs;

    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);

    (void)uart_read8(kRegIir);

    pump_rx_unlocked();
    pump_tx_irq_unlocked();
}

}

extern "C" void serial_core_init(uint16_t port) {
    spinlock_init(&g_serial_core_lock);

    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);

    g_port = static_cast<NS16550::Port>(port);
    g_rx = Ring{};
    g_tx = Ring{};

    pump_rx_unlocked();

    uart_disable_thre_irq();

    irq_install_handler(4, serial_core_irq_handler);

    outb(0x21, static_cast<uint8_t>(inb(0x21) & ~(1u << 4)));
}

extern "C" size_t serial_core_write(const void* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);

    pump_rx_unlocked();

    size_t written = 0;
    while (written < size) {
        if (g_tx.free_space() == 0) {
            break;
        }

        if (!g_tx.push(bytes[written])) {
            break;
        }

        written++;
    }

    if (written != 0) {
        uart_enable_thre_irq();
        pump_tx_irq_unlocked();
    }

    return written;
}

extern "C" size_t serial_core_read(void* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    uint8_t* bytes = static_cast<uint8_t*>(data);

    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);

    pump_rx_unlocked();

    size_t read = 0;
    while (read < size) {
        uint8_t b = 0;
        if (!g_rx.pop(b)) {
            break;
        }

        bytes[read++] = b;
    }

    return read;
}

extern "C" size_t serial_core_rx_available(void) {
    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);
    pump_rx_unlocked();
    return g_rx.count;
}

extern "C" size_t serial_core_tx_free(void) {
    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);
    return g_tx.free_space();
}

extern "C" void serial_core_poll(void) {
    kernel::SpinLockNativeSafeGuard guard(g_serial_core_lock);
    pump_rx_unlocked();
    pump_tx_irq_unlocked();
}

extern "C" void serial_core_console_write(void* ctx, const char* data, size_t size) {
    (void)ctx;

    (void)serial_core_write(data, size);
}
