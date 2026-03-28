#ifndef DRIVERS_USB_USB_HCD_H
#define DRIVERS_USB_USB_HCD_H

#include <stdint.h>
#include <stddef.h>

#include <drivers/usb/usb.h>
#include <drivers/usb/usb_urb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usb_hcd usb_hcd_t;

typedef struct {
    uint8_t connected;
    usb_speed_t speed;
} usb_port_status_t;

typedef void (*usb_intr_cb_t)(void* ctx, const uint8_t* data, uint32_t len);

typedef struct usb_intr_pipe usb_intr_pipe_t;

typedef struct {
    int (*start)(usb_hcd_t* hcd);
    void (*stop)(usb_hcd_t* hcd);

    uint8_t (*root_port_count)(usb_hcd_t* hcd);
    int (*root_port_get_status)(usb_hcd_t* hcd, uint8_t port, usb_port_status_t* out);
    int (*root_port_reset)(usb_hcd_t* hcd, uint8_t port);

    int (*submit_urb)(usb_hcd_t* hcd, usb_urb_t* urb);

    int (*cancel_urb)(usb_hcd_t* hcd, usb_urb_t* urb);

    usb_intr_pipe_t* (*intr_open)(
        usb_hcd_t* hcd,
        uint8_t dev_addr,
        usb_speed_t speed,
        uint8_t ep_num,
        uint16_t max_packet,
        uint8_t interval,
        usb_intr_cb_t cb,
        void* cb_ctx
    );

    void (*intr_close)(usb_hcd_t* hcd, usb_intr_pipe_t* pipe);
} usb_hcd_ops_t;

struct usb_hcd {
    const char* name;

    const usb_hcd_ops_t* ops;

    void* private_data;
};

#ifdef __cplusplus
}
#endif

#endif
