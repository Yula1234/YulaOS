#include <drivers/usb/usb_core.h>
#include <drivers/usb/usb_hid_boot.h>

#include <drivers/keyboard.h>
#include <drivers/mouse.h>

#include <kernel/proc.h>

#include <mm/heap.h>

#include <lib/string.h>

extern volatile uint32_t timer_ticks;

#define USB_CLASS_HID 0x03u
#define USB_SUBCLASS_BOOT 0x01u

#define USB_PROTOCOL_BOOT_KBD 0x01u
#define USB_PROTOCOL_BOOT_MOUSE 0x02u

#define USB_REQ_HID_SET_PROTOCOL 0x0Bu
#define USB_REQ_HID_SET_IDLE 0x0Au

typedef struct {
    uint8_t addr;
    usb_speed_t speed;
    uint16_t ep0_mps;

    uint8_t iface;
    uint8_t proto;

    uint8_t ep_in;
    uint16_t ep_in_mps;
    uint8_t interval;

    usb_intr_pipe_t* intr;

    uint8_t kbd_mod;
    uint8_t kbd_keys[6];

    uint8_t kbd_repeat_usage;
    uint32_t kbd_repeat_next_tick;

    uint8_t mouse_btn;
} hid_boot_dev_t;

static hid_boot_dev_t g_devs[8];

static int hid_set_protocol(usb_device_t* dev, uint8_t iface, uint8_t boot) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE;
    setup.bRequest = USB_REQ_HID_SET_PROTOCOL;
    setup.wValue = boot ? 0u : 1u;
    setup.wIndex = iface;
    setup.wLength = 0;

    return usb_device_control_xfer(
        dev,
        &setup,
        0,
        0,
        1000000u
    ) >= 0;
}

static int hid_set_idle(usb_device_t* dev, uint8_t iface) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE;
    setup.bRequest = USB_REQ_HID_SET_IDLE;
    setup.wValue = 0;
    setup.wIndex = iface;
    setup.wLength = 0;

    return usb_device_control_xfer(
        dev,
        &setup,
        0,
        0,
        1000000u
    ) >= 0;
}

static hid_boot_dev_t* hid_alloc(void) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_devs) / sizeof(g_devs[0])); i++) {
        if (g_devs[i].addr == 0) {
            memset(&g_devs[i], 0, sizeof(g_devs[i]));
            return &g_devs[i];
        }
    }

    return 0;
}

typedef struct {
    uint8_t is_e0;
    uint8_t sc;
} ps2_sc_t;

static ps2_sc_t hid_usage_to_ps2_set1(uint8_t usage) {
    static const ps2_sc_t map[256] = {
        [0x04] = { 0, 0x1Eu }, [0x05] = { 0, 0x30u }, [0x06] = { 0, 0x2Eu }, [0x07] = { 0, 0x20u },
        [0x08] = { 0, 0x12u }, [0x09] = { 0, 0x21u }, [0x0A] = { 0, 0x22u }, [0x0B] = { 0, 0x23u },
        [0x0C] = { 0, 0x17u }, [0x0D] = { 0, 0x24u }, [0x0E] = { 0, 0x25u }, [0x0F] = { 0, 0x26u },
        [0x10] = { 0, 0x32u }, [0x11] = { 0, 0x31u }, [0x12] = { 0, 0x18u }, [0x13] = { 0, 0x19u },
        [0x14] = { 0, 0x10u }, [0x15] = { 0, 0x13u }, [0x16] = { 0, 0x1Fu }, [0x17] = { 0, 0x14u },
        [0x18] = { 0, 0x16u }, [0x19] = { 0, 0x2Fu }, [0x1A] = { 0, 0x11u }, [0x1B] = { 0, 0x2Du },
        [0x1C] = { 0, 0x15u }, [0x1D] = { 0, 0x2Cu },

        [0x1E] = { 0, 0x02u }, [0x1F] = { 0, 0x03u }, [0x20] = { 0, 0x04u }, [0x21] = { 0, 0x05u },
        [0x22] = { 0, 0x06u }, [0x23] = { 0, 0x07u }, [0x24] = { 0, 0x08u }, [0x25] = { 0, 0x09u },
        [0x26] = { 0, 0x0Au }, [0x27] = { 0, 0x0Bu },

        [0x28] = { 0, 0x1Cu }, [0x29] = { 0, 0x01u }, [0x2A] = { 0, 0x0Eu }, [0x2B] = { 0, 0x0Fu },
        [0x2C] = { 0, 0x39u }, [0x2D] = { 0, 0x0Cu }, [0x2E] = { 0, 0x0Du }, [0x2F] = { 0, 0x1Au },
        [0x30] = { 0, 0x1Bu }, [0x31] = { 0, 0x2Bu }, [0x33] = { 0, 0x27u }, [0x34] = { 0, 0x28u },
        [0x35] = { 0, 0x29u }, [0x36] = { 0, 0x33u }, [0x37] = { 0, 0x34u }, [0x38] = { 0, 0x35u },
        [0x39] = { 0, 0x3Au },

        [0x4F] = { 1, 0x4Du }, [0x50] = { 1, 0x4Bu }, [0x51] = { 1, 0x50u }, [0x52] = { 1, 0x48u },
    };

    return map[usage];
}

