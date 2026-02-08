// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "editor_file.h"
#include "editor.h"
#include "gapbuf.h"
#include "lines.h"
#include "undo.h"

static int load_file_impl(int silent) {
    if (!ed.filename[0]) {
        if (!silent) {
            status_set_col("No file name", C_UI_ERROR);
        }
        return 0;
    }

    int fd = open(ed.filename, 0);
    if (fd < 0) {
        if (!silent) {
            status_set_col("Failed to open file", C_UI_ERROR);
        }
        return 0;
    }

    stat_t st;
    if (stat(ed.filename, &st) != 0) {
        close(fd);
        if (!silent) {
            status_set_col("Failed to stat file", C_UI_ERROR);
        }
        return 0;
    }

    int size = (int)st.size;
    char* buf = 0;
    if (size > 0) {
        buf = (char*)malloc((size_t)size);
        if (!buf) {
            close(fd);
            if (!silent) {
                status_set_col("No memory", C_UI_ERROR);
            }
            return 0;
        }
        int rd = read(fd, buf, size);
        if (rd != size) {
            free(buf);
            close(fd);
            if (!silent) {
                status_set_col("Read error", C_UI_ERROR);
            }
            return 0;
        }
    }
    close(fd);

    gb_destroy(&ed.text);
    gb_init(&ed.text, size + 32);
    if (size > 0) {
        gb_insert_at(&ed.text, 0, buf, size);
        free(buf);
    }

    lines_rebuild(&ed.lines, &ed.text, ed.lang);

    ed.cursor = 0;
    ed.sel_bound = -1;
    ed.scroll_y = 0;
    ed.dirty = 0;
    ed.pref_col = 0;
    ustack_reset(&ed.undo);
    ustack_reset(&ed.redo);
    ed.find_len = 0;
    ed.find[0] = 0;

    if (!silent) {
        status_set_col("File loaded", C_UI_OK);
    }
    return 1;
}

int load_file(void) {
    return load_file_impl(0);
}

int load_file_silent(void) {
    return load_file_impl(1);
}

void save_file(void) {
    if (!ed.filename[0]) {
        status_set_col("No file name", C_UI_ERROR);
        return;
    }
    int fd = open(ed.filename, 1);
    if (fd < 0) {
        status_set_col("Failed to open file", C_UI_ERROR);
        return;
    }

    int len = gb_len(&ed.text);
    int ok = 1;
    for (int i = 0; i < len; i++) {
        char c = gb_char_at(&ed.text, i);
        int wr = write(fd, &c, 1);
        if (wr != 1) {
            ok = 0;
            break;
        }
    }
    close(fd);

    if (ok) {
        ed.dirty = 0;
        status_set_col("Saved", C_UI_OK);
    } else {
        status_set_col("Write error", C_UI_ERROR);
    }
}
