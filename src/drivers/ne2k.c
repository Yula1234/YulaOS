// SPDX-License-Identifier: GPL-2.0

#include <drivers/ne2k.h>

#include <fs/vfs.h>
#include <hal/io.h>
#include <hal/lock.h>
#include <yos/ioctl.h>

#include <lib/string.h>

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

#define NE2K_RESET_TIMEOUT  10000u

#define NE2K_FRAME_MIN 60u
#define NE2K_FRAME_MAX 1518u

typedef struct {
    uint16_t io_base;
    uint8_t mac[6];
    int word_mode;
    int initialized;
} ne2k_state_t;

static ne2k_state_t g_ne2k;
static spinlock_t g_ne2k_tx_lock;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t next_page;
    uint16_t len;
} ne2k_rx_hdr_t;

static uint8_t ne2k_read_reg(uint16_t base, uint8_t reg) {
    return inb((uint16_t)(base + reg));
}

static void ne2k_write_reg(uint16_t base, uint8_t reg, uint8_t value) {
    outb((uint16_t)(base + reg), value);
}

static void ne2k_set_cmd(uint16_t base, uint8_t value) {
    ne2k_write_reg(base, NE2K_REG_CR, value);
}

static uint8_t ne2k_cmd_state_bits(uint8_t cr) {
    return (uint8_t)(cr & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA | NE2K_CR_TXP));
}

static void ne2k_set_cmd_page0_idle(uint16_t base) {
    uint8_t cr = ne2k_read_reg(base, NE2K_REG_CR);
    uint8_t state = ne2k_cmd_state_bits(cr);

    if ((state & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA)) == 0u) {
        state = NE2K_CR_STA;
    }

    ne2k_set_cmd(base, (uint8_t)(state | NE2K_CR_RD2 | NE2K_CR_PAGE0));
}

static void ne2k_set_cmd_page1_idle(uint16_t base) {
    uint8_t cr = ne2k_read_reg(base, NE2K_REG_CR);
    uint8_t state = ne2k_cmd_state_bits(cr);

    if ((state & (uint8_t)(NE2K_CR_STP | NE2K_CR_STA)) == 0u) {
        state = NE2K_CR_STA;
    }

    ne2k_set_cmd(base, (uint8_t)(state | NE2K_CR_RD2 | NE2K_CR_PAGE1));
}

static int ne2k_reg_is_floating(uint8_t v) {
    return v == 0xFFu;
}

