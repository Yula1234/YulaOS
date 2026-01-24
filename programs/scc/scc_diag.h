// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_DIAG_H_INCLUDED
#define SCC_DIAG_H_INCLUDED

#include "scc_common.h"

void scc_fatal_at(const char* file, const char* src, int line, int col, const char* msg);

#endif
