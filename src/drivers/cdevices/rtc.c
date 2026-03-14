#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <hal/io.h>

#include <stdint.h>

static int rtc_is_updating(void) {
    outb(0x70, 0x0Au);
    return (inb(0x71) & 0x80u) != 0u;
}

static uint8_t rtc_get_register(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0Fu) + ((v >> 4) * 10u));
}

static void rtc_read_time(uint8_t* out_h, uint8_t* out_m, uint8_t* out_s) {
    if (!out_h || !out_m || !out_s) {
        return;
    }

    uint8_t h0 = 0u;
    uint8_t m0 = 0u;
    uint8_t s0 = 0u;

    uint8_t h1 = 0u;
    uint8_t m1 = 0u;
    uint8_t s1 = 0u;

    for (;;) {
        while (rtc_is_updating()) {
        }

        s0 = rtc_get_register(0x00u);
        m0 = rtc_get_register(0x02u);
        h0 = rtc_get_register(0x04u);

        while (rtc_is_updating()) {
        }

        s1 = rtc_get_register(0x00u);
        m1 = rtc_get_register(0x02u);
        h1 = rtc_get_register(0x04u);

        if (s0 == s1 && m0 == m1 && h0 == h1) {
            break;
        }
    }

    const uint8_t reg_b = rtc_get_register(0x0Bu);

    const int is_binary = (reg_b & 0x04u) != 0u;
    const int is_24h = (reg_b & 0x02u) != 0u;

    uint8_t s = s0;
    uint8_t m = m0;
    uint8_t h = h0;

    if (!is_binary) {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);

        const uint8_t h_bcd = (uint8_t)(h & 0x7Fu);
        h = bcd_to_bin(h_bcd);
    }

    if (!is_24h) {
        const int is_pm = (h0 & 0x80u) != 0u;
        if (is_pm) {
            if (h < 12u) {
                h = (uint8_t)(h + 12u);
            }
        } else {
            if (h == 12u) {
                h = 0u;
            }
        }
    }

    h = (uint8_t)((h + 5u) % 24u);

    *out_h = h;
    *out_m = m;
    *out_s = s;
}

static uint32_t rtc_format_time(char out[8]) {
    uint8_t h = 0u;
    uint8_t m = 0u;
    uint8_t s = 0u;

    rtc_read_time(&h, &m, &s);

    out[0] = (char)((h / 10u) + '0');
    out[1] = (char)((h % 10u) + '0');
    out[2] = ':';
    out[3] = (char)((m / 10u) + '0');
    out[4] = (char)((m % 10u) + '0');
    out[5] = ':';
    out[6] = (char)((s / 10u) + '0');
    out[7] = (char)((s % 10u) + '0');

    return 9u;
}

static int rtc_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (!node) {
        return -1;
    }

    if (!buffer || size == 0u) {
        return 0;
    }

    char tmp[8];
    const uint32_t len = rtc_format_time(tmp);

    node->size = len;

    if (offset >= len) {
        return 0;
    }

    uint32_t available = len - offset;
    uint32_t to_copy = (size < available) ? size : available;

    const char* src = &tmp[offset];
    char* dst = (char*)buffer;

    for (uint32_t i = 0u; i < to_copy; i++) {
        dst[i] = src[i];
    }

    return (int)to_copy;
}

static cdevice_t g_rtc_cdev = {
    .dev = {
        .name = "rtc",
    },
    .ops = {
        .read = rtc_read,
    },
    .node_template = {
        .name = "rtc",
    },
};

static int rtc_driver_init(void) {
    g_rtc_cdev.node_template.size = 8u;

    return cdevice_register(&g_rtc_cdev);
}

DRIVER_REGISTER(
    .name = "rtc",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = rtc_driver_init,
    .shutdown = 0
);