static int ne2k_reset(uint16_t base) {
    uint8_t val = ne2k_read_reg(base, NE2K_REG_RESET);
    ne2k_write_reg(base, NE2K_REG_RESET, val);

    for (uint32_t i = 0; i < NE2K_RESET_TIMEOUT; i++) {
        uint8_t isr = ne2k_read_reg(base, NE2K_REG_ISR);
        if (ne2k_reg_is_floating(isr)) {
            io_wait();
            continue;
        }
        if (isr & NE2K_ISR_RST) {
            ne2k_write_reg(base, NE2K_REG_ISR, NE2K_ISR_RST);

            uint8_t after = ne2k_read_reg(base, NE2K_REG_ISR);
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

    uint8_t isr = ne2k_read_reg(base, NE2K_REG_ISR);
    uint8_t cr = ne2k_read_reg(base, NE2K_REG_CR);
    if (ne2k_reg_is_floating(isr) || ne2k_reg_is_floating(cr)) {
        return 0;
    }

    return 1;
}

static int ne2k_probe(uint16_t base) {
    uint8_t isr0 = ne2k_read_reg(base, NE2K_REG_ISR);
    if (ne2k_reg_is_floating(isr0)) {
        return 0;
    }

    ne2k_set_cmd(base, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    uint8_t cr0 = ne2k_read_reg(base, NE2K_REG_CR);
    if (ne2k_reg_is_floating(cr0)) {
        return 0;
    }

    ne2k_write_reg(base, NE2K_REG_DCR, NE2K_DCR_FIFO_8);
    uint8_t dcr0 = ne2k_read_reg(base, NE2K_REG_DCR);
    if (ne2k_reg_is_floating(dcr0)) {
        return 0;
    }

    if (!ne2k_reset(base)) {
        return 0;
    }

    uint8_t isr1 = ne2k_read_reg(base, NE2K_REG_ISR);
    uint8_t cr1 = ne2k_read_reg(base, NE2K_REG_CR);
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

static int ne2k_wait_rdc(uint16_t base);

static void ne2k_dma_read(uint16_t base, uint16_t addr, uint8_t* out, uint16_t len, int word_mode) {
    uint16_t xfer_len = ne2k_dma_xfer_len(len, word_mode);

    ne2k_write_reg(base, NE2K_REG_ISR, NE2K_ISR_RDC);
    ne2k_set_cmd_page0_idle(base);

    ne2k_write_reg(base, NE2K_REG_RBCR0, (uint8_t)(xfer_len & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RBCR1, (uint8_t)((xfer_len >> 8) & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RSAR0, (uint8_t)(addr & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RSAR1, (uint8_t)((addr >> 8) & 0xFFu));

    ne2k_set_cmd(base, NE2K_CR_STA | NE2K_CR_RD0 | NE2K_CR_PAGE0);

    if (word_mode) {
        uint16_t i = 0;
        while ((uint16_t)(i + 1u) < len) {
            uint16_t w = inw((uint16_t)(base + NE2K_REG_DATA));
            out[i] = (uint8_t)(w & 0xFFu);
            out[i + 1u] = (uint8_t)((w >> 8) & 0xFFu);
            i = (uint16_t)(i + 2u);
        }

        if (i < len) {
            uint16_t w = inw((uint16_t)(base + NE2K_REG_DATA));
            out[i] = (uint8_t)(w & 0xFFu);
        }
    } else {
        for (uint16_t i = 0; i < len; i++) {
            out[i] = ne2k_read_reg(base, NE2K_REG_DATA);
        }
    }

    (void)ne2k_wait_rdc(base);
    ne2k_set_cmd_page0_idle(base);
}

static void ne2k_remote_read(uint16_t base, uint16_t addr, uint8_t* out, uint16_t len) {
    ne2k_dma_read(base, addr, out, len, g_ne2k.word_mode);
}

static int ne2k_wait_rdc(uint16_t base) {
    for (uint32_t i = 0; i < NE2K_RESET_TIMEOUT; i++) {
        uint8_t isr = ne2k_read_reg(base, NE2K_REG_ISR);
        if (isr & NE2K_ISR_RDC) {
            ne2k_write_reg(base, NE2K_REG_ISR, NE2K_ISR_RDC);
            return 1;
        }
        io_wait();
    }
    return 0;
}

static int ne2k_dma_write(uint16_t base, uint16_t addr, const uint8_t* data, uint16_t len, int word_mode) {
    if (!data) {
        return 0;
    }

    if (len == 0) {
        return 0;
    }

    ne2k_write_reg(base, NE2K_REG_ISR, NE2K_ISR_RDC);
    ne2k_set_cmd_page0_idle(base);

    uint16_t xfer_len = ne2k_dma_xfer_len(len, word_mode);

    ne2k_write_reg(base, NE2K_REG_RBCR0, (uint8_t)(xfer_len & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RBCR1, (uint8_t)((xfer_len >> 8) & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RSAR0, (uint8_t)(addr & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_RSAR1, (uint8_t)((addr >> 8) & 0xFFu));

    ne2k_set_cmd(base, NE2K_CR_STA | NE2K_CR_RD1 | NE2K_CR_PAGE0);

    if (word_mode) {
        uint16_t i = 0;
        while ((uint16_t)(i + 1u) < len) {
            uint16_t w = (uint16_t)data[i] | ((uint16_t)data[i + 1u] << 8);
            outw((uint16_t)(base + NE2K_REG_DATA), w);
            i = (uint16_t)(i + 2u);
        }

        if (i < len) {
            uint16_t w = (uint16_t)data[i];
            outw((uint16_t)(base + NE2K_REG_DATA), w);
        }
    } else {
        for (uint16_t i = 0; i < len; i++) {
            ne2k_write_reg(base, NE2K_REG_DATA, data[i]);
        }
    }

    int ok = ne2k_wait_rdc(base);
    ne2k_set_cmd_page0_idle(base);
    return ok;
}

static int ne2k_remote_write(uint16_t base, uint16_t addr, const uint8_t* data, uint16_t len) {
    return ne2k_dma_write(base, addr, data, len, g_ne2k.word_mode);
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

static void ne2k_read_prom_mac(uint16_t base, uint8_t out_mac[6]) {
    uint8_t prom_word[32];
    uint8_t prom_byte[32];

    uint8_t mac_word[6];
    uint8_t mac_byte[6];

    ne2k_write_reg(base, NE2K_REG_DCR, (uint8_t)(NE2K_DCR_FIFO_8 | NE2K_DCR_WTS));
    ne2k_dma_read(base, 0, prom_word, (uint16_t)sizeof(prom_word), 1);

    ne2k_write_reg(base, NE2K_REG_DCR, NE2K_DCR_FIFO_8);
    ne2k_dma_read(base, 0, prom_byte, (uint16_t)sizeof(prom_byte), 0);

    ne2k_prom_extract_mac_word(prom_word, mac_word);
    ne2k_prom_extract_mac_byte(prom_byte, mac_byte);

    int word_dup = ne2k_prom_dup_score(prom_word);
    int word_ok = (word_dup >= 8) && ne2k_mac_is_plausible(mac_word);
    int byte_ok = ne2k_mac_is_plausible(mac_byte);

    if (word_ok) {
        g_ne2k.word_mode = 1;
        memcpy(out_mac, mac_word, 6);
        return;
    }

    g_ne2k.word_mode = 0;
    if (byte_ok) {
        memcpy(out_mac, mac_byte, 6);
        return;
    }

    memcpy(out_mac, mac_word, 6);
}

static void ne2k_program_mac(uint16_t base, const uint8_t mac[6]) {
    ne2k_set_cmd(base, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE1);

    for (int i = 0; i < 6; i++) {
        ne2k_write_reg(base, (uint8_t)(NE2K_REG_PAR0 + i), mac[i]);
    }

    ne2k_write_reg(base, NE2K_REG_CURR, (uint8_t)(NE2K_RX_START + 1u));

    ne2k_set_cmd(base, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);
}

static uint8_t ne2k_read_curr(uint16_t base) {
    ne2k_set_cmd_page1_idle(base);
    uint8_t curr = ne2k_read_reg(base, NE2K_REG_CURR);
    ne2k_set_cmd_page0_idle(base);
    return curr;
}

static void ne2k_ring_read(uint16_t base, uint16_t addr, uint8_t* out, uint16_t len) {
    uint16_t ring_start = (uint16_t)(NE2K_RX_START * 256u);
    uint16_t ring_end = (uint16_t)(NE2K_RX_STOP * 256u);

    if (addr + len <= ring_end) {
        ne2k_remote_read(base, addr, out, len);
        return;
    }

    uint16_t first = (uint16_t)(ring_end - addr);
    ne2k_remote_read(base, addr, out, first);

    uint16_t remain = (uint16_t)(len - first);
    ne2k_remote_read(base, ring_start, out + first, remain);
}

static void ne2k_update_bnry(uint16_t base, uint8_t next_page) {
    uint8_t bnry;
    if (next_page <= NE2K_RX_START) {
        bnry = (uint8_t)(NE2K_RX_STOP - 1u);
    } else {
        bnry = (uint8_t)(next_page - 1u);
    }
    ne2k_write_reg(base, NE2K_REG_BNRY, bnry);
}

static void ne2k_resync_bnry_to_curr(uint16_t base, uint8_t curr) {
    uint8_t bnry;
    if (curr <= NE2K_RX_START) {
        bnry = (uint8_t)(NE2K_RX_STOP - 1u);
    } else {
        bnry = (uint8_t)(curr - 1u);
    }
    ne2k_write_reg(base, NE2K_REG_BNRY, bnry);
}

static int ne2k_try_read_packet(uint8_t* out, uint32_t cap) {
    if (!g_ne2k.initialized) {
        return -1;
    }

    if (!out) {
        return -1;
    }

    if (cap == 0) {
        return -1;
    }

    uint16_t base = g_ne2k.io_base;

    ne2k_set_cmd_page0_idle(base);

    uint8_t bnry = ne2k_read_reg(base, NE2K_REG_BNRY);
    uint8_t curr = ne2k_read_curr(base);

    if (bnry < NE2K_RX_START || bnry >= NE2K_RX_STOP) {
        ne2k_resync_bnry_to_curr(base, curr);
        bnry = ne2k_read_reg(base, NE2K_REG_BNRY);
    }

    uint8_t next = (uint8_t)(bnry + 1u);
    if (next >= NE2K_RX_STOP) {
        next = NE2K_RX_START;
    }

    if (next < NE2K_RX_START) {
        ne2k_resync_bnry_to_curr(base, curr);
        return 0;
    }

    if (next == curr) {
        uint8_t isr = ne2k_read_reg(base, NE2K_REG_ISR);
        if ((isr & 0x01u) != 0u) {
            ne2k_write_reg(base, NE2K_REG_ISR, 0x01u);
        }
        return 0;
    }

    uint16_t pkt_addr = (uint16_t)next * 256u;
    ne2k_rx_hdr_t hdr;
    ne2k_remote_read(base, pkt_addr, (uint8_t*)&hdr, (uint16_t)sizeof(hdr));

    uint8_t next_page = hdr.next_page;
    if (next_page < NE2K_RX_START || next_page >= NE2K_RX_STOP) {
        ne2k_resync_bnry_to_curr(base, curr);
        return 0;
    }

    uint16_t rx_count = hdr.len;
    if (rx_count < 4u) {
        ne2k_update_bnry(base, next_page);
        ne2k_write_reg(base, NE2K_REG_ISR, 0x01u);
        return 0;
    }

    uint16_t frame_len = (uint16_t)(rx_count - 4u);
    if (frame_len < 14u || frame_len > (uint16_t)NE2K_FRAME_MAX) {
        ne2k_update_bnry(base, next_page);
        return 0;
    }

    uint16_t data_len = frame_len;
    if (data_len > cap) {
        data_len = (uint16_t)cap;
    }

    uint16_t data_addr = (uint16_t)(pkt_addr + (uint16_t)sizeof(hdr));
    ne2k_ring_read(base, data_addr, out, data_len);

    ne2k_update_bnry(base, next_page);
    ne2k_write_reg(base, NE2K_REG_ISR, 0x01u);
    return (int)data_len;
}

static int ne2k_transmit(const uint8_t* data, uint32_t len) {
    if (!g_ne2k.initialized) {
        return -1;
    }

    if (!data) {
        return -1;
    }

    if (len == 0) {
        return -1;
    }

    if (len > NE2K_FRAME_MAX) {
        return -1;
    }

    uint32_t send_len = len;
    if (send_len < NE2K_FRAME_MIN) {
        send_len = NE2K_FRAME_MIN;
    }

    uint8_t frame[NE2K_FRAME_MAX];
    if (len > 0) {
        memcpy(frame, data, len);
    }
    if (send_len > len) {
        memset(frame + len, 0, send_len - len);
    }

    uint32_t flags = spinlock_acquire_safe(&g_ne2k_tx_lock);

    uint16_t base = g_ne2k.io_base;
    if (!ne2k_remote_write(base, (uint16_t)(NE2K_TX_START * 256u), frame, (uint16_t)send_len)) {
        spinlock_release_safe(&g_ne2k_tx_lock, flags);
        return -1;
    }

    ne2k_write_reg(base, NE2K_REG_TBCR0, (uint8_t)(send_len & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_TBCR1, (uint8_t)((send_len >> 8) & 0xFFu));
    ne2k_write_reg(base, NE2K_REG_TPSR, NE2K_TX_START);
    ne2k_set_cmd(base, NE2K_CR_STA | NE2K_CR_TXP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    spinlock_release_safe(&g_ne2k_tx_lock, flags);
    return (int)len;
}

static void ne2k_setup_ring(uint16_t base) {
    ne2k_write_reg(base, NE2K_REG_TPSR, NE2K_TX_START);
    ne2k_write_reg(base, NE2K_REG_PSTART, NE2K_RX_START);
    ne2k_write_reg(base, NE2K_REG_PSTOP, NE2K_RX_STOP);
    ne2k_write_reg(base, NE2K_REG_BNRY, NE2K_RX_START);
}

static void ne2k_basic_config(uint16_t base) {
    uint8_t dcr = NE2K_DCR_FIFO_8;
    if (g_ne2k.word_mode) {
        dcr = (uint8_t)(dcr | NE2K_DCR_WTS);
    }

    ne2k_write_reg(base, NE2K_REG_DCR, dcr);
    ne2k_write_reg(base, NE2K_REG_RBCR0, 0);
    ne2k_write_reg(base, NE2K_REG_RBCR1, 0);
    ne2k_write_reg(base, NE2K_REG_RCR, NE2K_RCR_MON);
    ne2k_write_reg(base, NE2K_REG_TCR, NE2K_TCR_LB0);
    ne2k_write_reg(base, NE2K_REG_ISR, 0xFFu);
    ne2k_write_reg(base, NE2K_REG_IMR, 0x00u);
}

static void ne2k_start_device(uint16_t base) {
    ne2k_set_cmd(base, NE2K_CR_STA | NE2K_CR_RD2 | NE2K_CR_PAGE0);
    ne2k_write_reg(base, NE2K_REG_TCR, 0x00u);
    ne2k_write_reg(base, NE2K_REG_RCR, NE2K_RCR_AB);
}

static int ne2k_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer) {
        return -1;
    }

    if (size == 0) {
        return -1;
    }

    int r = ne2k_try_read_packet((uint8_t*)buffer, size);
    return r;
}

static int ne2k_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer) {
        return -1;
    }

    if (size == 0) {
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

        if (!g_ne2k.initialized) {
            return -1;
        }

        yos_net_mac_t* out = (yos_net_mac_t*)arg;
        memcpy(out->mac, g_ne2k.mac, sizeof(out->mac));
        return 0;
    }
    return -1;
}

static vfs_ops_t ne2k_ops = {
    .read = ne2k_vfs_read,
    .write = ne2k_vfs_write,
    .ioctl = ne2k_vfs_ioctl,
};

static vfs_node_t ne2k_node = {
    .name = "ne2k0",
    .ops = &ne2k_ops,
};

static void ne2k_vfs_init(void) {
    devfs_register(&ne2k_node);
}

static int ne2k_find_io_base(uint16_t* out_base) {
    static const uint16_t candidates[] = {
        0x300u,
        0x320u,
        0x340u,
        0x360u,
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        uint16_t base = candidates[i];
        if (ne2k_probe(base)) {
            *out_base = base;
            return 1;
        }
    }

    return 0;
}

int ne2k_is_initialized(void) {
    return g_ne2k.initialized;
}

int ne2k_get_mac(uint8_t out_mac[6]) {
    if (!g_ne2k.initialized || !out_mac) {
        return 0;
    }

    memcpy(out_mac, g_ne2k.mac, 6);
    return 1;
}

void ne2k_init(void) {
    if (g_ne2k.initialized) {
        return;
    }

    spinlock_init(&g_ne2k_tx_lock);

    uint16_t io_base = 0;
    if (!ne2k_find_io_base(&io_base)) {
        return;
    }

    g_ne2k.io_base = io_base;

    ne2k_set_cmd(io_base, NE2K_CR_STP | NE2K_CR_RD2 | NE2K_CR_PAGE0);

    if (!ne2k_reset(io_base)) {
        return;
    }

    g_ne2k.word_mode = 0;
    ne2k_write_reg(io_base, NE2K_REG_DCR, NE2K_DCR_FIFO_8);
    ne2k_read_prom_mac(io_base, g_ne2k.mac);

    ne2k_basic_config(io_base);
    ne2k_setup_ring(io_base);
    ne2k_program_mac(io_base, g_ne2k.mac);
    ne2k_start_device(io_base);

    g_ne2k.initialized = 1;
    ne2k_vfs_init();
}