static void hid_send_ps2_scancode(ps2_sc_t sc, uint8_t make) {
    if (sc.sc == 0) {
        return;
    }

    if (sc.is_e0) {
        kbd_inject_scancode(0xE0u);
    }

    uint8_t v = sc.sc;
    if (!make) {
        v |= 0x80u;
    }

    kbd_inject_scancode(v);
}

static uint8_t hid_kbd_first_usage(const uint8_t keys[6]) {
    for (int i = 0; i < 6; i++) {
        if (keys[i] != 0) {
            return keys[i];
        }
    }

    return 0;
}

static int hid_kbd_has_key(const uint8_t keys[6], uint8_t code) {
    for (int i = 0; i < 6; i++) {
        if (keys[i] == code) {
            return 1;
        }
    }

    return 0;
}

static void hid_kbd_process(hid_boot_dev_t* d, const uint8_t* data, uint32_t len) {
    if (!d || len < 8) {
        return;
    }

    const uint32_t now = timer_ticks;

    const uint8_t mod = data[0];
    const uint8_t prev_mod = d->kbd_mod;

    const uint8_t* keys = &data[2];

    static const ps2_sc_t mod_sc[8] = {
        { 0, 0x1Du }, { 0, 0x2Au }, { 0, 0x38u }, { 1, 0x5Bu },
        { 1, 0x1Du }, { 0, 0x36u }, { 1, 0x38u }, { 1, 0x5Cu },
    };

    const uint8_t changed_mod = (uint8_t)(mod ^ prev_mod);
    if (changed_mod) {
        for (uint32_t i = 0; i < 8; i++) {
            const uint8_t bit = (uint8_t)(1u << i);
            if ((changed_mod & bit) == 0) {
                continue;
            }

            const uint8_t make = (mod & bit) ? 1u : 0u;
            hid_send_ps2_scancode(mod_sc[i], make);
        }
    }

    for (int i = 0; i < 6; i++) {
        const uint8_t prev = d->kbd_keys[i];
        if (prev == 0) {
            continue;
        }

        if (hid_kbd_has_key(keys, prev)) {
            continue;
        }

        hid_send_ps2_scancode(hid_usage_to_ps2_set1(prev), 0);
    }

    uint8_t repeat_usage = d->kbd_repeat_usage;
    if (repeat_usage && !hid_kbd_has_key(keys, repeat_usage)) {
        repeat_usage = 0;
    }

    for (int i = 0; i < 6; i++) {
        const uint8_t code = keys[i];
        if (code == 0) {
            continue;
        }

        if (hid_kbd_has_key(d->kbd_keys, code)) {
            continue;
        }

        hid_send_ps2_scancode(hid_usage_to_ps2_set1(code), 1);

        repeat_usage = code;
        d->kbd_repeat_next_tick = now + 240u;
    }

    if (repeat_usage && (uint32_t)(now - d->kbd_repeat_next_tick) < 0x80000000u) {
        hid_send_ps2_scancode(hid_usage_to_ps2_set1(repeat_usage), 1);
        d->kbd_repeat_next_tick = now + 30u;
    }

    if (!repeat_usage) {
        repeat_usage = hid_kbd_first_usage(keys);
        if (repeat_usage && d->kbd_repeat_usage != repeat_usage) {
            d->kbd_repeat_next_tick = now + 240u;
        }
    }

    d->kbd_mod = mod;
    memcpy(d->kbd_keys, keys, 6);
    d->kbd_repeat_usage = repeat_usage;
}

static void hid_mouse_process(hid_boot_dev_t* d, const uint8_t* data, uint32_t len) {
    if (!d || len < 3) {
        return;
    }

    const uint8_t buttons = data[0] & 0x07u;
    const int8_t dx = (int8_t)data[1];
    const int8_t dy = (int8_t)data[2];

    mouse_inject_delta((int)dx, (int)dy, (int)buttons);

    d->mouse_btn = buttons;
}

static void hid_intr_cb(void* ctx, const uint8_t* data, uint32_t len) {
    hid_boot_dev_t* d = (hid_boot_dev_t*)ctx;
    if (!d || !data || len == 0) {
        return;
    }

    if (d->proto == USB_PROTOCOL_BOOT_KBD) {
        hid_kbd_process(d, data, len);
        return;
    }

    if (d->proto == USB_PROTOCOL_BOOT_MOUSE) {
        hid_mouse_process(d, data, len);
        return;
    }
}

