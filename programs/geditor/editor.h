// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "geditor_defs.h"

void editor_update_lang_from_filename(void);
int editor_set_filename(const char* path);

void status_set(const char* s);
void status_set_col(const char* s, uint32_t col);

int editor_find_next_from(int start);

void mini_backspace(void);
void mini_putc(char c);

void enter_find_mode(void);
void enter_goto_mode(void);
void enter_open_mode(void);
void apply_find_mode(void);
void apply_goto_mode(void);
void apply_open_mode(void);

void editor_insert_newline_autoindent(void);
void editor_insert_tab_smart(void);

void editor_undo(void);
void editor_redo(void);

int get_line_start(int pos);
int get_line_len(int start);
void update_pref_col(void);

void delete_range(int start, int end);
void insert_str(const char* s, int len);
void insert_char(char c);
void backspace(void);
void copy_selection(void);
void paste_clipboard(void);

void handle_selection(int select);
void move_left(int select);
void move_right(int select);
void move_up(int select);
void move_down(int select);
void move_word_left(int select);
void move_word_right(int select);

int get_pos_from_coords(int mx, int my);
