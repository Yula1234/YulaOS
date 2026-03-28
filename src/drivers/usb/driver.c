/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/driver.h>

#include <drivers/usb/core.h>
#include <drivers/usb/hub.h>
#include <drivers/usb/hid_boot.h>
#include <drivers/usb/msc.h>

static int usb_driver_init(void) {
    if (!usb_hub_init()) {
        return 0;
    }

    if (!usb_msc_init()) {
        return 0;
    }

    return usb_hid_boot_init() != 0;
}

DRIVER_REGISTER(
    .name = "usb",
    .klass = DRIVER_CLASS_PSEUDO,
    .stage = DRIVER_STAGE_CORE,
    .init = usb_driver_init,
    .shutdown = 0
);
