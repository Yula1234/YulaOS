/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/usb/usb_core.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/usb/usb_urb.h>

#include <drivers/driver.h>
#include <drivers/pci/pci.h>

#include <kernel/workqueue.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

#include <hal/delay.h>
#include <hal/pmio.h>

#include <mm/dma/api.h>
#include <mm/heap.h>
#include <mm/pmm.h>

#include <lib/string.h>
#include <lib/dlist.h>

#include "uhci_hw.h"

#define UHCI_RESET_GRESET_DELAY_US     10000u
#define UHCI_RESET_HCRESET_TIMEOUT_US  50000u
#define UHCI_RESET_HCRESET_POLL_US       100u

typedef struct uhci_intr_pipe {
    uint8_t used;

    uint8_t addr;
    usb_speed_t speed;

    uint8_t ep_in;
    uint16_t ep_in_mps;
    uint8_t interval;

    uhci_qh_t* qh;
    uint32_t qh_phys;

    uhci_td_t* td;
    uint32_t td_phys;

    uint8_t* buf;
    uint32_t buf_phys;

    uint8_t toggle;

    usb_intr_cb_t cb;
    void* cb_ctx;
} uhci_intr_pipe_t;

typedef struct {
    usb_hcd_t hcd;

    pci_device_t* pdev;

    __iomem* iomem;

    workqueue_t* wq;
    work_struct_t irq_work;

    spinlock_t sched_lock;

    uint32_t* frame_list;
    uint32_t frame_list_phys;

    uhci_qh_t* async_qh;
    uint32_t async_qh_phys;

    uhci_qh_t* sched_head;

    dma_pool_t* td_pool;
    dma_pool_t* qh_pool;
    dma_pool_t* setup_pool;

    spinlock_t sync_lock;
    dlist_head_t xfer_waits;

    uhci_intr_pipe_t intr_pipes[16];
} uhci_hcd_impl_t;

typedef struct uhci_urb_priv uhci_urb_priv_t;

typedef struct {
    dlist_head_t node;

    uhci_qh_t* qh;
    usb_urb_t* urb;
} uhci_wait_entry_t;

struct uhci_urb_priv {
    uhci_td_t* td_first;
    uhci_td_t* td_status;

    uhci_qh_t* qh;

    usb_setup_packet_t* setup_dma;
    dma_sg_list_t* data_sg;
    uint8_t* data_dma;
    uint32_t data_phys;
    uint16_t length;

    uint8_t is_control;
    uint8_t dir_in;

    uint8_t* toggle_io;
    uint8_t toggle_val;

    void* user_buffer;
    uint16_t ep0_mps;

    uint8_t max_packet;
    uint8_t ep_num;
    uint8_t speed;
    uint8_t dev_addr;

    uint32_t bulk_length;
};

typedef struct {
    uhci_wait_entry_t wait;
    uhci_urb_priv_t priv;
} uhci_urb_bundle_t;

static uhci_td_t* uhci_td_alloc(uhci_hcd_impl_t* u, uint32_t* out_phys);
static void uhci_td_free(uhci_hcd_impl_t* u, uhci_td_t* td);

static uhci_qh_t* uhci_qh_alloc(uhci_hcd_impl_t* u, uint32_t* out_phys);
static void uhci_qh_free(uhci_hcd_impl_t* u, uhci_qh_t* qh);

static inline uint16_t uhci_readw(uhci_hcd_impl_t* u, uint16_t reg) {
    return ioread16(u->iomem, reg);
}

static inline void uhci_writew(uhci_hcd_impl_t* u, uint16_t reg, uint16_t v) {
    iowrite16(u->iomem, reg, v);
}

static inline void uhci_writel(uhci_hcd_impl_t* u, uint16_t reg, uint32_t v) {
    iowrite32(u->iomem, reg, v);
}

static void uhci_wait_frame_advance(uhci_hcd_impl_t* u) {
    if (!u || !u->iomem) {
        proc_usleep(1000);
        return;
    }

    uint16_t start = uhci_readw(u, UHCI_REG_USBFRNUM);
    const uint16_t start_frame = (uint16_t)(start & 0x03FFu);

    for (uint32_t waited = 0; waited < 2000u; waited += 50u) {
        uint16_t cur = uhci_readw(u, UHCI_REG_USBFRNUM);

        const uint16_t cur_frame = (uint16_t)(cur & 0x03FFu);
        if (cur_frame != start_frame) {
            return;
        }

        proc_usleep(50);
    }

    proc_usleep(1000);
}

static uint16_t uhci_port_reg(uint8_t port) {
    return port == 1 ? UHCI_REG_USBPORTSC1 : UHCI_REG_USBPORTSC2;
}

static uint16_t uhci_port_read(uhci_hcd_impl_t* u, uint8_t port) {
    return uhci_readw(u, uhci_port_reg(port));
}

static void uhci_port_write(uhci_hcd_impl_t* u, uint8_t port, uint16_t v) {
    uhci_writew(u, uhci_port_reg(port), v);
}

static void uhci_port_set(uhci_hcd_impl_t* u, uint8_t port, uint16_t bits) {
    uint16_t st = uhci_port_read(u, port);

    st |= bits;
    st &= (uint16_t)~UHCI_PORTSC_RWC;

    uhci_port_write(u, port, st);
}

static void uhci_port_clear(uhci_hcd_impl_t* u, uint8_t port, uint16_t bits) {
    uint16_t st = uhci_port_read(u, port);

    st &= (uint16_t)~UHCI_PORTSC_RWC;
    st &= (uint16_t)~bits;

    st |= (uint16_t)(UHCI_PORTSC_RWC & bits);

    uhci_port_write(u, port, st);
}

static void uhci_reset_controller(uhci_hcd_impl_t* u) {
    uhci_writew(u, UHCI_REG_USBCMD, 0);

    uhci_writew(u, UHCI_REG_USBCMD, UHCI_USBCMD_GRESET);

    udelay(UHCI_RESET_GRESET_DELAY_US);
    
    uhci_writew(u, UHCI_REG_USBCMD, 0);

    uhci_writew(u, UHCI_REG_USBCMD, UHCI_USBCMD_HCRESET);

    for (uint32_t waited = 0; waited < UHCI_RESET_HCRESET_TIMEOUT_US; waited += UHCI_RESET_HCRESET_POLL_US) {
        if ((uhci_readw(u, UHCI_REG_USBCMD) & UHCI_USBCMD_HCRESET) == 0) {
            break;
        }

        udelay(UHCI_RESET_HCRESET_POLL_US);
    }

    uhci_writew(u, UHCI_REG_USBSTS, UHCI_USBSTS_CLEAR_ALL);
}

