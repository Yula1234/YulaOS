#include <drivers/usb/usb_core.h>
#include <drivers/usb/usb_hcd.h>

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

    void* ctrl_data_buf;
    uint32_t ctrl_data_phys;

    mutex_t ctrl_mutex;

    spinlock_t sync_lock;
    dlist_head_t sync_waits;

    uhci_intr_pipe_t intr_pipes[16];
} uhci_hcd_impl_t;

typedef struct {
    dlist_head_t node;

    semaphore_t sem;

    uhci_qh_t* qh;
    volatile uint8_t done;
} uhci_sync_wait_t;

extern volatile uint32_t timer_ticks;

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

static uint32_t uhci_timeout_deadline_tick(uint32_t timeout_us) {
    if (timeout_us == 0) {
        return 0;
    }

    uint64_t ms64 = ((uint64_t)timeout_us + 999ull) / 1000ull;
    if (ms64 == 0) {
        ms64 = 1;
    }
    if (ms64 > 0xFFFFFFFFull) {
        ms64 = 0xFFFFFFFFull;
    }

    uint64_t t = (uint64_t)timer_ticks + ms64;
    if (t > 0xFFFFFFFFull) {
        t = 0xFFFFFFFFull;
    }

    return (uint32_t)t;
}

static void uhci_sync_wait_init(uhci_sync_wait_t* w, uhci_qh_t* qh) {
    if (!w) {
        return;
    }

    memset(w, 0, sizeof(*w));
    dlist_init(&w->node);
    sem_init(&w->sem, 0);
    w->qh = qh;
    w->done = 0;
}

static int uhci_sync_wait_arm(uhci_hcd_impl_t* u, uhci_sync_wait_t* w) {
    if (!u || !w || !w->qh) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&u->sync_lock);
    dlist_add_tail(&w->node, &u->sync_waits);
    spinlock_release_safe(&u->sync_lock, flags);

    return 1;
}

static void uhci_sync_wait_disarm(uhci_hcd_impl_t* u, uhci_sync_wait_t* w) {
    if (!u || !w) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&u->sync_lock);
    (void)dlist_remove_node_if_present(&u->sync_waits, &w->node);
    spinlock_release_safe(&u->sync_lock, flags);
}

