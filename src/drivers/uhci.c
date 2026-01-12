#include <drivers/pci.h>
#include <drivers/acpi.h>

#include <drivers/keyboard.h>
#include <drivers/mouse.h>

#include <arch/i386/paging.h>

#include <hal/io.h>
#include <hal/irq.h>
#include <hal/ioapic.h>

#include <hal/lock.h>

#include <kernel/cpu.h>

#include <mm/heap.h>

#include <lib/string.h>

#include "uhci.h"

extern volatile uint32_t timer_ticks;

#define UHCI_KBD_REPEAT_DELAY_TICKS  1900u
#define UHCI_KBD_REPEAT_RATE_TICKS    180u

#define UHCI_PCI_CLASS_SERIAL_BUS 0x0Cu
#define UHCI_PCI_SUBCLASS_USB     0x03u
#define UHCI_PCI_PROGIF_UHCI      0x00u

#define UHCI_PCI_REG_COMMAND      0x04u
#define UHCI_PCI_REG_BAR4         0x20u
#define UHCI_PCI_REG_IRQ_LINE     0x3Cu
#define UHCI_PCI_REG_LEGSUP       0xC0u

#define UHCI_PCI_LEGSUP_OS_OWNED  0x2000u

#define UHCI_PCI_CMD_IO_SPACE     (1u << 0)
#define UHCI_PCI_CMD_BUS_MASTER   (1u << 2)
#define UHCI_PCI_CMD_INTX_DISABLE (1u << 10)

#define UHCI_REG_USBCMD     0x00u
#define UHCI_REG_USBSTS     0x02u
#define UHCI_REG_USBINTR    0x04u
#define UHCI_REG_USBFRNUM   0x06u
#define UHCI_REG_USBFLBASE  0x08u
#define UHCI_REG_USBSOF     0x0Cu
#define UHCI_REG_USBPORTSC1 0x10u
#define UHCI_REG_USBPORTSC2 0x12u

#define UHCI_USBCMD_RUN        (1u << 0)
#define UHCI_USBCMD_HCRESET    (1u << 1)
#define UHCI_USBCMD_GRESET     (1u << 2)
#define UHCI_USBCMD_CF         (1u << 6)
#define UHCI_USBCMD_MAXP       (1u << 7)

#define UHCI_USBINTR_TIMEOUT_CRC (1u << 0)
#define UHCI_USBINTR_RESUME      (1u << 1)
#define UHCI_USBINTR_IOC         (1u << 2)
#define UHCI_USBINTR_SHORT_PKT   (1u << 3)

#define UHCI_USBSTS_CLEAR_ALL     0xFFFFu

#define UHCI_FRAME_LIST_ENTRIES 1024u
#define UHCI_FRAME_LIST_BYTES   (UHCI_FRAME_LIST_ENTRIES * sizeof(uint32_t))

#define UHCI_USBSOF_DEFAULT 0x40u

#define UHCI_RESET_WAIT_IO_LOOPS 20000u

#define UHCI_PIC_MASTER_DATA_PORT 0x21u
#define UHCI_PIC_SLAVE_DATA_PORT  0xA1u
#define UHCI_PIC_MASTER_CASCADE_IRQ 2u

static int g_uhci_initialized = 0;
static uint8_t g_uhci_irq_line = 0xFFu;

static spinlock_t g_uhci_sched_lock;
static int g_uhci_can_sleep = 0;

static uint16_t g_uhci_io_base = 0;

static uint32_t* g_uhci_frame_list = 0;
static uint32_t g_uhci_frame_list_phys = 0;

static uhci_qh_t* g_uhci_async_qh = 0;
static uint32_t g_uhci_async_qh_phys = 0;

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

#define USB_DESC_DEVICE        1u
#define USB_DESC_CONFIGURATION 2u
#define USB_DESC_INTERFACE     4u
#define USB_DESC_ENDPOINT      5u

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_ADDRESS    5u
#define USB_REQ_SET_CONFIG     9u

#define USB_REQ_HID_SET_PROTOCOL 0x0Bu
#define USB_REQ_HID_SET_IDLE     0x0Au

#define USB_CLASS_HID 0x03u
#define USB_SUBCLASS_BOOT 0x01u

#define USB_PROTOCOL_BOOT_KBD   0x01u
#define USB_PROTOCOL_BOOT_MOUSE 0x02u

#define USB_EP_DIR_IN 0x80u
#define USB_EP_XFER_INT 0x03u

typedef struct {
    int present;
    uint8_t port;
    uint8_t low_speed;

    uint8_t addr;
    uint8_t ep0_mps;

    uint8_t iface_num;
    uint8_t hid_protocol;

    uint8_t ep_in;
    uint16_t ep_in_mps;
    uint8_t ep_interval;

    uhci_qh_t* intr_qh;
    uhci_td_t* intr_td;
    uint8_t* intr_buf;
    uint32_t intr_buf_phys;
    uint8_t intr_toggle;
    uint8_t intr_reported;

    uint8_t kbd_prev_mod;
    uint8_t kbd_prev_keys[6];

    uint8_t kbd_repeat_key;
    uint32_t kbd_repeat_next_tick;
} uhci_hid_dev_t;

static uhci_hid_dev_t g_hid_devs[2];

static inline void sys_usleep(uint32_t us) {
    __asm__ volatile("int $0x80" : : "a"(11), "b"(us));
}

static inline uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFCu);
    return (uint8_t)((reg >> ((offset & 3u) * 8u)) & 0xFFu);
}