static int uhci_alloc_schedule(uhci_hcd_impl_t* u) {
    u->frame_list = (uint32_t*)dma_alloc_coherent(UHCI_FRAME_LIST_BYTES, &u->frame_list_phys);
    if (!u->frame_list || !u->frame_list_phys) {
        return 0;
    }

    u->async_qh = 0;
    u->async_qh_phys = 0;

    {
        uint32_t qh_phys = 0;
        u->async_qh = uhci_qh_alloc(u, &qh_phys);
        u->async_qh_phys = qh_phys;
    }

    if (!u->async_qh || !u->async_qh_phys) {
        dma_free_coherent(u->frame_list, UHCI_FRAME_LIST_BYTES, u->frame_list_phys);
        u->frame_list = 0;
        u->frame_list_phys = 0;
        return 0;
    }

    memset(u->async_qh, 0, sizeof(*u->async_qh));

    u->async_qh->link = UHCI_PTR_T;
    u->async_qh->element = UHCI_PTR_T;
    u->async_qh->sw_phys = u->async_qh_phys;

    for (uint32_t i = 0; i < UHCI_FRAME_LIST_ENTRIES; i++) {
        u->frame_list[i] = (u->async_qh_phys | UHCI_PTR_QH);
    }

    __sync_synchronize();

    return 1;
}

static uhci_td_t* uhci_td_alloc(uhci_hcd_impl_t* u, uint32_t* out_phys) {
    if (!u) {
        return 0;
    }

    uint32_t phys = 0;
    uhci_td_t* td = (uhci_td_t*)dma_pool_alloc(u->td_pool, &phys);
    if (!td || !phys) {
        return 0;
    }

    memset(td, 0, sizeof(*td));
    td->sw_phys = phys;

    if (out_phys) {
        *out_phys = phys;
    }

    return td;
}

static void uhci_td_free(uhci_hcd_impl_t* u, uhci_td_t* td) {
    if (!u || !td) {
        return;
    }

    dma_pool_free(u->td_pool, td);
}

static uhci_qh_t* uhci_qh_alloc(uhci_hcd_impl_t* u, uint32_t* out_phys) {
    if (!u) {
        return 0;
    }

    uint32_t phys = 0;
    uhci_qh_t* qh = (uhci_qh_t*)dma_pool_alloc(u->qh_pool, &phys);
    if (!qh || !phys) {
        return 0;
    }

    memset(qh, 0, sizeof(*qh));
    qh->sw_phys = phys;

    if (out_phys) {
        *out_phys = phys;
    }

    return qh;
}

static void uhci_qh_free(uhci_hcd_impl_t* u, uhci_qh_t* qh) {
    if (!u || !qh) {
        return;
    }

    dma_pool_free(u->qh_pool, qh);
}

static void uhci_sched_insert_head_qh(uhci_hcd_impl_t* u, uhci_qh_t* qh) {
    uint32_t flags = spinlock_acquire_safe(&u->sched_lock);

    qh->sw_reserved = (uint32_t)(uintptr_t)u->sched_head;

    if (u->sched_head) {
        qh->link = u->sched_head->sw_phys | UHCI_PTR_QH;
    } else {
        qh->link = UHCI_PTR_T;
    }

    u->sched_head = qh;

    u->async_qh->link = qh->sw_phys | UHCI_PTR_QH;

    __sync_synchronize();

    spinlock_release_safe(&u->sched_lock, flags);
}

static void uhci_sched_remove_qh(uhci_hcd_impl_t* u, uhci_qh_t* qh) {
    uint32_t flags = spinlock_acquire_safe(&u->sched_lock);

    uhci_qh_t* prev = 0;
    uhci_qh_t* cur = u->sched_head;

    while (cur) {
        if (cur == qh) {
            break;
        }

        prev = cur;
        cur = (uhci_qh_t*)(uintptr_t)cur->sw_reserved;
    }

    if (cur) {
        uhci_qh_t* next = (uhci_qh_t*)(uintptr_t)cur->sw_reserved;

        if (!prev) {
            u->sched_head = next;

            if (next) {
                u->async_qh->link = next->sw_phys | UHCI_PTR_QH;
            } else {
                u->async_qh->link = UHCI_PTR_T;
            }
        } else {
            prev->sw_reserved = (uint32_t)(uintptr_t)next;

            if (next) {
                prev->link = next->sw_phys | UHCI_PTR_QH;
            } else {
                prev->link = UHCI_PTR_T;
            }
        }
    }

    __sync_synchronize();

    spinlock_release_safe(&u->sched_lock, flags);
}

static void uhci_td_chain_free_pool(uhci_hcd_impl_t* u, uhci_td_t* td) {
    while (td) {
        uhci_td_t* next = (uhci_td_t*)(uintptr_t)td->sw_next;

        uhci_td_free(u, td);
        td = next;
    }
}

static int uhci_td_status_failed(uint32_t st) {
    return (st & (UHCI_TD_CTRL_STALLED | UHCI_TD_CTRL_DBUFERR | UHCI_TD_CTRL_BABBLE | UHCI_TD_CTRL_CRCTIMEO | UHCI_TD_CTRL_BITSTUFF)) != 0u;
}

static uint32_t uhci_sg_next_packet(
    dma_sg_list_t* sg,
    uint32_t* in_out_si,
    uint32_t* in_out_seg_start,
    uint32_t offset,
    uint32_t max_pkt,
    uint32_t remaining,
    uint32_t* out_phys
) {
    uint32_t si = *in_out_si;
    uint32_t seg_start = *in_out_seg_start;

    while (si < sg->count) {
        dma_sg_elem_t* e = &sg->elems[si];
        const uint32_t seg_end = seg_start + e->length;

        if (offset < seg_end) {
            *in_out_si = si;
            *in_out_seg_start = seg_start;

            const uint32_t in_seg = seg_end - offset;
            uint32_t pkt = remaining;

            if (pkt > max_pkt) {
                pkt = max_pkt;
            }

            if (pkt > in_seg) {
                pkt = in_seg;
            }

            if (pkt == 0u) {
                return 0u;
            }

            *out_phys = (uint32_t)(e->phys_addr + (offset - seg_start));

            return pkt;
        }

        seg_start = seg_end;
        si++;
    }

    return 0u;
}

