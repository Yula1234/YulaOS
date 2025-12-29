#ifndef KERNEL_KDB_H
#define KERNEL_KDB_H

#include "proc.h"

void kdb_enter(const char* reason, task_t* faulty_process);

#endif