#pragma once

#include "geditor_defs.h"

int gb_len(const GapBuf* g);
char gb_char_at(const GapBuf* g, int pos);

void gb_init(GapBuf* g, int initial_cap);
void gb_destroy(GapBuf* g);

int gb_insert_at(GapBuf* g, int pos, const char* s, int slen);
int gb_delete_range(GapBuf* g, int start, int end);
char* gb_copy_range(const GapBuf* g, int start, int end);

int gb_find_forward(const GapBuf* g, int start, const char* needle, int nlen);