static uint32_t uhci_sg_contiguous_from_offset(dma_sg_list_t* sg, uint32_t offset) {
    uint32_t pos = 0u;

    for (uint32_t i = 0u; i < sg->count; i++) {
        const dma_sg_elem_t* e = &sg->elems[i];
        const uint32_t seg_end = pos + e->length;

        if (offset < seg_end) {
            return seg_end - offset;
        }

        pos = seg_end;
    }

    return 0u;
}

static int uhci_sg_usb_requires_linear_coherent(dma_sg_list_t* sg, uint32_t length, uint32_t max_pkt) {
    uint32_t off = 0u;

    while (off < length) {
        uint32_t need = length - off;

        if (need > max_pkt) {
            need = max_pkt;
        }

        const uint32_t contig = uhci_sg_contiguous_from_offset(sg, off);

        if (contig < need) {
            return 1;
        }

        off += need;
    }

    return 0;
}

static void uhci_ctrl_xfer_free_data(
    dma_sg_list_t* data_sg,
    uint8_t* data_dma,
    uint32_t length,
    uint32_t data_phys
) {
    if (data_sg) {
        dma_unmap_buffer(data_sg);

        return;
    }

    if (length > 0 && data_dma) {
        dma_free_coherent(data_dma, length, data_phys);
    }
}

static void uhci_bulk_xfer_free_data(dma_sg_list_t* data_sg, void* data_dma, uint32_t length, uint32_t data_phys) {
    if (data_sg) {
        dma_unmap_buffer(data_sg);

        return;
    }

    if (data_dma) {
        dma_free_coherent(data_dma, length, data_phys);
    }
}

static int uhci_xfer_wait_arm(uhci_hcd_impl_t* u, uhci_wait_entry_t* e) {
    if (!u || !e || !e->qh || !e->urb) {
        return 0;
    }

    dlist_init(&e->node);

    uint32_t flags = spinlock_acquire_safe(&u->sync_lock);
    dlist_add_tail(&e->node, &u->xfer_waits);
    spinlock_release_safe(&u->sync_lock, flags);

    return 1;
}

static void uhci_complete_urb(uhci_hcd_impl_t* u, uhci_wait_entry_t* e, int cancelled) {
    usb_urb_t* urb = e ? e->urb : 0;
    if (!u || !urb || !urb->hcpriv) {
        return;
    }

    uhci_urb_bundle_t* bundle = (uhci_urb_bundle_t*)urb->hcpriv;
    uhci_urb_priv_t* p = &bundle->priv;

    if (!cancelled) {
        uhci_sched_remove_qh(u, p->qh);
        uhci_wait_frame_advance(u);
    } else {
        uhci_sched_remove_qh(u, p->qh);
        uhci_wait_frame_advance(u);

        if (p->qh) {
            p->qh->element = UHCI_PTR_T;
        }
    }

    if (cancelled) {
        urb->status = USB_URB_STATUS_CANCELLED;
        urb->actual_length = 0;
    } else if (p->is_control) {
        int success = 1;

        for (uhci_td_t* td = p->td_first; td; td = (uhci_td_t*)(uintptr_t)td->sw_next) {
            __sync_synchronize();

            const uint32_t st = td->status;
            if (uhci_td_status_failed(st)) {
                success = 0;
                break;
            }

            if (td == p->td_status) {
                break;
            }
        }

        if (success && p->dir_in && p->user_buffer && p->length && !p->data_sg) {
            memcpy(p->user_buffer, p->data_dma, p->length);
        }

        urb->status = success ? (int)p->length : USB_URB_STATUS_IOERROR;
        urb->actual_length = success ? (uint32_t)p->length : 0;
    } else {
        int success = 1;
        uint32_t total = 0;

        uhci_td_t* td = p->td_first;

        while (td) {
            __sync_synchronize();

            const uint32_t st = td->status;
            if (uhci_td_status_failed(st)) {
                success = 0;
                break;
            }

            uint32_t al = st & UHCI_TD_CTRL_ACTLEN_MASK;
            uint32_t got = (al == UHCI_TD_CTRL_ACTLEN_MASK) ? 0u : (al + 1u);

            if (got > p->max_packet) {
                got = p->max_packet;
            }

            total += got;

            if (got < p->max_packet) {
                break;
            }

            td = (uhci_td_t*)(uintptr_t)td->sw_next;
        }

        if (total > p->bulk_length) {
            total = p->bulk_length;
        }

        if (success && p->dir_in && p->data_dma) {
            memcpy(p->user_buffer, p->data_dma, total);
        }

        if (p->toggle_io) {
            *p->toggle_io = p->toggle_val & 1u;
        }

        urb->status = success ? (int)total : USB_URB_STATUS_IOERROR;
        urb->actual_length = total;
    }

    if (p->qh) {
        p->qh->element = UHCI_PTR_T;
        uhci_qh_free(u, p->qh);
        p->qh = 0;
    }

    uhci_td_chain_free_pool(u, p->td_first);
    p->td_first = 0;

    if (p->setup_dma) {
        dma_pool_free(u->setup_pool, p->setup_dma);
        p->setup_dma = 0;
    }

    if (p->is_control) {
        uhci_ctrl_xfer_free_data(p->data_sg, p->data_dma, p->length, p->data_phys);
    } else {
        uhci_bulk_xfer_free_data(p->data_sg, p->data_dma, p->bulk_length, p->data_phys);
    }

    p->data_sg = 0;
    p->data_dma = 0;

    usb_urb_complete_fn complete = urb->complete;

    kfree(bundle);
    urb->hcpriv = 0;

    if (complete) {
        complete(urb);
    }
}

