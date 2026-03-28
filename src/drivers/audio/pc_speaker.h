// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_PC_SPEAKER_H
#define DRIVERS_PC_SPEAKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pc_speaker_init(void);
void pc_speaker_play(uint32_t frequency);
void pc_speaker_stop(void);

void pc_speaker_beep(void);
void pc_speaker_error(void);

#ifdef __cplusplus
}
#endif

#endif