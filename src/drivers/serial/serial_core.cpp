#include <drivers/serial/serial_core.h>

#include <lib/cpp/lock_guard.h>

#include <stddef.h>
#include <stdint.h>

namespace {

static kernel::SpinLock g_serial_core_lock;
static NS16550::Port g_port = NS16550::Port::COM1;

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

static void pump_rx_unlocked(void) {
    while (NS16550::can_read(g_port)) {
        char c = NS16550::getc(g_port);
        (void)g_rx.push(static_cast<uint8_t>(c));
    }
}

static void pump_tx_unlocked(void) {
    while (g_tx.count != 0 && NS16550::can_write(g_port)) {
        uint8_t b = 0;
        if (!g_tx.pop(b)) {
            break;
        }

        NS16550::putc(g_port, static_cast<char>(b));
    }
}

}

extern "C" void serial_core_init(uint16_t port) {
    kernel::SpinLockSafeGuard guard(g_serial_core_lock);

    g_port = static_cast<NS16550::Port>(port);
    g_rx = Ring{};
    g_tx = Ring{};

    pump_rx_unlocked();
    pump_tx_unlocked();
}

extern "C" size_t serial_core_write(const void* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    kernel::SpinLockSafeGuard guard(g_serial_core_lock);

    pump_rx_unlocked();

    size_t written = 0;
    while (written < size) {
        if (g_tx.free_space() == 0) {
            pump_tx_unlocked();

            if (g_tx.free_space() == 0) {
                break;
            }
        }

        if (!g_tx.push(bytes[written])) {
            break;
        }

        written++;
    }

    pump_tx_unlocked();

    return written;
}

extern "C" size_t serial_core_read(void* data, size_t size) {
    if (!data || size == 0) {
        return 0;
    }

    uint8_t* bytes = static_cast<uint8_t*>(data);

    kernel::SpinLockSafeGuard guard(g_serial_core_lock);

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
    kernel::SpinLockSafeGuard guard(g_serial_core_lock);
    pump_rx_unlocked();
    return g_rx.count;
}

extern "C" size_t serial_core_tx_free(void) {
    kernel::SpinLockSafeGuard guard(g_serial_core_lock);
    pump_tx_unlocked();
    return g_tx.free_space();
}

extern "C" void serial_core_poll(void) {
    kernel::SpinLockSafeGuard guard(g_serial_core_lock);
    pump_rx_unlocked();
    pump_tx_unlocked();
}

extern "C" void serial_core_console_write(void* ctx, const char* data, size_t size) {
    (void)ctx;

    (void)serial_core_write(data, size);
}
