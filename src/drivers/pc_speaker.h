#ifndef DRIVERS_PC_SPEAKER_H
#define DRIVERS_PC_SPEAKER_H

#include <stdint.h>

void pc_speaker_init(void);

void pc_speaker_play(uint32_t frequency);

void pc_speaker_stop(void);

void pc_speaker_beep(void);

#endif