#include "pc_speaker.h"
#include <hal/io.h>

static void simple_wait(int count) {
    for (volatile int i = 0; i < count * 10000; i++);
}

void pc_speaker_play(uint32_t frequency) {
    uint32_t div;
    uint8_t tmp;

    div = 1193180 / frequency;

    outb(0x43, 0xB6);

    outb(0x42, (uint8_t)(div));
    outb(0x42, (uint8_t)(div >> 8));

    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

void pc_speaker_stop(void) {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

void pc_speaker_beep(void) {
    pc_speaker_play(1000);
    
    simple_wait(1000); 
    
    pc_speaker_stop();
}

void pc_speaker_init(void) {
    pc_speaker_stop();
}