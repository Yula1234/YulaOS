// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/lock.h>
#include <hal/pmio.h>
#include <hal/io.h>

#include <kernel/proc.h>

#include <lib/string.h>

#include <yos/ioctl.h>

#include <fs/vfs.h>

#include "ne2k.h"

#define NE2K_REG_CR     0x00u
#define NE2K_REG_PSTART 0x01u
#define NE2K_REG_PSTOP  0x02u
#define NE2K_REG_BNRY   0x03u
#define NE2K_REG_TPSR   0x04u
#define NE2K_REG_TBCR0  0x05u
#define NE2K_REG_TBCR1  0x06u
#define NE2K_REG_ISR    0x07u
#define NE2K_REG_RSAR0  0x08u
#define NE2K_REG_RSAR1  0x09u
#define NE2K_REG_RBCR0  0x0Au
#define NE2K_REG_RBCR1  0x0Bu
#define NE2K_REG_RCR    0x0Cu
#define NE2K_REG_TCR    0x0Du
#define NE2K_REG_DCR    0x0Eu
#define NE2K_REG_IMR    0x0Fu
#define NE2K_REG_DATA   0x10u
#define NE2K_REG_RESET  0x1Fu

#define NE2K_REG_PAR0   0x01u
#define NE2K_REG_CURR   0x07u

#define NE2K_CR_STP     0x01u
#define NE2K_CR_STA     0x02u
#define NE2K_CR_TXP     0x04u
#define NE2K_CR_RD0     0x08u
#define NE2K_CR_RD1     0x10u
#define NE2K_CR_RD2     0x20u
#define NE2K_CR_PAGE0   0x00u
#define NE2K_CR_PAGE1   0x40u

#define NE2K_ISR_RST    0x80u
#define NE2K_ISR_RDC    0x40u

#define NE2K_DCR_WTS    0x01u
#define NE2K_DCR_FIFO_8 0x40u

#define NE2K_RCR_AB     0x04u
#define NE2K_RCR_MON    0x20u

#define NE2K_TCR_LB0    0x02u

#define NE2K_TX_START   0x40u
#define NE2K_RX_START   0x46u
#define NE2K_RX_STOP    0x80u

#define NE2K_RESET_TIMEOUT 10000u

#define NE2K_FRAME_MIN 60u
#define NE2K_FRAME_MAX 1518u
#define NE2K_REGION_SIZE 0x20u

typedef struct {
    uint16_t io_base_;
    pmio_region_t* pmio_;
    mutex_t device_lock_;

    uint8_t mac_[6];
    int word_mode_;
    int initialized_;
} Ne2kState;

typedef struct __attribute__((packed)) {
    uint8_t status_;
    uint8_t next_page_;
    uint16_t len_;
} Ne2kRxHeader;

static Ne2kState g_ne2k;

static uint8_t ne2k_read_reg(pmio_region_t* pmio, uint8_t reg) {
    uint8_t val = 0u;

    (void)pmio_readb(pmio, (uint16_t)reg, &val);

    return val;
}

static void ne2k_write_reg(pmio_region_t* pmio, uint8_t reg, uint8_t value) {
    (void)pmio_writeb(pmio, (uint16_t)reg, value);
}

static void ne2k_set_cmd(pmio_region_t* pmio, uint8_t value) {
    ne2k_write_reg(pmio, NE2K_REG_CR, value);
}

static uint8_t ne2k_cmd_state_bits(uint8_t cr) {
    return (uint8_t)(cr & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA | NE2K_CR_TXP));
}

static void ne2k_set_cmd_page0_idle(pmio_region_t* pmio) {
    const uint8_t cr = ne2k_read_reg(pmio, NE2K_REG_CR);
    uint8_t state = ne2k_cmd_state_bits(cr);

    if ((state & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA)) == 0u) {
        state = NE2K_CR_STA;
    }

    ne2k_set_cmd(pmio, (uint8_t)(state | NE2K_CR_RD2 | NE2K_CR_PAGE0));
}

