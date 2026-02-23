#include <kernel/output/console.h>

#include <lib/cpp/lock_guard.h>

#include <stddef.h>

namespace {

static kernel::SpinLock g_console_lock;

static console_write_fn_t g_writer = nullptr;
static void* g_writer_ctx = nullptr;

static void console_write_unlocked(const char* data, size_t size) {
    if (!data || size == 0) {
        return;
    }

    console_write_fn_t writer = g_writer;
    void* ctx = g_writer_ctx;

    if (!writer) {
        return;
    }

    writer(ctx, data, size);
}

}

extern "C" void console_set_writer(console_write_fn_t writer, void* ctx) {
    kernel::SpinLockSafeGuard guard(g_console_lock);

    g_writer = writer;
    g_writer_ctx = ctx;
}

extern "C" void console_write(const char* data, size_t size) {
    kernel::SpinLockSafeGuard guard(g_console_lock);
    console_write_unlocked(data, size);
}

extern "C" void console_putc(char c) {
    console_write(&c, 1);
}