static int uhci_submit_control_urb(usb_hcd_t* hcd, usb_urb_t* urb) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !urb || urb->type != USB_URB_CONTROL) {
        return -1;
    }

    const usb_setup_packet_t* setup = &urb->setup;
    void* data = urb->buffer;
    uint16_t length = urb->length;

    uint16_t ep0_mps = urb->ep0_mps;
    if (ep0_mps == 0) {
        ep0_mps = 8;
    }
    if (ep0_mps > 64) {
        ep0_mps = 64;
    }

    uhci_urb_bundle_t* bundle = (uhci_urb_bundle_t*)kzalloc(sizeof(*bundle));
    if (!bundle) {
        return -1;
    }

    uhci_urb_priv_t* pr = &bundle->priv;
    pr->is_control = 1;
    pr->user_buffer = data;
    pr->length = length;
    pr->ep0_mps = ep0_mps;
    pr->dev_addr = urb->dev_addr;
    pr->speed = (uint8_t)urb->speed;

    uint32_t setup_phys = 0;
    usb_setup_packet_t* setup_dma = (usb_setup_packet_t*)dma_pool_alloc(u->setup_pool, &setup_phys);
    if (!setup_dma || !setup_phys) {
        kfree(bundle);
        return -1;
    }

    memcpy(setup_dma, setup, sizeof(*setup_dma));

    uint8_t* data_dma = 0;
    uint32_t data_phys = 0;
    dma_sg_list_t* data_sg = 0;

    const uint8_t dir_in = (setup->bmRequestType & USB_REQ_TYPE_DIR_IN) ? 1u : 0u;
    pr->dir_in = dir_in;

    if (length) {
        if (data) {
            data_sg = dma_map_buffer(data, length, dir_in ? DMA_DIR_FROM_DEVICE : DMA_DIR_TO_DEVICE);

            if (!data_sg) {
                dma_pool_free(u->setup_pool, setup_dma);
                kfree(bundle);
                return -1;
            }

            if (uhci_sg_usb_requires_linear_coherent(data_sg, length, (uint32_t)ep0_mps)) {
                dma_unmap_buffer(data_sg);
                data_sg = 0;

                data_dma = (uint8_t*)dma_alloc_coherent(length, &data_phys);

                if (!data_dma || !data_phys) {
                    dma_pool_free(u->setup_pool, setup_dma);
                    kfree(bundle);
                    return -1;
                }

                if (dir_in) {
                    memset(data_dma, 0, length);
                } else {
                    memcpy(data_dma, data, length);
                }
            }
        } else if (dir_in) {
            data_dma = (uint8_t*)dma_alloc_coherent(length, &data_phys);

            if (!data_dma || !data_phys) {
                dma_pool_free(u->setup_pool, setup_dma);
                kfree(bundle);
                return -1;
            }

            memset(data_dma, 0, length);
        } else {
            dma_pool_free(u->setup_pool, setup_dma);
            kfree(bundle);
            return -1;
        }
    }

    pr->data_sg = data_sg;
    pr->data_dma = data_dma;
    pr->data_phys = data_phys;

    uint32_t td_setup_phys = 0;
    uhci_td_t* td_setup = uhci_td_alloc(u, &td_setup_phys);
    if (!td_setup) {
        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
        dma_pool_free(u->setup_pool, setup_dma);
        kfree(bundle);
        return -1;
    }

    td_setup->link = UHCI_PTR_T;
    td_setup->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
    if (urb->speed == USB_SPEED_LOW) {
        td_setup->status |= UHCI_TD_CTRL_LS;
    }

    td_setup->token =
        (uhci_td_maxlen_field(sizeof(*setup_dma)) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        (0u << UHCI_TD_TOKEN_D_SHIFT) |
        (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)urb->dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        UHCI_TD_PID_SETUP;

    td_setup->buffer = setup_phys;

    uhci_td_t* td_first = td_setup;
    uhci_td_t* td_prev = td_setup;

    uint8_t toggle = 1u;

    uint32_t remaining = length;
    uint32_t offset = 0;

    uint32_t sg_si = 0u;
    uint32_t sg_seg_start = 0u;

    while (remaining) {
        uint32_t buf_phys = 0u;
        uint32_t pkt32 = 0u;

        if (data_sg) {
            pkt32 = uhci_sg_next_packet(
                data_sg,
                &sg_si,
                &sg_seg_start,
                offset,
                (uint32_t)ep0_mps,
                remaining,
                &buf_phys
            );

            if (pkt32 == 0u || buf_phys == 0u) {
                uhci_td_chain_free_pool(u, td_first);
                uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
                dma_pool_free(u->setup_pool, setup_dma);
                kfree(bundle);
                return -1;
            }
        } else {
            pkt32 = remaining > ep0_mps ? (uint32_t)ep0_mps : remaining;
            buf_phys = data_phys + offset;
        }

        const uint16_t pkt = (uint16_t)pkt32;

        uint32_t td_phys = 0;
        uhci_td_t* td = uhci_td_alloc(u, &td_phys);
        if (!td) {
            uhci_td_chain_free_pool(u, td_first);
            uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
            dma_pool_free(u->setup_pool, setup_dma);
            kfree(bundle);
            return -1;
        }

        td_prev->link = (td->sw_phys | UHCI_PTR_DEPTH);
        td_prev->sw_next = (uint32_t)(uintptr_t)td;

        td->link = UHCI_PTR_T;
        td->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
        if (urb->speed == USB_SPEED_LOW) {
            td->status |= UHCI_TD_CTRL_LS;
        }
        if (dir_in) {
            td->status |= UHCI_TD_CTRL_SPD;
        }

        td->token =
            (uhci_td_maxlen_field(pkt) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
            ((uint32_t)toggle << UHCI_TD_TOKEN_D_SHIFT) |
            (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
            ((uint32_t)urb->dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
            (dir_in ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT);

        td->buffer = buf_phys;

        td_prev = td;
        toggle ^= 1u;

        remaining -= pkt32;
        offset += pkt32;
    }

    uint32_t td_status_phys = 0;
    uhci_td_t* td_status = uhci_td_alloc(u, &td_status_phys);
    if (!td_status) {
        uhci_td_chain_free_pool(u, td_first);
        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
        dma_pool_free(u->setup_pool, setup_dma);
        kfree(bundle);
        return -1;
    }

    td_prev->link = (td_status->sw_phys | UHCI_PTR_DEPTH);
    td_prev->sw_next = (uint32_t)(uintptr_t)td_status;

    td_status->link = UHCI_PTR_T;
    td_status->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_IOC;
    if (urb->speed == USB_SPEED_LOW) {
        td_status->status |= UHCI_TD_CTRL_LS;
    }

    uint8_t status_pid = UHCI_TD_PID_IN;
    if (length) {
        status_pid = dir_in ? UHCI_TD_PID_OUT : UHCI_TD_PID_IN;
    }

    td_status->token =
        (uhci_td_maxlen_field(0) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        (1u << UHCI_TD_TOKEN_D_SHIFT) |
        (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)urb->dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        status_pid;

    td_status->buffer = 0;

    uint32_t qh_phys = 0;
    uhci_qh_t* qh = uhci_qh_alloc(u, &qh_phys);
    if (!qh) {
        uhci_td_chain_free_pool(u, td_first);
        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
        dma_pool_free(u->setup_pool, setup_dma);
        kfree(bundle);
        return -1;
    }

    qh->link = UHCI_PTR_T;
    qh->element = td_first->sw_phys;

    pr->td_first = td_first;
    pr->td_status = td_status;
    pr->qh = qh;
    pr->setup_dma = setup_dma;

    urb->hcpriv = bundle;

    bundle->wait.qh = qh;
    bundle->wait.urb = urb;

    uhci_sched_insert_head_qh(u, qh);

    if (!uhci_xfer_wait_arm(u, &bundle->wait)) {
        qh->element = UHCI_PTR_T;
        uhci_qh_free(u, qh);
        uhci_td_chain_free_pool(u, td_first);
        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys);
        dma_pool_free(u->setup_pool, setup_dma);
        urb->hcpriv = 0;
        kfree(bundle);
        return -1;
    }

    return 0;
}

static int uhci_submit_bulk_urb(usb_hcd_t* hcd, usb_urb_t* urb) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !urb || urb->type != USB_URB_BULK) {
        return -1;
    }

    uint8_t dev_addr = urb->dev_addr;
    usb_speed_t speed = urb->speed;
    uint8_t ep_num = urb->ep_num;
    uint8_t dir_in = urb->dir_in;
    uint16_t max_packet = urb->max_packet;
    void* data = urb->buffer;
    uint32_t length = urb->transfer_buffer_length;
    uint8_t* toggle_io = urb->toggle_io;

    if (!dev_addr || ep_num > 15) {
        return -1;
    }

    if (length && !data) {
        return -1;
    }

    if (max_packet == 0) {
        max_packet = 8;
    }

    if (max_packet > 64) {
        max_packet = 64;
    }

    if (length == 0) {
        urb->status = 0;
        urb->actual_length = 0;
        if (urb->complete) {
            urb->complete(urb);
        }
        return 0;
    }

    uhci_urb_bundle_t* bundle = (uhci_urb_bundle_t*)kzalloc(sizeof(*bundle));
    if (!bundle) {
        return -1;
    }

    uhci_urb_priv_t* pr = &bundle->priv;
    pr->is_control = 0;
    pr->user_buffer = data;
    pr->bulk_length = length;
    pr->max_packet = (uint8_t)max_packet;
    pr->ep_num = ep_num;
    pr->dev_addr = dev_addr;
    pr->speed = (uint8_t)speed;
    pr->toggle_io = toggle_io;

    uint8_t toggle = 0;
    if (toggle_io) {
        toggle = *toggle_io & 1u;
    }

    const uint32_t dma_dir = dir_in ? DMA_DIR_FROM_DEVICE : DMA_DIR_TO_DEVICE;

    dma_sg_list_t* data_sg = dma_map_buffer(data, length, dma_dir);

    if (!data_sg) {
        kfree(bundle);
        return -1;
    }

    uint8_t* data_dma = 0;
    uint32_t data_phys = 0u;

    if (uhci_sg_usb_requires_linear_coherent(data_sg, length, (uint32_t)max_packet)) {
        dma_unmap_buffer(data_sg);
        data_sg = 0;

        data_dma = (uint8_t*)dma_alloc_coherent(length, &data_phys);

        if (!data_dma || !data_phys) {
            kfree(bundle);
            return -1;
        }

        if (dir_in) {
            memset(data_dma, 0, length);
        } else {
            memcpy(data_dma, data, length);
        }
    }

    pr->data_sg = data_sg;
    pr->data_dma = data_dma;
    pr->data_phys = data_phys;
    pr->dir_in = dir_in;

    uhci_td_t* td_first = 0;
    uhci_td_t* td_prev = 0;

    uint32_t remaining = length;
    uint32_t offset = 0;

    uint32_t sg_si = 0u;
    uint32_t sg_seg_start = 0u;

    while (remaining) {
        uint32_t buf_phys = 0u;
        uint32_t pkt32 = 0u;

        if (data_sg) {
            pkt32 = uhci_sg_next_packet(
                data_sg,
                &sg_si,
                &sg_seg_start,
                offset,
                (uint32_t)max_packet,
                remaining,
                &buf_phys
            );

            if (pkt32 == 0u || buf_phys == 0u) {
                uhci_td_chain_free_pool(u, td_first);
                uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);
                kfree(bundle);
                return -1;
            }
        } else {
            pkt32 = remaining > max_packet ? (uint32_t)max_packet : remaining;
            buf_phys = data_phys + offset;
        }

        const uint16_t pkt = (uint16_t)pkt32;

        uint32_t td_phys = 0;
        uhci_td_t* td = uhci_td_alloc(u, &td_phys);
        if (!td) {
            uhci_td_chain_free_pool(u, td_first);
            uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);
            kfree(bundle);
            return -1;
        }

        td->link = UHCI_PTR_T;

        uint32_t status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
        if (speed == USB_SPEED_LOW) {
            status |= UHCI_TD_CTRL_LS;
        }
        if (dir_in) {
            status |= UHCI_TD_CTRL_SPD;
        }

        td->status = status;

        td->token =
            (uhci_td_maxlen_field(pkt) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
            ((uint32_t)toggle << UHCI_TD_TOKEN_D_SHIFT) |
            ((uint32_t)ep_num << UHCI_TD_TOKEN_ENDP_SHIFT) |
            ((uint32_t)dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
            (dir_in ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT);

        td->buffer = buf_phys;

        if (!td_first) {
            td_first = td;
        }

        if (td_prev) {
            td_prev->link = td->sw_phys;
            td_prev->sw_next = (uint32_t)(uintptr_t)td;
        }

        td_prev = td;

        toggle ^= 1u;

        remaining -= pkt32;
        offset += pkt32;
    }

    pr->toggle_val = toggle;

    if (td_prev) {
        td_prev->status |= UHCI_TD_CTRL_IOC;
    }

    uint32_t qh_phys = 0;
    uhci_qh_t* qh = uhci_qh_alloc(u, &qh_phys);
    if (!qh) {
        uhci_td_chain_free_pool(u, td_first);
        uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);
        kfree(bundle);
        return -1;
    }

    qh->link = UHCI_PTR_T;
    qh->element = td_first->sw_phys;

    pr->td_first = td_first;
    pr->qh = qh;

    urb->hcpriv = bundle;

    bundle->wait.qh = qh;
    bundle->wait.urb = urb;

    uhci_sched_insert_head_qh(u, qh);

    if (!uhci_xfer_wait_arm(u, &bundle->wait)) {
        qh->element = UHCI_PTR_T;
        uhci_qh_free(u, qh);
        uhci_td_chain_free_pool(u, td_first);
        uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);
        urb->hcpriv = 0;
        kfree(bundle);
        return -1;
    }

    return 0;
}

