#ifndef DRIVERS_UHCI_H
#define DRIVERS_UHCI_H

#include <stdint.h>

#define UHCI_PTR_T     (1u << 0)
#define UHCI_PTR_QH    (1u << 1)
#define UHCI_PTR_DEPTH (1u << 2)

typedef struct __attribute__((aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t element;
    uint32_t sw_phys;
    uint32_t sw_reserved;
} uhci_qh_t;

typedef struct __attribute__((aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;

    uint32_t sw_next;
    uint32_t sw_phys;
    uint32_t sw_flags;
    uint32_t sw_reserved;
} uhci_td_t;

#define UHCI_TD_CTRL_ACTLEN_MASK 0x7FFu

#define UHCI_TD_CTRL_BITSTUFF  (1u << 17)
#define UHCI_TD_CTRL_CRCTIMEO  (1u << 18)
#define UHCI_TD_CTRL_NAK       (1u << 19)
#define UHCI_TD_CTRL_BABBLE    (1u << 20)
#define UHCI_TD_CTRL_DBUFERR   (1u << 21)
#define UHCI_TD_CTRL_STALLED   (1u << 22)
#define UHCI_TD_CTRL_ACTIVE    (1u << 23)
#define UHCI_TD_CTRL_IOC       (1u << 24)
#define UHCI_TD_CTRL_IOS       (1u << 25)
#define UHCI_TD_CTRL_LS        (1u << 26)
#define UHCI_TD_CTRL_C_ERR_SHIFT 27u
#define UHCI_TD_CTRL_C_ERR_MASK  (3u << UHCI_TD_CTRL_C_ERR_SHIFT)
#define UHCI_TD_CTRL_SPD       (1u << 29)

#define UHCI_TD_PID_OUT   0xE1u
#define UHCI_TD_PID_IN    0x69u
#define UHCI_TD_PID_SETUP 0x2Du

#define UHCI_TD_TOKEN_DEVADDR_SHIFT 8u
#define UHCI_TD_TOKEN_ENDP_SHIFT    15u
#define UHCI_TD_TOKEN_D_SHIFT       19u
#define UHCI_TD_TOKEN_MAXLEN_SHIFT  21u
#define UHCI_TD_TOKEN_MAXLEN_MASK   0x7FFu

#define UHCI_PORTSC_CCS  (1u << 0)
#define UHCI_PORTSC_CSC  (1u << 1)
#define UHCI_PORTSC_PE   (1u << 2)
#define UHCI_PORTSC_PEC  (1u << 3)
#define UHCI_PORTSC_RD   (1u << 6)
#define UHCI_PORTSC_LSDA (1u << 8)
#define UHCI_PORTSC_PR   (1u << 9)

#define UHCI_PORTSC_RWC (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)

void uhci_init(void);
void uhci_late_init(void);
void uhci_poll(void);
int uhci_is_initialized(void);

#endif
