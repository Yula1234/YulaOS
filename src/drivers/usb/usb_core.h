#ifndef DRIVERS_USB_USB_CORE_H
#define DRIVERS_USB_USB_CORE_H

#include <stdint.h>

#include <drivers/usb/usb.h>
#include <drivers/usb/usb_hcd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usb_device usb_device_t;

typedef struct {
    uint8_t dev_addr;
    usb_speed_t speed;
    uint16_t ep0_mps;

    usb_device_descriptor_t device_desc;

    uint8_t active_config;
    uint8_t iface_num;

    uint8_t hub_depth;
} usb_device_info_t;

typedef int (*usb_class_probe_t)(usb_device_t* dev, const uint8_t* cfg, uint16_t cfg_len);

typedef struct {
    const char* name;
    usb_class_probe_t probe;

    void (*disconnect)(usb_device_t* dev);
} usb_class_driver_t;

int usb_register_class_driver(const usb_class_driver_t* drv);

void usb_request_enumeration(usb_hcd_t* hcd);

void usb_device_set_class_private(usb_device_t* dev, void* p);
void* usb_device_get_class_private(usb_device_t* dev);

int usb_enumerate_child_device(usb_device_t* parent, uint8_t hub_port, usb_speed_t speed);

void usb_detach_device(usb_device_t* dev);
void usb_detach_child_device(usb_device_t* parent, uint8_t hub_port);

usb_hcd_t* usb_device_get_hcd(usb_device_t* dev);
const usb_device_info_t* usb_device_get_info(const usb_device_t* dev);

int usb_device_control_xfer(
    usb_device_t* dev,
    const usb_setup_packet_t* setup,
    void* data,
    uint16_t length,
    uint32_t timeout_us
);

int usb_device_bulk_xfer(
    usb_device_t* dev,
    uint8_t ep_num,
    uint8_t dir_in,
    uint16_t max_packet,
    void* data,
    uint32_t length,
    uint32_t timeout_us,
    uint8_t* toggle_io
);

usb_intr_pipe_t* usb_device_intr_open(
    usb_device_t* dev,
    uint8_t ep_num,
    uint16_t max_packet,
    uint8_t interval,
    usb_intr_cb_t cb,
    void* cb_ctx
);

void usb_device_intr_close(usb_device_t* dev, usb_intr_pipe_t* pipe);

int usb_register_hcd(usb_hcd_t* hcd);

#ifdef __cplusplus
}
#endif

#endif