static int uhci_hcd_submit_urb(usb_hcd_t* hcd, usb_urb_t* urb) {
    if (!hcd || !urb) {
        return -1;
    }

    switch (urb->type) {
    case USB_URB_CONTROL:
        return uhci_submit_control_urb(hcd, urb);

    case USB_URB_BULK:
        return uhci_submit_bulk_urb(hcd, urb);

    case USB_URB_ISOCH:
        (void)hcd;
        return -1;

    default:
        return -1;
    }
}

static int uhci_hcd_cancel_urb(usb_hcd_t* hcd, usb_urb_t* urb) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !urb || !urb->hcpriv) {
        return -1;
    }

    uhci_wait_entry_t* found = 0;

    uint32_t flags = spinlock_acquire_safe(&u->sync_lock);

    dlist_head_t* it = u->xfer_waits.next;
    while (it != &u->xfer_waits) {
        uhci_wait_entry_t* e = container_of(it, uhci_wait_entry_t, node);
        it = it->next;

        if (e->urb == urb) {
            dlist_del(&e->node);
            found = e;
            break;
        }
    }

    spinlock_release_safe(&u->sync_lock, flags);

    if (!found) {
        return -1;
    }

    uhci_complete_urb(u, found, 1);

    return 0;
}

static void uhci_intr_arm(uhci_intr_pipe_t* p) {
    if (!p || !p->td || !p->qh) {
        return;
    }

    p->td->link = UHCI_PTR_T;

    uint32_t st = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_SPD | UHCI_TD_CTRL_IOC;
    if (p->speed == USB_SPEED_LOW) {
        st |= UHCI_TD_CTRL_LS;
    }

    p->td->status = st;

    p->td->token =
        (uhci_td_maxlen_field(p->ep_in_mps) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        ((uint32_t)p->toggle << UHCI_TD_TOKEN_D_SHIFT) |
        ((uint32_t)p->ep_in << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)p->addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        UHCI_TD_PID_IN;

    p->td->buffer = p->buf_phys;

    p->qh->element = p->td->sw_phys;

    __sync_synchronize();
}

static uhci_intr_pipe_t* uhci_intr_pipe_alloc(uhci_hcd_impl_t* u) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(u->intr_pipes) / sizeof(u->intr_pipes[0])); i++) {
        if (!u->intr_pipes[i].used) {
            memset(&u->intr_pipes[i], 0, sizeof(u->intr_pipes[i]));
            u->intr_pipes[i].used = 1;
            return &u->intr_pipes[i];
        }
    }

    return 0;
}