__attribute__((unused)) static inline uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFCu);
    return (uint16_t)((reg >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

__attribute__((unused)) static inline void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint8_t aligned = offset & 0xFCu;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    uint32_t shift = (uint32_t)(offset & 2u) * 8u;
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    pci_write(bus, slot, func, aligned, reg);
}

static inline uint16_t uhci_readw(uint16_t reg) {
    return inw((uint16_t)(g_uhci_io_base + reg));
}

static inline void uhci_writew(uint16_t reg, uint16_t val) {
    outw((uint16_t)(g_uhci_io_base + reg), val);
}

__attribute__((unused)) static inline uint32_t uhci_readl(uint16_t reg) {
    return inl((uint16_t)(g_uhci_io_base + reg));
}

static inline void uhci_writel(uint16_t reg, uint32_t val) {
    outl((uint16_t)(g_uhci_io_base + reg), val);
}

static inline void uhci_writeb(uint16_t reg, uint8_t val) {
    outb((uint16_t)(g_uhci_io_base + reg), val);
}

static void uhci_wait_io(uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        io_wait();
    }
}

static void uhci_irq_handler(registers_t* regs) {
    (void)regs;

    if (!g_uhci_initialized) {
        return;
    }

    uint16_t st = uhci_readw(UHCI_REG_USBSTS);

    if (st) {
        uhci_writew(UHCI_REG_USBSTS, st);
    }
}

static inline uint16_t uhci_port_reg(uint8_t port) {
    if (port == 1) return UHCI_REG_USBPORTSC1;
    return UHCI_REG_USBPORTSC2;
}

static inline uint16_t uhci_port_read(uint8_t port) {
    return uhci_readw(uhci_port_reg(port));
}

static inline void uhci_port_write(uint8_t port, uint16_t v) {
    uhci_writew(uhci_port_reg(port), v);
}

static void uhci_port_set(uint8_t port, uint16_t bits) {
    uint16_t st = uhci_port_read(port);
    st |= bits;
    st &= (uint16_t)~UHCI_PORTSC_RWC;
    uhci_port_write(port, st);
}

static void uhci_port_clear(uint8_t port, uint16_t bits) {
    uint16_t st = uhci_port_read(port);
    st &= (uint16_t)~UHCI_PORTSC_RWC;
    st &= (uint16_t)~bits;
    st |= (uint16_t)(UHCI_PORTSC_RWC & bits);
    uhci_port_write(port, st);
}

static int uhci_port_is_connected(uint8_t port) {
    return (uhci_port_read(port) & UHCI_PORTSC_CCS) != 0;
}

static int uhci_port_is_low_speed(uint8_t port) {
    return (uhci_port_read(port) & UHCI_PORTSC_LSDA) != 0;
}

static int uhci_port_reset_enable(uint8_t port) {
    uhci_port_clear(port, UHCI_PORTSC_RWC);

    uhci_port_set(port, UHCI_PORTSC_PR);
    sys_usleep(50000);
    uhci_port_clear(port, UHCI_PORTSC_PR);
    sys_usleep(10000);

    uhci_port_set(port, UHCI_PORTSC_PE);
    sys_usleep(10000);
    uhci_port_clear(port, UHCI_PORTSC_RWC);

    uint16_t st = uhci_port_read(port);
    return (st & UHCI_PORTSC_CCS) && (st & UHCI_PORTSC_PE);
}

static inline uint32_t uhci_td_maxlen_field(uint16_t len) {
    if (len == 0) return UHCI_TD_TOKEN_MAXLEN_MASK;
    return ((uint32_t)(len - 1u)) & UHCI_TD_TOKEN_MAXLEN_MASK;
}

static uhci_td_t* uhci_alloc_td(void) {
    uhci_td_t* td = (uhci_td_t*)kmalloc_a(sizeof(uhci_td_t));
    if (!td) return 0;
    memset(td, 0, sizeof(*td));
    uint32_t phys = paging_get_phys(kernel_page_directory, (uint32_t)td);
    if (!phys || (phys & 0xFu) != 0u) {
        kfree(td);
        return 0;
    }
    td->sw_phys = phys;
    return td;
}

static uhci_qh_t* uhci_alloc_qh(void) {
    uhci_qh_t* qh = (uhci_qh_t*)kmalloc_a(sizeof(uhci_qh_t));
    if (!qh) return 0;
    memset(qh, 0, sizeof(*qh));
    uint32_t phys = paging_get_phys(kernel_page_directory, (uint32_t)qh);
    if (!phys || (phys & 0xFu) != 0u) {
        kfree(qh);
        return 0;
    }
    qh->sw_phys = phys;
    return qh;
}

static void uhci_free_td_chain(uhci_td_t* td) {
    while (td) {
        uhci_td_t* next = (uhci_td_t*)(uintptr_t)td->sw_next;
        kfree(td);
        td = next;
    }
}

static void uhci_sched_insert_head_qh(uhci_qh_t* qh) {
    uint32_t flags = spinlock_acquire_safe(&g_uhci_sched_lock);

    qh->link = g_uhci_async_qh->link;
    g_uhci_async_qh->link = (qh->sw_phys | UHCI_PTR_QH);

    spinlock_release_safe(&g_uhci_sched_lock, flags);
}

static void uhci_sched_remove_head_qh(uhci_qh_t* qh) {
    uint32_t flags = spinlock_acquire_safe(&g_uhci_sched_lock);

    if ((g_uhci_async_qh->link & ~0xFu) == qh->sw_phys) {
        g_uhci_async_qh->link = qh->link;
    }

    spinlock_release_safe(&g_uhci_sched_lock, flags);
}

static int uhci_wait_qh_done(uhci_qh_t* qh, uint32_t timeout_us) {
    uint32_t waited = 0;

    while (1) {
        __sync_synchronize();
        if (qh->element & UHCI_PTR_T) {
            return 1;
        }

        if (timeout_us && waited >= timeout_us) {
            return 0;
        }

        if (g_uhci_can_sleep) {
            sys_usleep(1000);
            waited += 1000;
        } else {
            uhci_wait_io(1000);
        }
    }
}

