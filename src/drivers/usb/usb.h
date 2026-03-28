/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_USB_USB_H
#define DRIVERS_USB_USB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_SPEED_LOW = 1,
    USB_SPEED_FULL = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_SUPER = 4,
} usb_speed_t;

typedef enum {
    USB_XFER_CONTROL = 0,
    USB_XFER_ISOCH = 1,
    USB_XFER_BULK = 2,
    USB_XFER_INTERRUPT = 3,
} usb_xfer_type_t;

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
} usb_desc_header_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_endpoint_descriptor_t;

#define USB_DESC_DEVICE 1u
#define USB_DESC_CONFIGURATION 2u
#define USB_DESC_STRING 3u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT 5u

#define USB_REQ_GET_STATUS 0u
#define USB_REQ_CLEAR_FEATURE 1u
#define USB_REQ_SET_FEATURE 3u
#define USB_REQ_SET_ADDRESS 5u
#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_DESCRIPTOR 7u
#define USB_REQ_GET_CONFIGURATION 8u
#define USB_REQ_SET_CONFIGURATION 9u

#define USB_REQ_TYPE_DIR_IN 0x80u
#define USB_REQ_TYPE_DIR_OUT 0x00u

#define USB_REQ_TYPE_STANDARD 0x00u
#define USB_REQ_TYPE_CLASS 0x20u
#define USB_REQ_TYPE_VENDOR 0x40u

#define USB_REQ_RECIP_DEVICE 0x00u
#define USB_REQ_RECIP_INTERFACE 0x01u
#define USB_REQ_RECIP_ENDPOINT 0x02u
#define USB_REQ_RECIP_OTHER 0x03u

#define USB_EP_DIR_IN 0x80u
#define USB_EP_NUM_MASK 0x0Fu

#define USB_EP_XFER_MASK 0x03u
#define USB_EP_XFER_CONTROL 0x00u
#define USB_EP_XFER_ISOCH 0x01u
#define USB_EP_XFER_BULK 0x02u
#define USB_EP_XFER_INT 0x03u

static inline uint16_t usb_le16_read(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

#ifdef __cplusplus
}
#endif

#endif