static int uhci_sync_wait_sleep(uhci_sync_wait_t* w, uint32_t timeout_us) {
    if (!w) {
        return 0;
    }

    const uint32_t deadline = uhci_timeout_deadline_tick(timeout_us);
    if (deadline == 0) {
        sem_wait(&w->sem);
        return 1;
    }

    return sem_wait_timeout(&w->sem, deadline) != 0;
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

/*
 * Next USB packet for a scatter-gather buffer: one TD needs one contiguous DMA range.
 * Packet length is min(remaining, max_pkt, bytes left in the current SG segment).
 * Initialize *in_out_si and *in_out_seg_start to 0.
 */
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

/*
 * One UHCI TD is one USB transaction; the buffer must be physically contiguous for that
 * packet. If any max-packet-sized window crosses an SG break, fall back to a linear buffer.
 */
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
    uint32_t data_phys,
    void* ctrl_data_buf
) {
    if (data_sg) {
        dma_unmap_buffer(data_sg);

        return;
    }

    if (length > PAGE_SIZE && data_dma && data_dma != (uint8_t*)ctrl_data_buf) {
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

static int uhci_hcd_control_xfer(
    usb_hcd_t* hcd,
    uint8_t dev_addr,
    usb_speed_t speed,
    uint16_t ep0_mps,
    const usb_setup_packet_t* setup,
    void* data,
    uint16_t length,
    uint32_t timeout_us
) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !setup) {
        return -1;
    }

    if (ep0_mps == 0) {
        ep0_mps = 8;
    }

    if (ep0_mps > 64) {
        ep0_mps = 64;
    }

    mutex_lock(&u->ctrl_mutex);

    uint32_t setup_phys = 0;
    usb_setup_packet_t* setup_dma = (usb_setup_packet_t*)dma_pool_alloc(u->setup_pool, &setup_phys);
    if (!setup_dma || !setup_phys) {
        mutex_unlock(&u->ctrl_mutex);
        return -1;
    }

    memcpy(setup_dma, setup, sizeof(*setup_dma));

    uint8_t* data_dma = 0;
    uint32_t data_phys = 0;
    dma_sg_list_t* data_sg = 0;

    const uint8_t dir_in = (setup->bmRequestType & USB_REQ_TYPE_DIR_IN) ? 1u : 0u;

    if (length) {
        if (length > PAGE_SIZE) {
            if (data) {
                data_sg = dma_map_buffer(data, length, dir_in ? DMA_DIR_FROM_DEVICE : DMA_DIR_TO_DEVICE);

                if (!data_sg) {
                    dma_pool_free(u->setup_pool, setup_dma);
                    mutex_unlock(&u->ctrl_mutex);

                    return -1;
                }

                if (uhci_sg_usb_requires_linear_coherent(data_sg, length, (uint32_t)ep0_mps)) {
                    dma_unmap_buffer(data_sg);
                    data_sg = 0;

                    data_dma = (uint8_t*)dma_alloc_coherent(length, &data_phys);

                    if (!data_dma || !data_phys) {
                        dma_pool_free(u->setup_pool, setup_dma);
                        mutex_unlock(&u->ctrl_mutex);

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
                    mutex_unlock(&u->ctrl_mutex);

                    return -1;
                }

                memset(data_dma, 0, length);
            } else {
                dma_pool_free(u->setup_pool, setup_dma);
                mutex_unlock(&u->ctrl_mutex);

                return -1;
            }
        } else {
            data_dma = (uint8_t*)u->ctrl_data_buf;
            data_phys = u->ctrl_data_phys;

            if (dir_in) {
                memset(data_dma, 0, length);
            } else {
                if (!data) {
                    dma_pool_free(u->setup_pool, setup_dma);
                    mutex_unlock(&u->ctrl_mutex);

                    return -1;
                }

                memcpy(data_dma, data, length);
            }
        }
    }

    uint32_t td_setup_phys = 0;
    uhci_td_t* td_setup = uhci_td_alloc(u, &td_setup_phys);
    if (!td_setup) {
        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

        dma_pool_free(u->setup_pool, setup_dma);
        mutex_unlock(&u->ctrl_mutex);

        return -1;
    }

    td_setup->link = UHCI_PTR_T;
    td_setup->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
    if (speed == USB_SPEED_LOW) {
        td_setup->status |= UHCI_TD_CTRL_LS;
    }

    td_setup->token =
        (uhci_td_maxlen_field(sizeof(*setup_dma)) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
        (0u << UHCI_TD_TOKEN_D_SHIFT) |
        (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
        ((uint32_t)dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
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

                uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

                dma_pool_free(u->setup_pool, setup_dma);
                mutex_unlock(&u->ctrl_mutex);

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

            uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

            dma_pool_free(u->setup_pool, setup_dma);
            mutex_unlock(&u->ctrl_mutex);

            return -1;
        }

        td_prev->link = (td->sw_phys | UHCI_PTR_DEPTH);
        td_prev->sw_next = (uint32_t)(uintptr_t)td;

        td->link = UHCI_PTR_T;
        td->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE;
        if (speed == USB_SPEED_LOW) {
            td->status |= UHCI_TD_CTRL_LS;
        }
        if (dir_in) {
            td->status |= UHCI_TD_CTRL_SPD;
        }

        td->token =
            (uhci_td_maxlen_field(pkt) << UHCI_TD_TOKEN_MAXLEN_SHIFT) |
            ((uint32_t)toggle << UHCI_TD_TOKEN_D_SHIFT) |
            (0u << UHCI_TD_TOKEN_ENDP_SHIFT) |
            ((uint32_t)dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
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

        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

        dma_pool_free(u->setup_pool, setup_dma);
        mutex_unlock(&u->ctrl_mutex);

        return -1;
    }

    td_prev->link = (td_status->sw_phys | UHCI_PTR_DEPTH);
    td_prev->sw_next = (uint32_t)(uintptr_t)td_status;

    td_status->link = UHCI_PTR_T;
    td_status->status = (3u << UHCI_TD_CTRL_C_ERR_SHIFT) | UHCI_TD_CTRL_ACTIVE | UHCI_TD_CTRL_IOC;
    if (speed == USB_SPEED_LOW) {
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
        ((uint32_t)dev_addr << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
        status_pid;

    td_status->buffer = 0;

    uint32_t qh_phys = 0;
    uhci_qh_t* qh = uhci_qh_alloc(u, &qh_phys);
    if (!qh) {
        uhci_td_chain_free_pool(u, td_first);

        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

        dma_pool_free(u->setup_pool, setup_dma);
        mutex_unlock(&u->ctrl_mutex);

        return -1;
    }

    qh->link = UHCI_PTR_T;
    qh->element = td_first->sw_phys;

    uhci_sched_insert_head_qh(u, qh);

    uhci_sync_wait_t wait;
    uhci_sync_wait_init(&wait, qh);
    (void)uhci_sync_wait_arm(u, &wait);

    const int ok = uhci_sync_wait_sleep(&wait, timeout_us);

    uhci_sync_wait_disarm(u, &wait);

    uhci_sched_remove_qh(u, qh);

    uhci_wait_frame_advance(u);

    if (!ok) {
        qh->element = UHCI_PTR_T;
        uhci_qh_free(u, qh);

        uhci_td_chain_free_pool(u, td_first);

        uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

        dma_pool_free(u->setup_pool, setup_dma);

        mutex_unlock(&u->ctrl_mutex);

        return -1;
    }

    int success = 1;

    for (uhci_td_t* td = td_first; td; td = (uhci_td_t*)(uintptr_t)td->sw_next) {
        __sync_synchronize();

        const uint32_t st = td->status;
        if (uhci_td_status_failed(st)) {
            success = 0;
            break;
        }

        if (td == td_status) {
            break;
        }
    }

    if (success && dir_in && data && length && !data_sg) {
        memcpy(data, data_dma, length);
    }

    qh->element = UHCI_PTR_T;
    uhci_qh_free(u, qh);

    uhci_td_chain_free_pool(u, td_first);

    uhci_ctrl_xfer_free_data(data_sg, data_dma, length, data_phys, u->ctrl_data_buf);

    dma_pool_free(u->setup_pool, setup_dma);

    mutex_unlock(&u->ctrl_mutex);

    return success ? (int)length : -1;
}

static int uhci_hcd_bulk_xfer(
    usb_hcd_t* hcd,
    uint8_t dev_addr,
    usb_speed_t speed,
    uint8_t ep_num,
    uint8_t dir_in,
    uint16_t max_packet,
    void* data,
    uint32_t length,
    uint32_t timeout_us,
    uint8_t* toggle_io
) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !dev_addr || ep_num > 15) {
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
        return 0;
    }

    uint8_t toggle = 0;
    if (toggle_io) {
        toggle = *toggle_io & 1u;
    }

    const uint32_t dma_dir = dir_in ? DMA_DIR_FROM_DEVICE : DMA_DIR_TO_DEVICE;

    dma_sg_list_t* data_sg = dma_map_buffer(data, length, dma_dir);

    if (!data_sg) {
        return -1;
    }

    uint8_t* data_dma = 0;
    uint32_t data_phys = 0u;

    if (uhci_sg_usb_requires_linear_coherent(data_sg, length, (uint32_t)max_packet)) {
        dma_unmap_buffer(data_sg);
        data_sg = 0;

        data_dma = (uint8_t*)dma_alloc_coherent(length, &data_phys);

        if (!data_dma || !data_phys) {
            return -1;
        }

        if (dir_in) {
            memset(data_dma, 0, length);
        } else {
            memcpy(data_dma, data, length);
        }
    }

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
                dma_unmap_buffer(data_sg);

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

    if (td_prev) {
        td_prev->status |= UHCI_TD_CTRL_IOC;
    }

    uint32_t qh_phys = 0;
    uhci_qh_t* qh = uhci_qh_alloc(u, &qh_phys);
    if (!qh) {
        uhci_td_chain_free_pool(u, td_first);
        uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);

        return -1;
    }

    qh->link = UHCI_PTR_T;
    qh->element = td_first->sw_phys;

    uhci_sched_insert_head_qh(u, qh);

    uhci_sync_wait_t wait;
    uhci_sync_wait_init(&wait, qh);
    (void)uhci_sync_wait_arm(u, &wait);

    const int ok = uhci_sync_wait_sleep(&wait, timeout_us);

    uhci_sync_wait_disarm(u, &wait);

    uhci_sched_remove_qh(u, qh);

    uhci_wait_frame_advance(u);

    if (!ok) {
        qh->element = UHCI_PTR_T;
        uhci_qh_free(u, qh);

        uhci_td_chain_free_pool(u, td_first);
        uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);

        return -1;
    }

    int success = 1;
    uint32_t total = 0;

    uhci_td_t* td = td_first;

    while (td) {
        __sync_synchronize();

        const uint32_t st = td->status;
        if (uhci_td_status_failed(st)) {
            success = 0;
            break;
        }

        uint32_t al = st & UHCI_TD_CTRL_ACTLEN_MASK;
        uint32_t got = (al == UHCI_TD_CTRL_ACTLEN_MASK) ? 0u : (al + 1u);

        if (got > max_packet) {
            got = max_packet;
        }

        total += got;

        if (got < max_packet) {
            break;
        }

        td = (uhci_td_t*)(uintptr_t)td->sw_next;
    }

    if (total > length) {
        total = length;
    }

    if (success && dir_in && data_dma) {
        memcpy(data, data_dma, total);
    }

    if (toggle_io) {
        *toggle_io = toggle & 1u;
    }

    qh->element = UHCI_PTR_T;

    uhci_qh_free(u, qh);

    uhci_td_chain_free_pool(u, td_first);
    uhci_bulk_xfer_free_data(data_sg, data_dma, length, data_phys);

    return success ? (int)total : -1;
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
        uint32_t flags = spinlock_acquire_safe(&u->sync_lock);

        dlist_head_t* it = u->sync_waits.next;
        while (it != &u->sync_waits) {
            uhci_sync_wait_t* w = container_of(it, uhci_sync_wait_t, node);
            it = it->next;

            if (!w->qh || w->done) {
                continue;
            }

            __sync_synchronize();

            if ((w->qh->element & UHCI_PTR_T) == 0u) {
                continue;
            }

            w->done = 1;
            dlist_del(&w->node);

            sem_signal(&w->sem);
        }

        spinlock_release_safe(&u->sync_lock, flags);
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

static int uhci_hcd_root_port_get_status(usb_hcd_t* hcd, uint8_t port, usb_port_status_t* out) {
    uhci_hcd_impl_t* u = (uhci_hcd_impl_t*)hcd->private_data;
    if (!u || !out || port == 0 || port > 2) {
        return 0;
    }

    const uint16_t st = uhci_port_read(u, port);

    out->connected = (st & UHCI_PORTSC_CCS) ? 1u : 0u;

    if (!out->connected) {
        out->speed = USB_SPEED_FULL;
        return 1;
    }

    out->speed = (st & UHCI_PORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;

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

static const usb_hcd_ops_t g_uhci_hcd_ops = {
    .start = uhci_hcd_start,
    .stop = uhci_hcd_stop,

    .root_port_count = uhci_hcd_root_port_count,
    .root_port_get_status = uhci_hcd_root_port_get_status,
    .root_port_reset = uhci_hcd_root_port_reset,

    .control_xfer = uhci_hcd_control_xfer,
    .bulk_xfer = uhci_hcd_bulk_xfer,

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

    if (u->ctrl_data_buf) {
        dma_free_coherent(u->ctrl_data_buf, PAGE_SIZE, u->ctrl_data_phys);
        u->ctrl_data_buf = 0;
        u->ctrl_data_phys = 0;
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
    dlist_init(&u->sync_waits);

    u->pdev = pdev;

    spinlock_init(&u->sched_lock);
    u->sched_head = 0;

    u->td_pool = dma_pool_create("uhci_td", sizeof(uhci_td_t), 16u);
    u->qh_pool = dma_pool_create("uhci_qh", sizeof(uhci_qh_t), 16u);
    u->setup_pool = dma_pool_create("usb_setup", sizeof(usb_setup_packet_t), 8u);

    u->ctrl_data_buf = dma_alloc_coherent(PAGE_SIZE, &u->ctrl_data_phys);
    mutex_init(&u->ctrl_mutex);

    if (!u->td_pool || !u->qh_pool || !u->setup_pool || !u->ctrl_data_buf || !u->ctrl_data_phys) {
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