static int uhci_control_transfer(uint8_t devaddr, uint8_t low_speed, uint16_t ep0_mps,
                                 const usb_setup_packet_t* setup, void* data, uint16_t length,
                                 uint32_t timeout_us) {
    if (!g_uhci_initialized || !g_uhci_async_qh) {
        return -1;
    }

    uint8_t dir_in = (setup->bmRequestType & 0x80u) != 0;
    uint16_t max_packet = ep0_mps ? ep0_mps : 8;

    usb_setup_packet_t* setup_dma = (usb_setup_packet_t*)kmalloc_a(sizeof(*setup_dma));
    if (!setup_dma) return -1;
    memcpy(setup_dma, setup, sizeof(*setup_dma));
    uint32_t setup_phys = paging_get_phys(kernel_page_directory, (uint32_t)setup_dma);
    if (!setup_phys) {
        kfree(setup_dma);
        return -1;
    }

    uint8_t* data_dma = 0;
    uint32_t data_phys = 0;
    if (length) {
        data_dma = (uint8_t*)kmalloc_a(length);
        if (!data_dma) {
            kfree(setup_dma);
            return -1;
        }
        if (dir_in) {
            memset(data_dma, 0, length);
        } else {
            if (!data) {
                kfree(data_dma);
                kfree(setup_dma);
                return -1;
            }
            memcpy(data_dma, data, length);
        }

        data_phys = paging_get_phys(kernel_page_directory, (uint32_t)data_dma);
        if (!data_phys) {
            kfree(data_dma);
            kfree(setup_dma);
            return -1;
        }
    }

    uhci_td_t* td_setup = uhci_alloc_td();
    if (!td_setup) {
        if (data_dma) kfree(data_dma);
        kfree(setup_dma);
        return -1;
    }

    td_setup->link = UHCI_PTR_T;
    td_setup->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
    if (low_speed) td_setup->status |= UHCI_TD_CTRL_LS;
    td_setup->token =
        (uhci_td_maxlen_field(sizeof(*setup_dma)) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        (0u << UHCI_TD_TOKEN_D_SHIFT) |
        (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)devaddr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        UHCI_TD_PID_SETUP;
    td_setup->buffer = setup_phys;

    uhci_td_t* td_first = td_setup;
    uhci_td_t* td_prev = td_setup;

    uint32_t total_in = 0;
    uint8_t toggle = 1;

    uint32_t remaining = length;
    uint32_t offset = 0;
    while (remaining) {
        uint16_t pkt = (remaining > max_packet) ? max_packet : (uint16_t)remaining;

        uhci_td_t* td = uhci_alloc_td();
        if (!td) {
            uhci_free_td_chain(td_first);
            if (data_dma) kfree(data_dma);
            kfree(setup_dma);
            return -1;
        }

        td_prev->link = (td->sw_phys | UHCI_PTR_DEPTH);
        td_prev->sw_next = (uint32_t)(uintptr_t)td;

        td->link = UHCI_PTR_T;
        td->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
        if (low_speed) td->status |= UHCI_TD_CTRL_LS;
        if (dir_in) td->status |= UHCI_TD_CTRL_SPD;

        td->token =
            (uhci_td_maxlen_field(pkt) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
            ((uint32_t)toggle << UHCI_TD_TOKEN_D_SHIFT) |
            (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
            ((uint32_t)devaddr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
            (dir_in ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT);

        td->buffer = data_phys + offset;

        td_prev = td;
        toggle ^= 1u;

        remaining -= pkt;
        offset += pkt;
    }

    uhci_td_t* td_status = uhci_alloc_td();
    if (!td_status) {
        uhci_free_td_chain(td_first);
        if (data_dma) kfree(data_dma);
        kfree(setup_dma);
        return -1;
    }

    td_prev->link = (td_status->sw_phys | UHCI_PTR_DEPTH);
    td_prev->sw_next = (uint32_t)(uintptr_t)td_status;

    td_status->link = UHCI_PTR_T;
    td_status->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_IOC;
    if (low_speed) td_status->status |= UHCI_TD_CTRL_LS;

    uint8_t status_pid = UHCI_TD_PID_IN;
    if (length) {
        status_pid = dir_in ? UHCI_TD_PID_OUT : UHCI_TD_PID_IN;
    }

    td_status->token =
        (uhci_td_maxlen_field(0) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        (1u << UHCI_TD_TOKEN_D_SHIFT) |
        (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)devaddr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        status_pid;
    td_status->buffer = 0;

    uhci_qh_t* qh = uhci_alloc_qh();
    if (!qh) {
        uhci_free_td_chain(td_first);
        if (data_dma) kfree(data_dma);
        kfree(setup_dma);
        return -1;
    }

    qh->link = UHCI_PTR_T;
    qh->element = td_first->sw_phys;

    uhci_sched_insert_head_qh(qh);

    int ok = uhci_wait_qh_done(qh, timeout_us);

    uhci_sched_remove_head_qh(qh);

    if (g_uhci_can_sleep) {
        sys_usleep(2000);
    } else {
        uhci_wait_io(2000);
    }

    if (!ok) {
        qh->element = UHCI_PTR_T;
        kfree(qh);
        uhci_free_td_chain(td_first);
        if (data_dma) kfree(data_dma);
        kfree(setup_dma);
        return -1;
    }

    int success = 1;
    uhci_td_t* td = td_first;
    while (td) {
        uint32_t st = td->status;
        if (st & (UHCI_TD_CTRL_STALLED | UHCI_TD_CTRL_DBUFERR | UHCI_TD_CTRL_BABBLE | UHCI_TD_CTRL_CRCTIMEO | UHCI_TD_CTRL_BITSTUFF)) {
            success = 0;
            break;
        }
        td = (uhci_td_t*)(uintptr_t)td->sw_next;
    }

    if (success && dir_in && length && data && data_dma) {
        td = (uhci_td_t*)(uintptr_t)td_setup->sw_next;
        uint32_t remaining_in = length;
        while (td && remaining_in) {
            uint32_t st = td->status;
            uint32_t al = st & UHCI_TD_CTRL_ACTLEN_MASK;
            uint32_t got = (al == UHCI_TD_CTRL_ACTLEN_MASK) ? 0u : (al + 1u);
            if (got > remaining_in) got = remaining_in;
            total_in += got;
            if (got == 0) break;
            if (got < max_packet) break;
            remaining_in -= got;
            td = (uhci_td_t*)(uintptr_t)td->sw_next;
        }

        if (total_in > length) total_in = length;
        memcpy(data, data_dma, total_in);
    }

    qh->element = UHCI_PTR_T;
    kfree(qh);
    uhci_free_td_chain(td_first);
    if (data_dma) kfree(data_dma);
    kfree(setup_dma);

    if (!success) return -1;
    if (dir_in && length) return (int)total_in;
    return (int)length;
}

static inline uint16_t usb_le16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static int uhci_usb_get_descriptor(uint8_t addr, uint8_t low_speed, uint16_t ep0_mps,
                                  uint8_t desc_type, uint8_t desc_index,
                                  void* out, uint16_t out_len) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)desc_type << 8) | desc_index);
    setup.wIndex = 0;
    setup.wLength = out_len;
    return uhci_control_transfer(addr, low_speed, ep0_mps, &setup, out, out_len, 1000000);
}

static int uhci_usb_set_address(uint8_t low_speed, uint16_t ep0_mps, uint8_t new_addr) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    int r = uhci_control_transfer(0, low_speed, ep0_mps, &setup, 0, 0, 1000000);
    if (r < 0) return 0;
    sys_usleep(10000);
    return 1;
}

