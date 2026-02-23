#include <drivers/serial/ttyS0.h>

#include <drivers/serial/serial_core.h>

#include <fs/vfs.h>
#include <lib/string.h>
#include <yos/ioctl.h>

#include <kernel/term/term.h>
#include <kernel/tty/tty_internal.h>
#include <kernel/tty/tty_service.h>
#include <kernel/tty/line_discipline.h>

#include <stddef.h>
#include <stdint.h>

namespace {

static kernel::tty::LineDiscipline g_ld;
static yos_termios_t g_termios;

static kernel::term::Term* active_term(void) {
    tty_handle_t* active = kernel::tty::TtyService::instance().get_active_for_render();
    return tty_term_ptr(active);
}

static size_t serial_emit(const uint8_t* data, size_t size, void* ctx) {
    (void)ctx;
    return serial_core_write(data, size);
}

static size_t echo_emit(const uint8_t* data, size_t size, void* ctx) {
    (void)ctx;

    size_t w = serial_core_write(data, size);

    kernel::term::Term* term = active_term();
    if (term && size != 0) {
        term->write((const char*)data, (uint32_t)size);
        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
    }

    return w;
}

static void drain_rx(void) {
    uint8_t buf[64];

    for (;;) {
        serial_core_poll();

        size_t n = serial_core_read(buf, sizeof(buf));
        if (n == 0) {
            break;
        }

        g_ld.receive_bytes(buf, n);
    }
}

static kernel::tty::LineDisciplineConfig config_from_termios(const yos_termios_t& t) {
    kernel::tty::LineDisciplineConfig cfg;

    (void)t;

    return cfg;
}

int ttyS0_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    drain_rx();

    size_t n = g_ld.read(buffer, size);
    return (int)n;
}

int ttyS0_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    size_t n = g_ld.write_transform(buffer, size, serial_emit, 0);

    kernel::term::Term* term = active_term();
    if (term && size != 0) {
        term->write((const char*)buffer, size);
        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
    }

    return (int)n;
}

int ttyS0_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    if (req == YOS_TCGETS) {
        if (!arg) {
            return -1;
        }

        memcpy(arg, &g_termios, sizeof(g_termios));

        return 0;
    }

    if (req == YOS_TCSETS) {
        if (!arg) {
            return -1;
        }

        memcpy(&g_termios, arg, sizeof(g_termios));

        kernel::tty::LineDisciplineConfig cfg = config_from_termios(g_termios);
        g_ld.set_config(cfg);

        return 0;
    }

    return -1;
}

vfs_ops_t ttyS0_ops = {
    ttyS0_vfs_read,
    ttyS0_vfs_write,
    0,
    0,
    ttyS0_vfs_ioctl,
};

vfs_node_t ttyS0_node = {
    "ttyS0",
    0,
    0,
    0,
    0,
    &ttyS0_ops,
    0,
    0,
    0,
};

}

extern "C" void ttyS0_init(void) {
    memset(&g_termios, 0, sizeof(g_termios));
    g_ld.set_echo_emitter(echo_emit, 0);

    devfs_register(&ttyS0_node);
}
