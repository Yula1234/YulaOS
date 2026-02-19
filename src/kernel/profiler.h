#ifndef KERNEL_PROFILER_H
#define KERNEL_PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

void profiler_init(void);

void profiler_dump_stats(void);
void profiler_reset_stats(void);

void profiler_irq_enter(void);
void profiler_irq_exit(void);

void profiler_task(void* arg);

#ifdef __cplusplus
}
#endif

#endif