static int uhci_usb_set_config(uint8_t addr, uint8_t low_speed, uint16_t ep0_mps, uint8_t cfg_value) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_CONFIG;
    setup.wValue = cfg_value;
    setup.wIndex = 0;
    setup.wLength = 0;
    return uhci_control_transfer(addr, low_speed, ep0_mps, &setup, 0, 0, 1000000) >= 0;
}

static int uhci_hid_set_protocol(uint8_t addr, uint8_t low_speed, uint16_t ep0_mps, uint8_t iface, uint8_t protocol_boot) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x21;
    setup.bRequest = USB_REQ_HID_SET_PROTOCOL;
    setup.wValue = protocol_boot;
    setup.wIndex = iface;
    setup.wLength = 0;
    return uhci_control_transfer(addr, low_speed, ep0_mps, &setup, 0, 0, 1000000) >= 0;
}

static int uhci_hid_set_idle(uint8_t addr, uint8_t low_speed, uint16_t ep0_mps, uint8_t iface) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x21;
    setup.bRequest = USB_REQ_HID_SET_IDLE;
    setup.wValue = 0;
    setup.wIndex = iface;
    setup.wLength = 0;
    return uhci_control_transfer(addr, low_speed, ep0_mps, &setup, 0, 0, 1000000) >= 0;
}

static int uhci_hid_parse_cfg(const uint8_t* cfg, uint16_t cfg_len,
                              uint8_t* out_cfg_value,
                              uint8_t* out_iface,
                              uint8_t* out_protocol,
                              uint8_t* out_ep_in,
                              uint16_t* out_ep_mps,
                              uint8_t* out_ep_interval) {
    if (cfg_len < sizeof(usb_config_descriptor_t)) return 0;

    const usb_config_descriptor_t* cd = (const usb_config_descriptor_t*)cfg;
    if (cd->bLength < 9 || cd->bDescriptorType != USB_DESC_CONFIGURATION) return 0;

    *out_cfg_value = cd->bConfigurationValue;

    int in_hid = 0;
    uint8_t iface_num = 0;
    uint8_t hid_proto = 0;

    uint16_t i = 0;
    while (i + 2 <= cfg_len) {
        uint8_t blen = cfg[i + 0];
        uint8_t dtype = cfg[i + 1];
        if (blen < 2) break;
        if (i + blen > cfg_len) break;

        if (dtype == USB_DESC_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t* id = (const usb_interface_descriptor_t*)&cfg[i];
            if (id->bInterfaceClass == USB_CLASS_HID) {
                in_hid = 1;
                iface_num = id->bInterfaceNumber;
                if (id->bInterfaceProtocol == USB_PROTOCOL_BOOT_KBD || id->bInterfaceProtocol == USB_PROTOCOL_BOOT_MOUSE) {
                    hid_proto = id->bInterfaceProtocol;
                } else {
                    hid_proto = USB_PROTOCOL_BOOT_MOUSE;
                }
            } else {
                in_hid = 0;
            }
        } else if (dtype == USB_DESC_ENDPOINT && blen >= sizeof(usb_endpoint_descriptor_t)) {
            if (in_hid) {
                const usb_endpoint_descriptor_t* ed = (const usb_endpoint_descriptor_t*)&cfg[i];
                uint8_t ep_addr = ed->bEndpointAddress;
                uint8_t ep_attr = ed->bmAttributes & 0x03u;
                if ((ep_addr & USB_EP_DIR_IN) && ep_attr == USB_EP_XFER_INT) {
                    *out_iface = iface_num;
                    *out_protocol = hid_proto;
                    *out_ep_in = (uint8_t)(ep_addr & 0x0Fu);
                    *out_ep_mps = (uint16_t)(usb_le16(&ed->wMaxPacketSize) & 0x07FFu);
                    *out_ep_interval = ed->bInterval;
                    return 1;
                }
            }
        }

        i = (uint16_t)(i + blen);
    }
    return 0;
}