static void uhci_intr_pipe_free(uhci_hcd_impl_t* u, uhci_intr_pipe_t* p) {
    (void)u;

    if (!p || !p->used) {
        return;
    }

    if (p->qh) {
        uhci_sched_remove_qh(u, p->qh);

        uhci_wait_frame_advance(u);

        p->qh->element = UHCI_PTR_T;

        uhci_qh_free(u, p->qh);
    }

    if (p->td) {
        uhci_td_free(u, p->td);
    }

    if (p->buf) {
        dma_free_coherent(p->buf, p->ep_in_mps, p->buf_phys);
    }

    memset(p, 0, sizeof(*p));
}

static usb_intr_pipe_t* uhci_hcd_intr_open(
    usb_hcd_t* hcd,
    uint8_t dev_addr,
    usb_speed_t speed,
    uint8_t ep_num,
    uint16_t max_packet,
    uint8_t interval,
    usb_intr_cb_t cb,
    void* cb_ctx
) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !dev_addr || !ep_num || !cb) {
        return 0;
    }

    if (max_packet == 0) {
        max_packet = 8;
    }

    if (max_packet > 64) {
        max_packet = 64;
    }

    uhci_intr_pipe_t* p = uhci_intr_pipe_alloc(u);
    if (!p) {
        return 0;
    }

    p->addr = dev_addr;
    p->speed = speed;

    p->ep_in = ep_num;
    p->ep_in_mps = max_packet;

    p->interval = interval ? interval : 10;

    p->cb = cb;
    p->cb_ctx = cb_ctx;

    p->toggle = 0;

    p->buf = (uint8_t*)dma_alloc_coherent(p->ep_in_mps, &p->buf_phys);
    if (!p->buf || !p->buf_phys) {
        uhci_intr_pipe_free(u, p);
        return 0;
    }

    memset(p->buf, 0, p->ep_in_mps);

    p->td = uhci_td_alloc(u, &p->td_phys);
    p->qh = uhci_qh_alloc(u, &p->qh_phys);

    if (!p->td || !p->qh) {
        uhci_intr_pipe_free(u, p);
        return 0;
    }

    p->qh->link = UHCI_PTR_T;
    p->qh->element = UHCI_PTR_T;

    uhci_intr_arm(p);

    uhci_sched_insert_head_qh(u, p->qh);

    return (usb_intr_pipe_t*)p;
}

static void uhci_hcd_intr_close(usb_hcd_t* hcd, usb_intr_pipe_t* pipe) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !pipe) {
        return;
    }

    uhci_intr_pipe_t* p = (uhci_intr_pipe_t*)pipe;
    uhci_intr_pipe_free(u, p);
}