static void ne2k_set_cmd_page1_idle(pmio_region_t* pmio) {
    const uint8_t cr = ne2k_read_reg(pmio, NE2K_REG_CR);
    uint8_t state = ne2k_cmd_state_bits(cr);

    if ((state & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA)) == 0u) {
        state = NE2K_CR_STA;
    }

    ne2k_set_cmd(pmio, (uint8_t)(state | NE2K_CR_RD2 | NE2K_CR_PAGE1));
}

static int ne2k_reg_is_floating(uint8_t v) {
    return v == 0xFFu;
}

static int ne2k_reset(pmio_region_t* pmio) {
    const uint8_t val = ne2k_read_reg(pmio, NE2K_REG_RESET);

    ne2k_write_reg(pmio, NE2K_REG_RESET, val);

    for (uint32_t i = 0u; i < NE2K_RESET_TIMEOUT; i++) {
        const uint8_t isr = ne2k_read_reg(pmio, NE2K_REG_ISR);

        if (ne2k_reg_is_floating(isr)) {
            io_wait();
            continue;
        }

        if ((isr & NE2K_ISR_RST) != 0u) {
            ne2k_write_reg(pmio, NE2K_REG_ISR, NE2K_ISR_RST);

            const uint8_t after = ne2k_read_reg(pmio, NE2K_REG_ISR);

            if (ne2k_reg_is_floating(after)) {
                io_wait();
                continue;
            }

            if ((after & NE2K_ISR_RST) != 0u) {
                io_wait();
                continue;
            }

            return 1;
        }

        io_wait();
    }

    const uint8_t isr = ne2k_read_reg(pmio, NE2K_REG_ISR);
    const uint8_t cr = ne2k_read_reg(pmio, NE2K_REG_CR);

    if (ne2k_reg_is_floating(isr) || ne2k_reg_is_floating(cr)) {
        return 0;
    }

    return 1;
}