static int uhci_hid_setup_interrupt(uhci_hid_dev_t* dev) {
    if (!dev || !dev->present) return 0;
    if (!dev->addr || !dev->ep_in) return 0;

    if (dev->ep_in_mps == 0) dev->ep_in_mps = 8;
    if (dev->ep_in_mps > 64) dev->ep_in_mps = 64;

    dev->intr_buf = (uint8_t*)kmalloc_a(dev->ep_in_mps);
    if (!dev->intr_buf) return 0;
    memset(dev->intr_buf, 0, dev->ep_in_mps);
    dev->intr_buf_phys = paging_get_phys(kernel_page_directory, (uint32_t)dev->intr_buf);
    if (!dev->intr_buf_phys) return 0;

    dev->intr_td = uhci_alloc_td();
    dev->intr_qh = uhci_alloc_qh();
    if (!dev->intr_td || !dev->intr_qh) return 0;

    dev->intr_toggle = 0;
    dev->intr_reported = 0;

    dev->intr_td->link = UHCI_PTR_T;
    dev->intr_td->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_SPD;
    if (dev->low_speed) dev->intr_td->status |= UHCI_TD_CTRL_LS;
    dev->intr_td->token =
        (uhci_td_maxlen_field(dev->ep_in_mps) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        ((uint32_t)dev->intr_toggle << UHCI_TD_TOKEN_D_SHIFT) |
        ((uint32_t)dev->ep_in << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)dev->addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        UHCI_TD_PID_IN;
    dev->intr_td->buffer = dev->intr_buf_phys;

    dev->intr_qh->link = UHCI_PTR_T;
    dev->intr_qh->element = dev->intr_td->sw_phys;

    uhci_sched_insert_head_qh(dev->intr_qh);
    return 1;
}

static void uhci_hid_kbd_process(uhci_hid_dev_t* dev, const uint8_t* rep, uint32_t rep_len);
static void uhci_hid_mouse_process(const uint8_t* rep, uint32_t rep_len);

static void uhci_hid_poll_dev(uhci_hid_dev_t* dev) {
    if (!dev || !dev->present || !dev->intr_td || !dev->intr_qh) return;

    __sync_synchronize();

    uint32_t st = dev->intr_td->status;
    if (st & UHCI_TD_CTRL_ACTIVE) {
        return;
    }

    int bad = 0;
    if (st & (UHCI_TD_CTRL_STALLED | UHCI_TD_CTRL_DBUFERR | UHCI_TD_CTRL_BABBLE | UHCI_TD_CTRL_CRCTIMEO | UHCI_TD_CTRL_BITSTUFF)) {
        bad = 1;
    }

    if (!bad) {
        uint32_t al = st & UHCI_TD_CTRL_ACTLEN_MASK;
        uint32_t got = (al == UHCI_TD_CTRL_ACTLEN_MASK) ? 0u : (al + 1u);
        if (got > dev->ep_in_mps) got = dev->ep_in_mps;

        if (got) {
            if (!dev->intr_reported) {
                dev->intr_reported = 1;
            }
            if (dev->hid_protocol == USB_PROTOCOL_BOOT_KBD) {
                uhci_hid_kbd_process(dev, dev->intr_buf, got);
            } else if (dev->hid_protocol == USB_PROTOCOL_BOOT_MOUSE) {
                uhci_hid_mouse_process(dev->intr_buf, got);
            }

            dev->intr_toggle ^= 1u;
        }
    }

    dev->intr_td->link = UHCI_PTR_T;
    dev->intr_td->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_SPD;
    if (dev->low_speed) dev->intr_td->status |= UHCI_TD_CTRL_LS;
    dev->intr_td->token =
        (uhci_td_maxlen_field(dev->ep_in_mps) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        ((uint32_t)dev->intr_toggle << UHCI_TD_TOKEN_D_SHIFT) |
        ((uint32_t)dev->ep_in << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)dev->addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        UHCI_TD_PID_IN;
    dev->intr_td->buffer = dev->intr_buf_phys;

    dev->intr_qh->element = dev->intr_td->sw_phys;

    __sync_synchronize();
}

static void uhci_kbd_send_scancode(uint8_t sc, int is_e0, int is_break) {
    if (is_e0) {
        kbd_handle_scancode(0xE0);
    }

    if (is_break) {
        kbd_handle_scancode((uint8_t)(sc | 0x80u));
    } else {
        kbd_handle_scancode(sc);
    }
}

static int uhci_hid_to_set1(uint8_t hid, uint8_t* out_sc, int* out_e0) {
    *out_e0 = 0;

    if (hid >= 0x04 && hid <= 0x1D) {
        static const uint8_t table[26] = {
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
        };
        *out_sc = table[hid - 0x04];
        return 1;
    }

    if (hid >= 0x1E && hid <= 0x27) {
        static const uint8_t table[10] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B };
        *out_sc = table[hid - 0x1E];
        return 1;
    }

    switch (hid) {
        case 0x28: *out_sc = 0x1C; return 1;
        case 0x29: *out_sc = 0x01; return 1;
        case 0x2A: *out_sc = 0x0E; return 1;
        case 0x2B: *out_sc = 0x0F; return 1;
        case 0x2C: *out_sc = 0x39; return 1;
        case 0x2D: *out_sc = 0x0C; return 1;
        case 0x2E: *out_sc = 0x0D; return 1;
        case 0x2F: *out_sc = 0x1A; return 1;
        case 0x30: *out_sc = 0x1B; return 1;
        case 0x31: *out_sc = 0x2B; return 1;
        case 0x33: *out_sc = 0x27; return 1;
        case 0x34: *out_sc = 0x28; return 1;
        case 0x35: *out_sc = 0x29; return 1;
        case 0x36: *out_sc = 0x33; return 1;
        case 0x37: *out_sc = 0x34; return 1;
        case 0x38: *out_sc = 0x35; return 1;
        case 0x39: *out_sc = 0x3A; return 1;
        case 0x4F: *out_sc = 0x4D; *out_e0 = 1; return 1;
        case 0x50: *out_sc = 0x4B; *out_e0 = 1; return 1;
        case 0x51: *out_sc = 0x50; *out_e0 = 1; return 1;
        case 0x52: *out_sc = 0x48; *out_e0 = 1; return 1;
        default: break;
    }

    return 0;
}

static int uhci_arr_contains_u8(const uint8_t* a, uint32_t n, uint8_t v) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] == v) return 1;
    }
    return 0;
}

