// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "geditor_defs.h"

void lines_init(LineIndex* li);
void lines_destroy(LineIndex* li);
int lines_ensure(LineIndex* li, int need);
void lines_rebuild(LineIndex* li, const GapBuf* g, int lang);
int lines_find_line(const LineIndex* li, int pos);

int lines_apply_insert(LineIndex* li, const GapBuf* g, int pos, const char* s, int slen, int lang);
int lines_apply_delete(LineIndex* li, const GapBuf* g, int start, int end, int lang);