static int ne2k_probe(pmio_region_t* pmio) {
    const uint8_t isr0 = ne2k_read_reg(pmio, NE2K_REG_ISR);

    if (ne2k_reg_is_floating(isr0)) {
        return 0;
    }

    ne2k_set_cmd(pmio, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    const uint8_t cr0 = ne2k_read_reg(pmio, NE2K_REG_CR);

    if (ne2k_reg_is_floating(cr0)) {
        return 0;
    }

    ne2k_write_reg(pmio, NE2K_REG_DCR, NE2K_DCR_FIFO_8);

    const uint8_t dcr0 = ne2k_read_reg(pmio, NE2K_REG_DCR);

    if (ne2k_reg_is_floating(dcr0)) {
        return 0;
    }

    if (!ne2k_reset(pmio)) {
        return 0;
    }

    const uint8_t isr1 = ne2k_read_reg(pmio, NE2K_REG_ISR);
    const uint8_t cr1 = ne2k_read_reg(pmio, NE2K_REG_CR);

    if (ne2k_reg_is_floating(isr1) || ne2k_reg_is_floating(cr1)) {
        return 0;
    }

    return 1;
}

static uint16_t ne2k_dma_xfer_len(uint16_t len, int word_mode) {
    if (!word_mode) {
        return len;
    }

    return (uint16_t)((len + 1u) & (uint16_t)~1u);
}

static int ne2k_wait_rdc(pmio_region_t* pmio) {
    static const uint32_t k_total_wait_us = 10000u;
    static const uint32_t k_sleep_step_us = 50u;

    uint32_t waited_us = 0u;

    while (waited_us < k_total_wait_us) {
        uint8_t isr = 0u;
        (void)pmio_readb(pmio, NE2K_REG_ISR, &isr);

        if ((isr & NE2K_ISR_RDC) != 0u) {
            (void)pmio_writeb(pmio, NE2K_REG_ISR, NE2K_ISR_RDC);
            return 1;
        }

        if (proc_current()) {
            proc_usleep(k_sleep_step_us);
            waited_us += k_sleep_step_us;
        } else {
            io_wait();
            waited_us += 1u;
        }
    }

    return 0;
}

static void ne2k_dma_read(
    uint16_t io_base, pmio_region_t* pmio,
    uint16_t addr, uint8_t* out,
    uint16_t len, int word_mode
) {
    const uint16_t xfer_len = ne2k_dma_xfer_len(len, word_mode);

    pmio_acquire_bus(pmio);

    ne2k_write_reg(pmio, NE2K_REG_ISR, NE2K_ISR_RDC);
    ne2k_set_cmd_page0_idle(pmio);

    ne2k_write_reg(pmio, NE2K_REG_RBCR0, (uint8_t)(xfer_len & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_RBCR1, (uint8_t)((xfer_len >> 8) & 0xFFu));

    ne2k_write_reg(pmio, NE2K_REG_RSAR0, (uint8_t)(addr & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_RSAR1, (uint8_t)((addr >> 8) & 0xFFu));

    ne2k_set_cmd(pmio, NE2K_CR_STA | NE2K_CR_RD0 | NE2K_CR_PAGE0);

    pmio_release_bus(pmio);

    if (word_mode) {
        const uint16_t words = xfer_len / 2u;

        if (len == xfer_len) {
            insw(io_base + NE2K_REG_DATA, out, words);
        } else {
            if (words > 1u) {
                insw(io_base + NE2K_REG_DATA, out, words - 1u);
            }

            const uint16_t last_word = inw(io_base + NE2K_REG_DATA);
            out[len - 1u] = (uint8_t)(last_word & 0xFFu);
        }
    } else {
        insb(io_base + NE2K_REG_DATA, out, xfer_len);
    }

    (void)ne2k_wait_rdc(pmio);

    pmio_acquire_bus(pmio);
    ne2k_set_cmd_page0_idle(pmio);
    pmio_release_bus(pmio);
}

static void ne2k_remote_read(
    uint16_t io_base, pmio_region_t* pmio,
    uint16_t addr, uint8_t* out, uint16_t len
) {
    ne2k_dma_read(io_base, pmio, addr, out, len, g_ne2k.word_mode_);
}

static int ne2k_dma_write(
    uint16_t io_base, pmio_region_t* pmio,
    uint16_t addr, const uint8_t* data,
    uint16_t len, int word_mode
) {
    if (!data || len == 0u) {
        return 0;
    }

    const uint16_t xfer_len = ne2k_dma_xfer_len(len, word_mode);

    pmio_acquire_bus(pmio);

    ne2k_write_reg(pmio, NE2K_REG_ISR, NE2K_ISR_RDC);
    ne2k_set_cmd_page0_idle(pmio);

    ne2k_write_reg(pmio, NE2K_REG_RBCR0, (uint8_t)(xfer_len & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_RBCR1, (uint8_t)((xfer_len >> 8) & 0xFFu));

    ne2k_write_reg(pmio, NE2K_REG_RSAR0, (uint8_t)(addr & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_RSAR1, (uint8_t)((addr >> 8) & 0xFFu));

    ne2k_set_cmd(pmio, NE2K_CR_STA | NE2K_CR_RD1 | NE2K_CR_PAGE0);

    pmio_release_bus(pmio);

    if (word_mode) {
        const uint16_t words = xfer_len / 2u;

        if (len == xfer_len) {
            outsw(io_base + NE2K_REG_DATA, data, words);
        } else {
            if (words > 1u) {
                outsw(io_base + NE2K_REG_DATA, data, words - 1u);
            }

            const uint16_t last_word = (uint16_t)data[len - 1u];
            outw(io_base + NE2K_REG_DATA, last_word);
        }
    } else {
        outsb(io_base + NE2K_REG_DATA, data, xfer_len);
    }

    const int ok = ne2k_wait_rdc(pmio);

    pmio_acquire_bus(pmio);
    ne2k_set_cmd_page0_idle(pmio);
    pmio_release_bus(pmio);

    return ok;
}

static int ne2k_remote_write(
    uint16_t io_base, pmio_region_t* pmio,
    uint16_t addr, const uint8_t* data, uint16_t len
) {
    return ne2k_dma_write(io_base, pmio, addr, data, len, g_ne2k.word_mode_);
}

static int ne2k_mac_is_plausible(const uint8_t mac[6]) {
    if (!mac) {
        return 0;
    }

    int all_zero = 1;
    int all_ff = 1;

    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00u) {
            all_zero = 0;
        }

        if (mac[i] != 0xFFu) {
            all_ff = 0;
        }
    }

    if (all_zero || all_ff) {
        return 0;
    }

    return 1;
}

static int ne2k_prom_dup_score(const uint8_t prom[32]) {
    if (!prom) {
        return 0;
    }

    int dup = 0;

    for (int i = 0; i < 16; i++) {
        if (prom[i * 2] == prom[i * 2 + 1]) {
            dup++;
        }
    }

    return dup;
}

static void ne2k_prom_extract_mac_word(const uint8_t prom[32], uint8_t out_mac[6]) {
    for (int i = 0; i < 6; i++) {
        out_mac[i] = prom[i * 2];
    }
}

static void ne2k_prom_extract_mac_byte(const uint8_t prom[32], uint8_t out_mac[6]) {
    for (int i = 0; i < 6; i++) {
        out_mac[i] = prom[i];
    }
}

static void ne2k_read_prom_mac(uint16_t io_base, pmio_region_t* pmio, uint8_t out_mac[6]) {
    uint8_t prom_word[32];
    uint8_t prom_byte[32];

    uint8_t mac_word[6];
    uint8_t mac_byte[6];

    pmio_acquire_bus(pmio);
    ne2k_write_reg(pmio, NE2K_REG_DCR, (uint8_t)(NE2K_DCR_FIFO_8 | NE2K_DCR_WTS));
    pmio_release_bus(pmio);

    ne2k_dma_read(io_base, pmio, 0u, prom_word, (uint16_t)sizeof(prom_word), 1);

    pmio_acquire_bus(pmio);
    ne2k_write_reg(pmio, NE2K_REG_DCR, NE2K_DCR_FIFO_8);
    pmio_release_bus(pmio);

    ne2k_dma_read(io_base, pmio, 0u, prom_byte, (uint16_t)sizeof(prom_byte), 0);

    ne2k_prom_extract_mac_word(prom_word, mac_word);
    ne2k_prom_extract_mac_byte(prom_byte, mac_byte);

    const int word_dup = ne2k_prom_dup_score(prom_word);
    const int word_ok = (word_dup >= 8) && ne2k_mac_is_plausible(mac_word);
    const int byte_ok = ne2k_mac_is_plausible(mac_byte);

    if (word_ok) {
        g_ne2k.word_mode_ = 1;
        memcpy(out_mac, mac_word, 6u);

        return;
    }

    g_ne2k.word_mode_ = 0;

    if (byte_ok) {
        memcpy(out_mac, mac_byte, 6u);

        return;
    }

    memcpy(out_mac, mac_word, 6u);
}

static void ne2k_program_mac(pmio_region_t* pmio, const uint8_t mac[6]) {
    pmio_acquire_bus(pmio);

    ne2k_set_cmd(pmio, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE1);

    for (int i = 0; i < 6; i++) {
        ne2k_write_reg(pmio, (uint8_t)(NE2K_REG_PAR0 + i), mac[i]);
    }

    ne2k_write_reg(pmio, NE2K_REG_CURR, (uint8_t)(NE2K_RX_START + 1u));

    ne2k_set_cmd(pmio, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    pmio_release_bus(pmio);
}

static uint8_t ne2k_read_curr(pmio_region_t* pmio) {
    pmio_acquire_bus(pmio);

    ne2k_set_cmd_page1_idle(pmio);
    const uint8_t curr = ne2k_read_reg(pmio, NE2K_REG_CURR);
    ne2k_set_cmd_page0_idle(pmio);

    pmio_release_bus(pmio);

    return curr;
}

static void ne2k_ring_read(
    uint16_t io_base, pmio_region_t* pmio,
    uint16_t addr, uint8_t* out, uint16_t len
) {
    const uint16_t ring_start = (uint16_t)(NE2K_RX_START * 256u);
    const uint16_t ring_end = (uint16_t)(NE2K_RX_STOP * 256u);

    if (addr + len <= ring_end) {
        ne2k_remote_read(io_base, pmio, addr, out, len);
        return;
    }

    const uint16_t first_len = (uint16_t)(ring_end - addr);
    ne2k_remote_read(io_base, pmio, addr, out, first_len);

    const uint16_t remain_len = (uint16_t)(len - first_len);
    ne2k_remote_read(io_base, pmio, ring_start, out + first_len, remain_len);
}

static void ne2k_update_bnry(pmio_region_t* pmio, uint8_t next_page) {
    uint8_t bnry;

    if (next_page <= NE2K_RX_START) {
        bnry = (uint8_t)(NE2K_RX_STOP - 1u);
    } else {
        bnry = (uint8_t)(next_page - 1u);
    }

    ne2k_write_reg(pmio, NE2K_REG_BNRY, bnry);
}

static void ne2k_resync_bnry_to_curr(pmio_region_t* pmio, uint8_t curr) {
    uint8_t bnry;

    if (curr <= NE2K_RX_START) {
        bnry = (uint8_t)(NE2K_RX_STOP - 1u);
    } else {
        bnry = (uint8_t)(curr - 1u);
    }

    ne2k_write_reg(pmio, NE2K_REG_BNRY, bnry);
}

static int ne2k_try_read_packet(uint8_t* out, uint32_t cap) {
    if (!g_ne2k.initialized_) {
        return -1;
    }

    if (!out || cap == 0u) {
        return -1;
    }

    mutex_lock(&g_ne2k.device_lock_);

    pmio_region_t* pmio = g_ne2k.pmio_;
    const uint16_t io_base = g_ne2k.io_base_;

    pmio_acquire_bus(pmio);
    ne2k_set_cmd_page0_idle(pmio);
    pmio_release_bus(pmio);

    uint8_t bnry = ne2k_read_reg(pmio, NE2K_REG_BNRY);
    const uint8_t curr = ne2k_read_curr(pmio);

    if (bnry < NE2K_RX_START || bnry >= NE2K_RX_STOP) {
        pmio_acquire_bus(pmio);
        ne2k_resync_bnry_to_curr(pmio, curr);
        bnry = ne2k_read_reg(pmio, NE2K_REG_BNRY);
        pmio_release_bus(pmio);
    }

    uint8_t next = (uint8_t)(bnry + 1u);

    if (next >= NE2K_RX_STOP) {
        next = NE2K_RX_START;
    }

    if (next < NE2K_RX_START) {
        pmio_acquire_bus(pmio);
        ne2k_resync_bnry_to_curr(pmio, curr);
        pmio_release_bus(pmio);

        mutex_unlock(&g_ne2k.device_lock_);
        return 0;
    }

    if (next == curr) {
        const uint8_t isr = ne2k_read_reg(pmio, NE2K_REG_ISR);

        if ((isr & 0x01u) != 0u) {
            ne2k_write_reg(pmio, NE2K_REG_ISR, 0x01u);
        }

        mutex_unlock(&g_ne2k.device_lock_);
        return 0;
    }

    const uint16_t pkt_addr = (uint16_t)next * 256u;
    Ne2kRxHeader hdr;

    ne2k_remote_read(io_base, pmio, pkt_addr, (uint8_t*)&hdr, (uint16_t)sizeof(hdr));

    const uint8_t next_page = hdr.next_page_;

    if (next_page < NE2K_RX_START || next_page >= NE2K_RX_STOP) {
        pmio_acquire_bus(pmio);
        ne2k_resync_bnry_to_curr(pmio, curr);
        pmio_release_bus(pmio);

        mutex_unlock(&g_ne2k.device_lock_);
        return 0;
    }

    const uint16_t rx_count = hdr.len_;

    if (rx_count < 4u) {
        pmio_acquire_bus(pmio);
        ne2k_update_bnry(pmio, next_page);
        ne2k_write_reg(pmio, NE2K_REG_ISR, 0x01u);
        pmio_release_bus(pmio);

        mutex_unlock(&g_ne2k.device_lock_);
        return 0;
    }

    const uint16_t frame_len = (uint16_t)(rx_count - 4u);

    if (frame_len < 14u || frame_len > (uint16_t)NE2K_FRAME_MAX) {
        pmio_acquire_bus(pmio);
        ne2k_update_bnry(pmio, next_page);
        pmio_release_bus(pmio);

        mutex_unlock(&g_ne2k.device_lock_);
        return 0;
    }

    uint16_t data_len = frame_len;

    if (data_len > cap) {
        data_len = (uint16_t)cap;
    }

    const uint16_t data_addr = (uint16_t)(pkt_addr + (uint16_t)sizeof(hdr));

    ne2k_ring_read(io_base, pmio, data_addr, out, data_len);

    pmio_acquire_bus(pmio);

    ne2k_update_bnry(pmio, next_page);
    ne2k_write_reg(pmio, NE2K_REG_ISR, 0x01u);

    pmio_release_bus(pmio);

    mutex_unlock(&g_ne2k.device_lock_);

    return (int)data_len;
}

static int ne2k_transmit(const uint8_t* data, uint32_t len) {
    if (!g_ne2k.initialized_) {
        return -1;
    }

    if (!data || len == 0u || len > NE2K_FRAME_MAX) {
        return -1;
    }

    uint32_t send_len = len;

    if (send_len < NE2K_FRAME_MIN) {
        send_len = NE2K_FRAME_MIN;
    }

    uint8_t frame[NE2K_FRAME_MAX];

    memcpy(frame, data, len);

    if (send_len > len) {
        memset(frame + len, 0, send_len - len);
    }

    mutex_lock(&g_ne2k.device_lock_);

    pmio_region_t* pmio = g_ne2k.pmio_;
    const uint16_t io_base = g_ne2k.io_base_;

    if (!ne2k_remote_write(io_base, pmio, (uint16_t)(NE2K_TX_START * 256u), frame, (uint16_t)send_len)) {
        mutex_unlock(&g_ne2k.device_lock_);
        return -1;
    }

    pmio_acquire_bus(pmio);

    ne2k_write_reg(pmio, NE2K_REG_TBCR0, (uint8_t)(send_len & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_TBCR1, (uint8_t)((send_len >> 8) & 0xFFu));
    ne2k_write_reg(pmio, NE2K_REG_TPSR, NE2K_TX_START);

    ne2k_set_cmd(pmio, NE2K_CR_STA | NE2K_CR_TXP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    pmio_release_bus(pmio);

    mutex_unlock(&g_ne2k.device_lock_);

    return (int)len;
}

static void ne2k_setup_ring(pmio_region_t* pmio) {
    pmio_acquire_bus(pmio);

    ne2k_write_reg(pmio, NE2K_REG_TPSR, NE2K_TX_START);
    ne2k_write_reg(pmio, NE2K_REG_PSTART, NE2K_RX_START);
    ne2k_write_reg(pmio, NE2K_REG_PSTOP, NE2K_RX_STOP);
    ne2k_write_reg(pmio, NE2K_REG_BNRY, NE2K_RX_START);

    pmio_release_bus(pmio);
}

static void ne2k_basic_config(pmio_region_t* pmio, int word_mode) {
    pmio_acquire_bus(pmio);

    uint8_t dcr = NE2K_DCR_FIFO_8;

    if (word_mode) {
        dcr = (uint8_t)(dcr | NE2K_DCR_WTS);
    }

    ne2k_write_reg(pmio, NE2K_REG_DCR, dcr);
    ne2k_write_reg(pmio, NE2K_REG_RBCR0, 0u);
    ne2k_write_reg(pmio, NE2K_REG_RBCR1, 0u);
    ne2k_write_reg(pmio, NE2K_REG_RCR, NE2K_RCR_MON);
    ne2k_write_reg(pmio, NE2K_REG_TCR, NE2K_TCR_LB0);
    ne2k_write_reg(pmio, NE2K_REG_ISR, 0xFFu);
    ne2k_write_reg(pmio, NE2K_REG_IMR, 0x00u);

    pmio_release_bus(pmio);
}

static void ne2k_start_device(pmio_region_t* pmio) {
    pmio_acquire_bus(pmio);

    ne2k_set_cmd(pmio, NE2K_CR_STA | NE2K_CR_RD2 | NE2K_CR_PAGE0);
    ne2k_write_reg(pmio, NE2K_REG_TCR, 0x00u);
    ne2k_write_reg(pmio, NE2K_REG_RCR, NE2K_RCR_AB);

    pmio_release_bus(pmio);
}

static int ne2k_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0u) {
        return -1;
    }

    const int r = ne2k_try_read_packet((uint8_t*)buffer, size);

    return r;
}

static int ne2k_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0u) {
        return -1;
    }

    return ne2k_transmit((const uint8_t*)buffer, size);
}

static int ne2k_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    if (req == YOS_NET_GET_MAC) {
        if (!arg) {
            return -1;
        }

        if (!g_ne2k.initialized_) {
            return -1;
        }

        yos_net_mac_t* out = (yos_net_mac_t*)arg;

        memcpy(out->mac, g_ne2k.mac_, sizeof(out->mac));

        return 0;
    }

    return -1;
}

static vfs_ops_t g_ne2k_ops = {
    .read = ne2k_vfs_read,
    .write = ne2k_vfs_write,
    .open = 0,
    .close = 0,
    .ioctl = ne2k_vfs_ioctl,
    .get_phys_page = 0,
    .poll_status = 0,
    .poll_register = 0,
};

static vfs_node_t g_ne2k_node = {
    .name = "ne2k0",
    .flags = 0u,
    .size = 0u,
    .inode_idx = 0u,
    .refs = 0u,
    .fs_driver = 0,
    .ops = &g_ne2k_ops,
    .private_data = 0,
    .private_retain = 0,
    .private_release = 0,
};

static void ne2k_vfs_init(void) {
    devfs_register(&g_ne2k_node);
}

static pmio_region_t* ne2k_find_pmio_region(uint16_t* out_base) {
    static const uint16_t candidates[] = {
        0x300u,
        0x320u,
        0x340u,
        0x360u,
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        const uint16_t base = candidates[i];

        pmio_region_t* region = pmio_request_region(base, NE2K_REGION_SIZE, "ne2k");

        if (!region) {
            continue;
        }

        if (ne2k_probe(region)) {
            *out_base = base;
            return region;
        }

        pmio_release_region(region);
    }

    return 0;
}

int ne2k_is_initialized(void) {
    return g_ne2k.initialized_;
}

int ne2k_get_mac(uint8_t out_mac[6]) {
    if (!g_ne2k.initialized_ || !out_mac) {
        return 0;
    }

    memcpy(out_mac, g_ne2k.mac_, 6u);

    return 1;
}

void ne2k_init(void) {
    if (g_ne2k.initialized_) {
        return;
    }

    mutex_init(&g_ne2k.device_lock_);

    uint16_t io_base = 0u;
    pmio_region_t* pmio = ne2k_find_pmio_region(&io_base);

    if (!pmio) {
        return;
    }

    g_ne2k.pmio_ = pmio;
    g_ne2k.io_base_ = io_base;

    pmio_acquire_bus(pmio);
    ne2k_set_cmd(pmio, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);
    pmio_release_bus(pmio);

    if (!ne2k_reset(pmio)) {
        pmio_release_region(pmio);
        g_ne2k.pmio_ = 0;

        return;
    }

    g_ne2k.word_mode_ = 0;

    pmio_acquire_bus(pmio);
    ne2k_write_reg(pmio, NE2K_REG_DCR, NE2K_DCR_FIFO_8);
    pmio_release_bus(pmio);

    ne2k_read_prom_mac(io_base, pmio, g_ne2k.mac_);

    ne2k_basic_config(pmio, g_ne2k.word_mode_);
    ne2k_setup_ring(pmio);
    ne2k_program_mac(pmio, g_ne2k.mac_);
    ne2k_start_device(pmio);

    g_ne2k.initialized_ = 1;

    ne2k_vfs_init();
}