static void uhci_kbd_repeat_tick(uhci_hid_dev_t* dev) {
    if (!dev || !dev->present) return;
    if (dev->hid_protocol != USB_PROTOCOL_BOOT_KBD) return;

    if (dev->kbd_repeat_key == 0) return;
    if (!uhci_arr_contains_u8(dev->kbd_prev_keys, 6, dev->kbd_repeat_key)) {
        dev->kbd_repeat_key = 0;
        return;
    }

    uint32_t now = timer_ticks;
    if ((int32_t)(now - dev->kbd_repeat_next_tick) < 0) return;

    for (int burst = 0; burst < 4; burst++) {
        if ((int32_t)(now - dev->kbd_repeat_next_tick) < 0) break;

        uint8_t sc;
        int e0;
        if (uhci_hid_to_set1(dev->kbd_repeat_key, &sc, &e0)) {
            uhci_kbd_send_scancode(sc, e0, 0);
        }

        dev->kbd_repeat_next_tick += UHCI_KBD_REPEAT_RATE_TICKS;
    }

    if ((int32_t)(now - dev->kbd_repeat_next_tick) >= 0) {
        dev->kbd_repeat_next_tick = now + UHCI_KBD_REPEAT_RATE_TICKS;
    }
}

static void uhci_hid_kbd_process(uhci_hid_dev_t* dev, const uint8_t* rep, uint32_t rep_len) {
    if (rep_len < 8) return;

    uint32_t now = timer_ticks;

    uint8_t mod = rep[0];
    const uint8_t* keys = &rep[2];

    uint8_t mod_lctrl = (mod & (1u << 0)) ? 1u : 0u;
    uint8_t mod_lshift = (mod & (1u << 1)) ? 1u : 0u;
    uint8_t mod_lalt = (mod & (1u << 2)) ? 1u : 0u;
    uint8_t mod_rctrl = (mod & (1u << 4)) ? 1u : 0u;
    uint8_t mod_rshift = (mod & (1u << 5)) ? 1u : 0u;
    uint8_t mod_ralt = (mod & (1u << 6)) ? 1u : 0u;

    uint8_t prev_lctrl = (dev->kbd_prev_mod & (1u << 0)) ? 1u : 0u;
    uint8_t prev_lshift = (dev->kbd_prev_mod & (1u << 1)) ? 1u : 0u;
    uint8_t prev_lalt = (dev->kbd_prev_mod & (1u << 2)) ? 1u : 0u;
    uint8_t prev_rctrl = (dev->kbd_prev_mod & (1u << 4)) ? 1u : 0u;
    uint8_t prev_rshift = (dev->kbd_prev_mod & (1u << 5)) ? 1u : 0u;
    uint8_t prev_ralt = (dev->kbd_prev_mod & (1u << 6)) ? 1u : 0u;

    if (mod_lctrl != prev_lctrl) uhci_kbd_send_scancode(0x1D, 0, mod_lctrl ? 0 : 1);
    if (mod_lshift != prev_lshift) uhci_kbd_send_scancode(0x2A, 0, mod_lshift ? 0 : 1);
    if (mod_lalt != prev_lalt) uhci_kbd_send_scancode(0x38, 0, mod_lalt ? 0 : 1);
    if (mod_rctrl != prev_rctrl) uhci_kbd_send_scancode(0x1D, 1, mod_rctrl ? 0 : 1);
    if (mod_rshift != prev_rshift) uhci_kbd_send_scancode(0x36, 0, mod_rshift ? 0 : 1);
    if (mod_ralt != prev_ralt) uhci_kbd_send_scancode(0x38, 1, mod_ralt ? 0 : 1);

    for (uint32_t i = 0; i < 6; i++) {
        uint8_t k = dev->kbd_prev_keys[i];
        if (k == 0) continue;
        if (!uhci_arr_contains_u8(keys, 6, k)) {
            uint8_t sc;
            int e0;
            if (uhci_hid_to_set1(k, &sc, &e0)) {
                uhci_kbd_send_scancode(sc, e0, 1);
            }

            if (dev->kbd_repeat_key == k) {
                dev->kbd_repeat_key = 0;
            }
        }
    }

    for (uint32_t i = 0; i < 6; i++) {
        uint8_t k = keys[i];
        if (k == 0) continue;
        if (!uhci_arr_contains_u8(dev->kbd_prev_keys, 6, k)) {
            uint8_t sc;
            int e0;
            if (uhci_hid_to_set1(k, &sc, &e0)) {
                uhci_kbd_send_scancode(sc, e0, 0);

                dev->kbd_repeat_key = k;
                dev->kbd_repeat_next_tick = now + UHCI_KBD_REPEAT_DELAY_TICKS;
            }
        }
    }

    dev->kbd_prev_mod = mod;
    memcpy(dev->kbd_prev_keys, keys, 6);
}

