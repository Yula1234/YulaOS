/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_USB_USB_URB_H
#define DRIVERS_USB_USB_URB_H

#include <stdint.h>

#include <drivers/usb/usb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usb_urb usb_urb_t;

typedef enum {
    USB_URB_CONTROL = 0,
    USB_URB_BULK = 1,
    USB_URB_ISOCH = 2,
} usb_urb_type_t;

enum {
    USB_URB_STATUS_OK = 0,
    USB_URB_STATUS_IOERROR = -1,
    USB_URB_STATUS_TIMEOUT = -2,
    USB_URB_STATUS_CANCELLED = -3,
    USB_URB_STATUS_NOT_SUPPORTED = -4,
};

typedef void (*usb_urb_complete_fn)(usb_urb_t* urb);

struct usb_urb {
    usb_urb_type_t type;

    uint8_t dev_addr;
    usb_speed_t speed;

    uint16_t ep0_mps;

    usb_setup_packet_t setup;

    void* buffer;
    uint16_t length;

    uint8_t ep_num;
    uint8_t dir_in;
    uint16_t max_packet;
    uint32_t transfer_buffer_length;

    uint8_t* toggle_io;

    uint32_t iso_start_frame;
    uint16_t iso_packet_count;
    uint16_t iso_frame_interval;

    uint32_t timeout_us;

    usb_urb_complete_fn complete;
    void* context;

    int status;
    uint32_t actual_length;

    void* hcpriv;
};

#ifdef __cplusplus
}
#endif

#endif