static void uhci_intr_poll_one(uhci_hcd_impl_t* u, uhci_intr_pipe_t* p) {
    (void)u;

    if (!p || !p->used || !p->td || !p->qh) {
        return;
    }

    __sync_synchronize();

    const uint32_t st = p->td->status;
    if (st & UHCI_TD_CTRL_ACTIVE) {
        return;
    }

    if (!uhci_td_status_failed(st)) {
        uint32_t al = st & UHCI_TD_CTRL_ACTLEN_MASK;
        uint32_t got = (al == UHCI_TD_CTRL_ACTLEN_MASK) ? 0u : (al + 1u);

        if (got > p->ep_in_mps) {
            got = p->ep_in_mps;
        }

        if (got) {
            p->cb(p->cb_ctx, p->buf, got);
            p->toggle ^= 1u;
        }
    }

    uhci_intr_arm(p);
}

static void uhci_irq_bh(work_struct_t* work) {
    uhci_hcd_impl_t* u = container_of(work, uhci_hcd_impl_t, irq_work);

    if (!u || !u->iomem) {
        return;
    }

    for (uint32_t i = 0; i < (uint32_t)(sizeof(u->intr_pipes) / sizeof(u->intr_pipes[0])); i++) {
        if (u->intr_pipes[i].used) {
            uhci_intr_poll_one(u, &u->intr_pipes[i]);
        }
    }

    {
        dlist_head_t done;
        dlist_init(&done);

        uint32_t flags = spinlock_acquire_safe(&u->sync_lock);

        dlist_head_t* it = u->xfer_waits.next;
        while (it != &u->xfer_waits) {
            uhci_wait_entry_t* e = container_of(it, uhci_wait_entry_t, node);
            it = it->next;

            if (!e->qh || !e->urb) {
                continue;
            }

            __sync_synchronize();

            if ((e->qh->element & UHCI_PTR_T) == 0u) {
                continue;
            }

            dlist_del(&e->node);
            dlist_add_tail(&e->node, &done);
        }

        spinlock_release_safe(&u->sync_lock, flags);

        while (!dlist_empty(&done)) {
            uhci_wait_entry_t* e = container_of(done.next, uhci_wait_entry_t, node);

            dlist_del(&e->node);
            uhci_complete_urb(u, e, 0);
        }
    }

    uhci_writew(u, UHCI_REG_USBSTS, UHCI_USBSTS_CLEAR_ALL);
}

static void uhci_irq_handler(registers_t* regs, void* ctx) {
    (void)regs;

    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)ctx;
    if (!u || !u->iomem) {
        return;
    }

    const uint16_t st = uhci_readw(u, UHCI_REG_USBSTS);
    if (!st) {
        return;
    }

    uhci_writew(u, UHCI_REG_USBSTS, st);

    queue_work(u->wq, &u->irq_work);
}

static int uhci_hcd_start(usb_hcd_t* hcd) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !u->iomem) {
        return 0;
    }

    uhci_writew(u, UHCI_REG_USBSTS, UHCI_USBSTS_CLEAR_ALL);
    uhci_writew(u, UHCI_REG_USBINTR, UHCI_USBINTR_IOC);

    uhci_writel(u, UHCI_REG_USBFLBASE, u->frame_list_phys);
    uhci_writew(u, UHCI_REG_USBFRNUM, 0);

    iowrite8(u->iomem, UHCI_REG_USBSOF, 0x40u);

    uhci_writew(u, UHCI_REG_USBCMD, (uint16_t)(UHCI_USBCMD_RUN | UHCI_USBCMD_CF | UHCI_USBCMD_MAXP));

    return 1;
}

static void uhci_hcd_stop(usb_hcd_t* hcd) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !u->iomem) {
        return;
    }

    uhci_writew(u, UHCI_REG_USBINTR, 0);
    uhci_writew(u, UHCI_REG_USBCMD, 0);
}

static uint8_t uhci_hcd_root_port_count(usb_hcd_t* hcd) {
    (void)hcd;
    return 2;
}

static void* uhci_hcd_alloc_buffer(usb_hcd_t* hcd, size_t size, uint32_t* out_phys) {
    (void)hcd;

    if (!size || !out_phys) {
        return 0;
    }

    return dma_alloc_coherent(size, out_phys);
}

static void uhci_hcd_free_buffer(usb_hcd_t* hcd, void* vaddr, size_t size, uint32_t phys) {
    (void)hcd;

    if (!vaddr || !size) {
        return;
    }

    dma_free_coherent(vaddr, size, phys);
}

static int uhci_hcd_root_port_get_status(usb_hcd_t* hcd, uint8_t port, usb_port_status_t* out) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !out || port == 0 || port > 2) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    const uint16_t st = uhci_port_read(u, port);

    out->connected = (st & UHCI_PORTSC_CCS) ? 1u : 0u;
    out->port_protocol = USB_PORT_PROTO_USB_1_1;
    out->link_state = USB_PORT_LINK_UNKNOWN;

    if (!out->connected) {
        out->speed = USB_SPEED_FULL;
        return 1;
    }

    out->speed = (st & UHCI_PORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;

    if (st & UHCI_PORTSC_PE) {
        out->flags |= USB_PORT_FLAG_POWERED;
    }

    return 1;
}

static int uhci_hcd_root_port_reset(usb_hcd_t* hcd, uint8_t port) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || port == 0 || port > 2) {
        return 0;
    }

    uhci_port_clear(u, port, UHCI_PORTSC_RWC);

    uhci_port_set(u, port, UHCI_PORTSC_PR);
    proc_usleep(50000);
    uhci_port_clear(u, port, UHCI_PORTSC_PR);
    proc_usleep(10000);

    uhci_port_set(u, port, UHCI_PORTSC_PE);
    proc_usleep(10000);

    uhci_port_clear(u, port, UHCI_PORTSC_RWC);

    const uint16_t st = uhci_port_read(u, port);

    return ((st & UHCI_PORTSC_CCS) != 0u) && ((st & UHCI_PORTSC_PE) != 0u);
}

static int uhci_hcd_device_address(usb_hcd_t* hcd, struct usb_device* dev) {
    (void)hcd;
    (void)dev;

    return 0;
}