static void uhci_hid_mouse_process(const uint8_t* rep, uint32_t rep_len) {
    if (rep_len < 3) return;

    uint8_t buttons = rep[0] & 0x07u;
    int16_t dx16 = (int8_t)rep[1];
    int16_t dy16 = (int8_t)rep[2];
    dy16 = (int16_t)(-dy16);
    if (dx16 < -128) dx16 = -128;
    if (dx16 > 127) dx16 = 127;
    if (dy16 < -128) dy16 = -128;
    if (dy16 > 127) dy16 = 127;
    int8_t dx = (int8_t)dx16;
    int8_t dy = (int8_t)dy16;

    uint8_t b0 = 0x08u;
    b0 |= buttons;
    if (dx < 0) b0 |= 0x10u;
    if (dy < 0) b0 |= 0x20u;

    mouse_process_byte(b0);
    mouse_process_byte((uint8_t)dx);
    mouse_process_byte((uint8_t)dy);
}

static int uhci_find_controller(uint8_t* out_bus, uint8_t* out_slot, uint8_t* out_func) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = (uint16_t)(pci_read((uint8_t)bus, slot, func, 0x00u) & 0xFFFFu);
                if (vendor == 0xFFFFu) {
                    continue;
                }

                uint32_t reg = pci_read((uint8_t)bus, slot, func, 0x08u);
                uint8_t class_code = (uint8_t)((reg >> 24) & 0xFFu);
                uint8_t subclass = (uint8_t)((reg >> 16) & 0xFFu);
                uint8_t prog_if = (uint8_t)((reg >> 8) & 0xFFu);

                if (class_code == UHCI_PCI_CLASS_SERIAL_BUS &&
                    subclass == UHCI_PCI_SUBCLASS_USB &&
                    prog_if == UHCI_PCI_PROGIF_UHCI) {
                    *out_bus = (uint8_t)bus;
                    *out_slot = slot;
                    *out_func = func;
                    return 1;
                }
            }
        }
    }

    return 0;
}

static void uhci_route_irq(uint8_t irq_line) {
    if (irq_line >= 16) {
        return;
    }

    irq_install_handler(irq_line, uhci_irq_handler);

    if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
        uint32_t gsi;
        int active_low;
        int level_trigger;

        if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
            gsi = (uint32_t)irq_line;
            active_low = 0;
            level_trigger = 0;
        }

        ioapic_route_gsi(gsi, (uint8_t)(32 + irq_line), (uint8_t)cpus[0].id, active_low, level_trigger);
        return;
    }

    if (irq_line < 8) {
        outb(UHCI_PIC_MASTER_DATA_PORT, (uint8_t)(inb(UHCI_PIC_MASTER_DATA_PORT) & ~(1u << irq_line)));
    } else {
        outb(UHCI_PIC_SLAVE_DATA_PORT, (uint8_t)(inb(UHCI_PIC_SLAVE_DATA_PORT) & ~(1u << (irq_line - 8))));
        outb(UHCI_PIC_MASTER_DATA_PORT, (uint8_t)(inb(UHCI_PIC_MASTER_DATA_PORT) & ~(1u << UHCI_PIC_MASTER_CASCADE_IRQ)));
    }
}

static int uhci_alloc_schedule(void) {
    g_uhci_frame_list = (uint32_t*)kmalloc_a(UHCI_FRAME_LIST_BYTES);
    if (!g_uhci_frame_list) {
        return 0;
    }

    memset(g_uhci_frame_list, 0, UHCI_FRAME_LIST_BYTES);

    g_uhci_frame_list_phys = paging_get_phys(kernel_page_directory, (uint32_t)g_uhci_frame_list);
    if (!g_uhci_frame_list_phys || (g_uhci_frame_list_phys & 0xFFFu) != 0u) {
        return 0;
    }

    g_uhci_async_qh = (uhci_qh_t*)kmalloc_a(sizeof(uhci_qh_t));
    if (!g_uhci_async_qh) {
        return 0;
    }

    memset(g_uhci_async_qh, 0, sizeof(*g_uhci_async_qh));

    g_uhci_async_qh_phys = paging_get_phys(kernel_page_directory, (uint32_t)g_uhci_async_qh);
    if (!g_uhci_async_qh_phys || (g_uhci_async_qh_phys & 0xFu) != 0u) {
        return 0;
    }

    g_uhci_async_qh->link = UHCI_PTR_T;
    g_uhci_async_qh->element = UHCI_PTR_T;
    g_uhci_async_qh->sw_phys = g_uhci_async_qh_phys;

    for (uint32_t i = 0; i < UHCI_FRAME_LIST_ENTRIES; i++) {
        g_uhci_frame_list[i] = (g_uhci_async_qh_phys | UHCI_PTR_QH);
    }

    return 1;
}

static void uhci_reset_controller(void) {
    uhci_writew(UHCI_REG_USBCMD, 0);

    uhci_writew(UHCI_REG_USBCMD, UHCI_USBCMD_GRESET);
    uhci_wait_io(UHCI_RESET_WAIT_IO_LOOPS);
    uhci_writew(UHCI_REG_USBCMD, 0);

    uhci_writew(UHCI_REG_USBCMD, UHCI_USBCMD_HCRESET);

    for (uint32_t i = 0; i < UHCI_RESET_WAIT_IO_LOOPS; i++) {
        if ((uhci_readw(UHCI_REG_USBCMD) & UHCI_USBCMD_HCRESET) == 0) {
            break;
        }
        io_wait();
    }

    uhci_writew(UHCI_REG_USBSTS, UHCI_USBSTS_CLEAR_ALL);
}

int uhci_is_initialized(void) {
    return g_uhci_initialized;
}

