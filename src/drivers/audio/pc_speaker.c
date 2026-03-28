/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <hal/delay.h>
#include <hal/pmio.h>

#include <lib/compiler.h>

#include "pc_speaker.h"

#define PIT_FREQUENCY      1193180u

#define PORT_PIT_CH2       0x42u
#define PORT_PIT_COMMAND   0x43u
#define PORT_PPI_CTRL      0x61u

#define PIT_CMD_CH2_SQUARE 0xB6u

#define PPI_SPEAKER_ENABLE 0x03u
#define PPI_SPEAKER_MASK   0xFCu

static pmio_region_t* g_pit_region = 0;
static pmio_region_t* g_ppi_ctrl_region = 0;

static int g_speaker_ready = 0;

void pc_speaker_init(void) {
    if (unlikely(g_speaker_ready)) {
        return;
    }

    g_pit_region = pmio_request_region(PORT_PIT_CH2, 2u, "pit_ch2");

    if (unlikely(!g_pit_region)) {
        return;
    }

    g_ppi_ctrl_region = pmio_request_region(PORT_PPI_CTRL, 1u, "ppi_speaker");
    
    if (unlikely(!g_ppi_ctrl_region)) {
        pmio_release_region(g_pit_region);
        g_pit_region = 0;

        return;
    }

    g_speaker_ready = 1;

    pc_speaker_stop();
}

void pc_speaker_play(uint32_t frequency) {
    if (unlikely(!g_speaker_ready)) {
        return;
    }

    if (unlikely(frequency == 0u || frequency > PIT_FREQUENCY)) {
        return;
    }

    const uint32_t divisor = PIT_FREQUENCY / frequency;

    pmio_acquire_bus(g_pit_region);

    (void)pmio_writeb(g_pit_region, 1u, PIT_CMD_CH2_SQUARE);

    (void)pmio_writeb(g_pit_region, 0u, (uint8_t)(divisor & 0xFFu));
    (void)pmio_writeb(g_pit_region, 0u, (uint8_t)((divisor >> 8) & 0xFFu));

    pmio_release_bus(g_pit_region);

    pmio_acquire_bus(g_ppi_ctrl_region);

    uint8_t ppi_state = 0;
    (void)pmio_readb(g_ppi_ctrl_region, 0u, &ppi_state);

    if ((ppi_state & PPI_SPEAKER_ENABLE) != PPI_SPEAKER_ENABLE) {
        (void)pmio_writeb(g_ppi_ctrl_region, 0u, ppi_state | PPI_SPEAKER_ENABLE);
    }

    pmio_release_bus(g_ppi_ctrl_region);
}

void pc_speaker_stop(void) {
    if (unlikely(!g_speaker_ready)) {
        return;
    }

    pmio_acquire_bus(g_ppi_ctrl_region);

    uint8_t ppi_state = 0;
    (void)pmio_readb(g_ppi_ctrl_region, 0u, &ppi_state);

    (void)pmio_writeb(g_ppi_ctrl_region, 0u, ppi_state & PPI_SPEAKER_MASK);

    pmio_release_bus(g_ppi_ctrl_region);
}

void pc_speaker_beep(void) {
    if (unlikely(!g_speaker_ready)) {
        return;
    }

    pc_speaker_play(1000u);

    mdelay(100u);

    pc_speaker_stop();
}

void pc_speaker_error(void) {
    if (unlikely(!g_speaker_ready)) {
        return;
    }

    for (uint32_t freq = 2000u; freq > 300u; freq -= 15u) {
        pc_speaker_play(freq);

        mdelay(2u);
    }

    pc_speaker_stop();
}