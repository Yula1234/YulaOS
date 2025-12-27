#include <lib/string.h>
#include <hal/apic.h>
#include "cpu.h"

cpu_t cpus[MAX_CPUS];
int cpu_count = 0;

void cpu_init_system(void) {
    memset(cpus, 0, sizeof(cpus));
    for(int i=0; i<MAX_CPUS; i++) {
        cpus[i].id = -1;
        cpus[i].index = i;
        cpus[i].started = 0;
        
        cpus[i].runq_head = 0;
        cpus[i].runq_tail = 0;
        cpus[i].runq_count = 0;
        spinlock_init(&cpus[i].lock);
    }
}

static inline uint32_t lapic_read_local(uint32_t reg) {
    return *(volatile uint32_t*)(LAPIC_BASE + reg);
}

cpu_t* cpu_current(void) {
    if (cpu_count == 0) return &cpus[0];

    uint32_t apic_id_reg = lapic_read_local(LAPIC_ID);
    int apic_id = (apic_id_reg >> 24) & 0xFF;

    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].id == apic_id) {
            return &cpus[i];
        }
    }
    return &cpus[0];
}