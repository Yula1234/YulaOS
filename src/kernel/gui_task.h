#ifndef KERNEL_GUI_TASK_H
#define KERNEL_GUI_TASK_H

#include "../drivers/vga.h"
#include "../drivers/mouse.h"
#include "sched.h"
#include <stdint.h>

void gui_task(void* arg);

#endif