#pragma once

#include "geditor_defs.h"

int min(int a, int b);
int max(int a, int b);

int is_digit(char c);
int is_alpha(char c);
int is_word_char(char c);

char lower_char(char c);
const char* path_ext(const char* s);
const char* path_base(const char* s);

void fmt_title_ellipsis(const char* s, char* out, int out_cap, int max_chars);
void fmt_int(int n, char* buf);
