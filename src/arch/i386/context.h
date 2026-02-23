// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234


#ifndef ARCH_I386_CONTEXT_H
#define ARCH_I386_CONTEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ctx_switch(uint32_t** old_esp_store, uint32_t* new_esp);
void ctx_start(uint32_t* new_esp);
void irq_return(void);

#ifdef __cplusplus
}
#endif

#endif