static void uhci_hcd_device_unplug(usb_hcd_t* hcd, uint8_t dev_addr) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || dev_addr == 0u) {
        return;
    }

    dlist_head_t cancel;
    dlist_init(&cancel);

    uint32_t flags = spinlock_acquire_safe(&u->sync_lock);

    dlist_head_t* it = u->xfer_waits.next;
    while (it != &u->xfer_waits) {
        uhci_wait_entry_t* e = container_of(it, uhci_wait_entry_t, node);
        it = it->next;

        if (!e->urb || e->urb->dev_addr != dev_addr) {
            continue;
        }

        dlist_del(&e->node);
        dlist_add_tail(&e->node, &cancel);
    }

    spinlock_release_safe(&u->sync_lock, flags);

    while (!dlist_empty(&cancel)) {
        uhci_wait_entry_t* e = container_of(cancel.next, uhci_wait_entry_t, node);

        dlist_del(&e->node);
        uhci_complete_urb(u, e, 1);
    }

    for (uint32_t i = 0u; i < (uint32_t)(sizeof(u->intr_pipes) / sizeof(u->intr_pipes[0])); i++) {
        if (u->intr_pipes[i].used && u->intr_pipes[i].addr == dev_addr) {
            uhci_intr_pipe_free(u, &u->intr_pipes[i]);
        }
    }
}

static void uhci_hcd_endpoint_reset(usb_hcd_t* hcd, uint8_t dev_addr, uint8_t ep_addr) {
    (void)hcd;
    (void)dev_addr;
    (void)ep_addr;
}

static const usb_hcd_ops_t g_uhci_hcd_ops = {
    .start = uhci_hcd_start,
    .stop = uhci_hcd_stop,

    .root_port_count = uhci_hcd_root_port_count,
    .root_port_get_status = uhci_hcd_root_port_get_status,
    .root_port_reset = uhci_hcd_root_port_reset,

    .submit_urb = uhci_hcd_submit_urb,
    .cancel_urb = uhci_hcd_cancel_urb,

    .device_address = uhci_hcd_device_address,
    .device_unplug = uhci_hcd_device_unplug,
    .endpoint_reset = uhci_hcd_endpoint_reset,

    .alloc_buffer = uhci_hcd_alloc_buffer,
    .free_buffer = uhci_hcd_free_buffer,

    .intr_open = uhci_hcd_intr_open,
    .intr_close = uhci_hcd_intr_close,
};

static void uhci_destroy(uhci_hcd_impl_t* u) {
    if (!u) {
        return;
    }

    if (u->hcd.ops) {
        u->hcd.ops->stop(&u->hcd);
    }

    for (uint32_t i = 0; i < (uint32_t)(sizeof(u->intr_pipes) / sizeof(u->intr_pipes[0])); i++) {
        if (u->intr_pipes[i].used) {
            uhci_intr_pipe_free(u, &u->intr_pipes[i]);
        }
    }

    if (u->wq) {
        destroy_workqueue(u->wq);
        u->wq = 0;
    }

    if (u->async_qh) {
        uhci_qh_free(u, u->async_qh);
        u->async_qh = 0;
        u->async_qh_phys = 0;
    }

    if (u->frame_list) {
        dma_free_coherent(u->frame_list, UHCI_FRAME_LIST_BYTES, u->frame_list_phys);
        u->frame_list = 0;
        u->frame_list_phys = 0;
    }

    if (u->setup_pool) {
        dma_pool_destroy(u->setup_pool);
        u->setup_pool = 0;
    }

    if (u->td_pool) {
        dma_pool_destroy(u->td_pool);
        u->td_pool = 0;
    }

    if (u->qh_pool) {
        dma_pool_destroy(u->qh_pool);
        u->qh_pool = 0;
    }

    if (u->iomem) {
        iomem_free(u->iomem);
        u->iomem = 0;
    }

    kfree(u);
}

static int uhci_probe(pci_device_t* pdev) {
    if (!pdev) {
        return -1;
    }

    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)kzalloc(sizeof(*u));
    if (!u) {
        return -1;
    }

    spinlock_init(&u->sync_lock);
    dlist_init(&u->xfer_waits);

    u->pdev = pdev;

    spinlock_init(&u->sched_lock);
    u->sched_head = 0;

    u->td_pool = dma_pool_create("uhci_td", sizeof(uhci_td_t), 16u);
    u->qh_pool = dma_pool_create("uhci_qh", sizeof(uhci_qh_t), 16u);
    u->setup_pool = dma_pool_create("usb_setup", sizeof(usb_setup_packet_t), 8u);

    if (!u->td_pool || !u->qh_pool || !u->setup_pool) {
        uhci_destroy(u);
        return -1;
    }

    u->wq = create_workqueue("uhci");
    if (!u->wq) {
        uhci_destroy(u);
        return -1;
    }

    init_work(&u->irq_work, uhci_irq_bh);

    pci_dev_enable_busmaster(pdev);

    u->iomem = pci_request_bar(pdev, 4u, "uhci");
    if (!u->iomem) {
        uhci_destroy(u);
        return -1;
    }

    uhci_reset_controller(u);

    if (!uhci_alloc_schedule(u)) {
        uhci_destroy(u);
        return -1;
    }

    if (!pci_request_irq(pdev, uhci_irq_handler, u)) {
        uhci_destroy(u);
        return -1;
    }

    u->hcd.name = "uhci";
    u->hcd.ops = &g_uhci_hcd_ops;
    u->hcd.private_data = u;

    if (!usb_register_hcd(&u->hcd)) {
        uhci_destroy(u);
        return -1;
    }

    return 0;
}

static void uhci_remove(pci_device_t* pdev) {
    (void)pdev;
}

static const pci_device_id_t g_uhci_ids[] = {
    {
        .match_flags = PCI_MATCH_CLASS | PCI_MATCH_SUBCLASS | PCI_MATCH_PROG_IF,
        .vendor_id = 0,
        .device_id = 0,
        .class_code = 0x0Cu,
        .subclass = 0x03u,
        .prog_if = 0x00u,
    },
    { .match_flags = 0u },
};

static pci_driver_t g_uhci_pci_driver = {
    .base = {
        .name = "uhci",
        .klass = DRIVER_CLASS_PSEUDO,
        .stage = DRIVER_STAGE_CORE,
        .init = 0,
        .shutdown = 0,
    },
    .id_table = g_uhci_ids,
    .probe = uhci_probe,
    .remove = uhci_remove,
};

static int uhci_driver_init(void) {
    return pci_register_driver(&g_uhci_pci_driver);
}

DRIVER_REGISTER(
    .name = "uhci",
    .klass = DRIVER_CLASS_PSEUDO,
    .stage = DRIVER_STAGE_CORE,
    .init = uhci_driver_init,
    .shutdown = 0
);
