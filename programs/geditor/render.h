#pragma once

#include "geditor_defs.h"

void draw_rect(int x, int y, int w, int h, uint32_t color);
void render_char(uint32_t* fb, int fb_w, int fb_h, int x, int y, char c, uint32_t color);
void render_string(int x, int y, const char* s, uint32_t color);
void render_editor(void);