static int hid_parse_cfg(
    const uint8_t* cfg,
    uint16_t cfg_len,
    uint8_t* out_cfg_value,
    uint8_t* out_iface,
    uint8_t* out_proto,
    uint8_t* out_ep_in,
    uint16_t* out_ep_mps,
    uint8_t* out_ep_interval
) {
    if (!cfg || cfg_len < sizeof(usb_config_descriptor_t)) {
        return 0;
    }

    const usb_config_descriptor_t* cd = (const usb_config_descriptor_t*)cfg;
    if (cd->bLength < 9 || cd->bDescriptorType != USB_DESC_CONFIGURATION) {
        return 0;
    }

    *out_cfg_value = cd->bConfigurationValue;

    int in_hid = 0;
    uint8_t iface_num = 0;
    uint8_t proto = 0;

    uint16_t i = 0;
    while (i + 2 <= cfg_len) {
        const uint8_t blen = cfg[i + 0];
        const uint8_t dtype = cfg[i + 1];

        if (blen < 2) {
            break;
        }

        if ((uint32_t)i + (uint32_t)blen > cfg_len) {
            break;
        }

        if (dtype == USB_DESC_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t* id = (const usb_interface_descriptor_t*)&cfg[i];
            if (id->bInterfaceClass == USB_CLASS_HID && id->bInterfaceSubClass == USB_SUBCLASS_BOOT) {
                in_hid = 1;
                iface_num = id->bInterfaceNumber;
                proto = id->bInterfaceProtocol;
            } else {
                in_hid = 0;
            }
        } else if (dtype == USB_DESC_ENDPOINT && blen >= sizeof(usb_endpoint_descriptor_t)) {
            if (in_hid) {
                const usb_endpoint_descriptor_t* ed = (const usb_endpoint_descriptor_t*)&cfg[i];

                const uint8_t ep_addr = ed->bEndpointAddress;
                const uint8_t ep_attr = ed->bmAttributes & USB_EP_XFER_MASK;

                if ((ep_addr & USB_EP_DIR_IN) && ep_attr == USB_EP_XFER_INT) {
                    *out_iface = iface_num;
                    *out_proto = proto;
                    *out_ep_in = (uint8_t)(ep_addr & USB_EP_NUM_MASK);
                    *out_ep_mps = (uint16_t)(usb_le16_read(&ed->wMaxPacketSize) & 0x07FFu);
                    *out_ep_interval = ed->bInterval;
                    return 1;
                }
            }
        }

        i = (uint16_t)(i + blen);
    }

    return 0;
}

static int usb_hid_boot_probe(usb_device_t* dev, const uint8_t* cfg, uint16_t cfg_len) {
    if (!dev || !cfg || cfg_len == 0) {
        return 0;
    }

    uint8_t cfg_value = 0;

    uint8_t iface = 0;
    uint8_t proto = 0;

    uint8_t ep_in = 0;
    uint16_t ep_mps = 0;
    uint8_t interval = 0;

    if (!hid_parse_cfg(cfg, cfg_len, &cfg_value, &iface, &proto, &ep_in, &ep_mps, &interval)) {
        return 0;
    }

    if (proto != USB_PROTOCOL_BOOT_KBD && proto != USB_PROTOCOL_BOOT_MOUSE) {
        return 0;
    }

    hid_boot_dev_t* d = hid_alloc();
    if (!d) {
        return 0;
    }

    const usb_device_info_t* info = usb_device_get_info(dev);
    if (!info) {
        memset(d, 0, sizeof(*d));
        return 0;
    }

    d->addr = info->dev_addr;
    d->speed = info->speed;
    d->ep0_mps = info->ep0_mps;

    d->iface = iface;
    d->proto = proto;

    d->ep_in = ep_in;
    d->ep_in_mps = ep_mps ? ep_mps : 8;
    d->interval = interval ? interval : 10;

    (void)hid_set_idle(dev, iface);

    if (!hid_set_protocol(dev, iface, 1)) {
        memset(d, 0, sizeof(*d));
        return 0;
    }

    usb_intr_pipe_t* pipe = usb_device_intr_open(
        dev,
        d->ep_in,
        d->ep_in_mps,
        d->interval,
        hid_intr_cb,
        d
    );

    if (!pipe) {
        memset(d, 0, sizeof(*d));
        return 0;
    }

    d->intr = pipe;

    usb_device_set_class_private(dev, d);

    return 1;
}

static void usb_hid_boot_disconnect(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    hid_boot_dev_t* d = (hid_boot_dev_t*)usb_device_get_class_private(dev);
    if (!d) {
        return;
    }

    if (d->intr) {
        usb_device_intr_close(dev, d->intr);
        d->intr = 0;
    }

    usb_device_set_class_private(dev, 0);

    memset(d, 0, sizeof(*d));
}

static const usb_class_driver_t g_usb_hid_boot_drv = {
    .name = "usb_hid_boot",
    .probe = usb_hid_boot_probe,
    .disconnect = usb_hid_boot_disconnect,
};

int usb_hid_boot_init(void) {
    memset(g_devs, 0, sizeof(g_devs));

    return usb_register_class_driver(&g_usb_hid_boot_drv);
}