void uhci_init(void) {
    if (g_uhci_initialized) {
        return;
    }

    spinlock_init(&g_uhci_sched_lock);

    uint8_t bus, slot, func;
    if (!uhci_find_controller(&bus, &slot, &func)) {
        return;
    }

    uint32_t bar4 = pci_read(bus, slot, func, UHCI_PCI_REG_BAR4);
    uint16_t io_base = (uint16_t)(bar4 & 0xFFFCu);
    if (io_base == 0) {
        return;
    }

    g_uhci_io_base = io_base;

    uint32_t cmd = pci_read(bus, slot, func, UHCI_PCI_REG_COMMAND);
    cmd |= (UHCI_PCI_CMD_IO_SPACE | UHCI_PCI_CMD_BUS_MASTER);
    cmd &= ~UHCI_PCI_CMD_INTX_DISABLE;
    pci_write(bus, slot, func, UHCI_PCI_REG_COMMAND, cmd);

    pci_write(bus, slot, func, UHCI_PCI_REG_LEGSUP, UHCI_PCI_LEGSUP_OS_OWNED);

    g_uhci_irq_line = pci_read8(bus, slot, func, UHCI_PCI_REG_IRQ_LINE);

    uhci_reset_controller();

    if (!uhci_alloc_schedule()) {
        return;
    }

    uhci_writel(UHCI_REG_USBFLBASE, g_uhci_frame_list_phys);
    uhci_writew(UHCI_REG_USBFRNUM, 0);
    uhci_writeb(UHCI_REG_USBSOF, UHCI_USBSOF_DEFAULT);

    uhci_writew(UHCI_REG_USBINTR, 0);
    uhci_writew(UHCI_REG_USBCMD, 0);

    g_uhci_initialized = 1;
}

void uhci_late_init(void) {
    if (!g_uhci_initialized) {
        return;
    }

    g_uhci_can_sleep = 1;

    uhci_writew(UHCI_REG_USBSTS, UHCI_USBSTS_CLEAR_ALL);
    uhci_writew(UHCI_REG_USBINTR, 0);
    uhci_writew(UHCI_REG_USBCMD, (uint16_t)(UHCI_USBCMD_RUN | UHCI_USBCMD_CF | UHCI_USBCMD_MAXP));

    if (g_uhci_irq_line != 0xFFu) {
        uhci_route_irq(g_uhci_irq_line);
    }

    memset(g_hid_devs, 0, sizeof(g_hid_devs));

    uint8_t next_addr = 1;

    for (uint8_t port = 1; port <= 2; port++) {
        if (!uhci_port_is_connected(port)) {
            continue;
        }

        uint8_t low_speed = (uint8_t)uhci_port_is_low_speed(port);

        if (!uhci_port_reset_enable(port)) {
            continue;
        }

        if (next_addr == 0 || next_addr >= 127) {
            break;
        }

        uhci_hid_dev_t* dev = &g_hid_devs[port - 1];
        memset(dev, 0, sizeof(*dev));
        dev->present = 1;
        dev->port = port;
        dev->low_speed = low_speed;

        usb_device_descriptor_t dd;
        memset(&dd, 0, sizeof(dd));
        int r = uhci_usb_get_descriptor(0, low_speed, 8, USB_DESC_DEVICE, 0, &dd, 8);
        if (r < 8) {
            dev->present = 0;
            continue;
        }

        dev->ep0_mps = dd.bMaxPacketSize0 ? dd.bMaxPacketSize0 : 8;

        if (!uhci_usb_set_address(low_speed, dev->ep0_mps, next_addr)) {
            dev->present = 0;
            continue;
        }
        dev->addr = next_addr;
        next_addr++;

        memset(&dd, 0, sizeof(dd));
        uhci_usb_get_descriptor(dev->addr, low_speed, dev->ep0_mps, USB_DESC_DEVICE, 0, &dd, sizeof(dd));

        usb_config_descriptor_t cd;
        memset(&cd, 0, sizeof(cd));
        r = uhci_usb_get_descriptor(dev->addr, low_speed, dev->ep0_mps, USB_DESC_CONFIGURATION, 0, &cd, sizeof(cd));
        if (r < 9) {
            dev->present = 0;
            continue;
        }

        uint16_t total = usb_le16(&cd.wTotalLength);
        if (total < 9) total = 9;
        if (total > 512) total = 512;

        uint8_t* cfg = (uint8_t*)kmalloc_a(total);
        if (!cfg) {
            dev->present = 0;
            continue;
        }
        memset(cfg, 0, total);

        r = uhci_usb_get_descriptor(dev->addr, low_speed, dev->ep0_mps, USB_DESC_CONFIGURATION, 0, cfg, total);
        if (r < 9) {
            kfree(cfg);
            dev->present = 0;
            continue;
        }

        if (r < (int)total) {
            kfree(cfg);
            dev->present = 0;
            continue;
        }

        uint8_t cfg_value = 0;
        uint8_t iface = 0;
        uint8_t proto = 0;
        uint8_t ep_in = 0;
        uint16_t ep_mps = 0;
        uint8_t ep_interval = 0;
        int ok = uhci_hid_parse_cfg(cfg, (uint16_t)r, &cfg_value, &iface, &proto, &ep_in, &ep_mps, &ep_interval);
        kfree(cfg);

        if (!ok) {
            dev->present = 0;
            continue;
        }

        dev->iface_num = iface;
        dev->hid_protocol = proto;
        dev->ep_in = ep_in;
        dev->ep_in_mps = ep_mps;
        dev->ep_interval = ep_interval;

        if (!uhci_usb_set_config(dev->addr, low_speed, dev->ep0_mps, cfg_value)) {
            dev->present = 0;
            continue;
        }

        uhci_hid_set_idle(dev->addr, low_speed, dev->ep0_mps, dev->iface_num);
        uhci_hid_set_protocol(dev->addr, low_speed, dev->ep0_mps, dev->iface_num, 0);

        if (!uhci_hid_setup_interrupt(dev)) {
            dev->present = 0;
            continue;
        }
    }
}

void uhci_poll(void) {
    if (!g_uhci_initialized) {
        return;
    }

    uhci_hid_poll_dev(&g_hid_devs[0]);
    uhci_hid_poll_dev(&g_hid_devs[1]);

    uhci_kbd_repeat_tick(&g_hid_devs[0]);
    uhci_kbd_repeat_tick(&g_hid_devs[1]);
}
