// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_PROC_H
#define YOS_NETWORKCTL_PROC_H

#include "netctl_common.h"

const char* netctl_proc_state_name(uint32_t st);
int netctl_find_process(const char* name, yos_proc_info_t* out);

#endif
