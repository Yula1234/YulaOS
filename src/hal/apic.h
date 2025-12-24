    
#ifndef HAL_APIC_H
#define HAL_APIC_H

#include <stdint.h>

#define LAPIC_BASE          0xFEE00000

#define LAPIC_ID            0x0020
#define LAPIC_VER           0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_EOI           0x00B0
#define LAPIC_LDR           0x00D0
#define LAPIC_DFR           0x00E0
#define LAPIC_SVR           0x00F0
#define LAPIC_ESR           0x0280
#define LAPIC_ICRLO         0x0300
#define LAPIC_ICRHI         0x0310
#define LAPIC_TIMER         0x0320
#define LAPIC_THERMAL       0x0330
#define LAPIC_PERF          0x0340
#define LAPIC_LINT0         0x0350
#define LAPIC_LINT1         0x0360
#define LAPIC_ERROR         0x0370
#define LAPIC_TIMER_INIT    0x0380
#define LAPIC_TIMER_CUR     0x0390
#define LAPIC_TIMER_DIV     0x03E0

void lapic_init(void);
void lapic_timer_init(uint32_t hz);
void lapic_eoi(void);

#endif

  