#include "editor.h"
#include "editor_file.h"
#include "gapbuf.h"
#include "lines.h"
#include "undo.h"
#include "util.h"

static void editor_delete_raw(int start, int end);
static void editor_insert_raw(int pos, const char* s, int len);
static void editor_apply_action(UndoAction a, UndoStack* inverse_target);
static void mini_set(const char* s, int len);
static void mini_clear(void);
static int editor_insert_with_undo_at(int pos, const char* s, int len);
static int count_line_indent_spaces(int line_start);
static int is_asm_label_line(int line_start);
static int last_nonspace_before_in_line(int line_start, int pos);
static int next_nonspace_after_in_line(int pos, int line_end);
static int ext_eq_ci(const char* ext, const char* lit);

int get_line_start(int pos) {
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int line = lines_find_line(&ed.lines, pos);
    if (line < 0) line = 0;
    if (line >= ed.lines.count) line = ed.lines.count - 1;
    if (line < 0) return 0;
    return ed.lines.starts[line];
}

int get_line_len(int start) {
    int text_len = gb_len(&ed.text);
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int line = lines_find_line(&ed.lines, start);
    if (line < 0) line = 0;
    if (line >= ed.lines.count) line = ed.lines.count - 1;
    int line_start = (line >= 0 && line < ed.lines.count) ? ed.lines.starts[line] : 0;
    int line_end = (line + 1 < ed.lines.count) ? (ed.lines.starts[line + 1] - 1) : text_len;
    if (line_end < line_start) line_end = line_start;
    return line_end - line_start;
}

void update_pref_col() {
    ed.pref_col = ed.cursor - get_line_start(ed.cursor);
}

static void editor_delete_raw(int start, int end) {
    gb_delete_range(&ed.text, start, end);
    if (!lines_apply_delete(&ed.lines, &ed.text, start, end, ed.lang)) {
        lines_rebuild(&ed.lines, &ed.text, ed.lang);
    }
    ed.dirty = 1;
}

static void editor_insert_raw(int pos, const char* s, int len) {
    gb_insert_at(&ed.text, pos, s, len);
    if (!lines_apply_insert(&ed.lines, &ed.text, pos, s, len, ed.lang)) {
        lines_rebuild(&ed.lines, &ed.text, ed.lang);
    }
    ed.dirty = 1;
}

static void editor_apply_action(UndoAction a, UndoStack* inverse_target) {
    if (a.type == UNDO_DELETE) {
        char* txt = gb_copy_range(&ed.text, a.pos, a.pos + a.len);
        editor_delete_raw(a.pos, a.pos + a.len);
        ed.cursor = a.pos;
        ed.sel_bound = -1;
        update_pref_col();

        UndoAction inv;
        inv.type = UNDO_INSERT;
        inv.pos = a.pos;
        inv.len = a.len;
        inv.text = txt;
        ustack_push(inverse_target, inv);
        uaction_free(&a);
        return;
    }

    if (a.type == UNDO_INSERT) {
        editor_insert_raw(a.pos, a.text ? a.text : "", a.len);
        ed.cursor = a.pos + a.len;
        ed.sel_bound = -1;
        update_pref_col();

        UndoAction inv;
        inv.type = UNDO_DELETE;
        inv.pos = a.pos;
        inv.len = a.len;
        inv.text = 0;
        ustack_push(inverse_target, inv);
        uaction_free(&a);
        return;
    }

    uaction_free(&a);
}

void editor_undo(void) {
    UndoAction a = ustack_pop(&ed.undo);
    if (!a.type) return;
    editor_apply_action(a, &ed.redo);
}

void editor_redo(void) {
    UndoAction a = ustack_pop(&ed.redo);
    if (!a.type) return;
    editor_apply_action(a, &ed.undo);
}

int editor_find_next_from(int start) {
    if (ed.find_len <= 0) return 0;
    int pos = gb_find_forward(&ed.text, start, ed.find, ed.find_len);
    if (pos < 0 && start > 0) pos = gb_find_forward(&ed.text, 0, ed.find, ed.find_len);
    if (pos < 0) return 0;
    status_set(0);
    ed.sel_bound = pos;
    ed.cursor = pos + ed.find_len;
    update_pref_col();
    return 1;
}

static void mini_set(const char* s, int len) {
    if (len < 0) len = 0;
    if (len > (int)sizeof(ed.mini) - 1) len = (int)sizeof(ed.mini) - 1;
    for (int i = 0; i < len; i++) ed.mini[i] = s[i];
    ed.mini[len] = 0;
    ed.mini_len = len;
}

static void mini_clear(void) {
    ed.mini[0] = 0;
    ed.mini_len = 0;
}

void mini_backspace(void) {
    if (ed.mini_len <= 0) return;
    ed.mini_len--;
    ed.mini[ed.mini_len] = 0;
    ed.open_confirm = 0;
}

void mini_putc(char c) {
    if (ed.mini_len >= (int)sizeof(ed.mini) - 1) return;
    ed.mini[ed.mini_len++] = c;
    ed.mini[ed.mini_len] = 0;
    ed.open_confirm = 0;
}

void enter_find_mode(void) {
    ed.mode = MODE_FIND;
    if (ed.find_len > 0) mini_set(ed.find, ed.find_len);
    else mini_clear();
}

void enter_goto_mode(void) {
    ed.mode = MODE_GOTO;
    mini_clear();
}

void enter_open_mode(void) {
    ed.mode = MODE_OPEN;
    int n = (int)strlen(ed.filename);
    if (n > 0) mini_set(ed.filename, n);
    else mini_clear();
    ed.open_confirm = 0;
}

void apply_find_mode(void) {
    ed.find_len = ed.mini_len;
    if (ed.find_len > (int)sizeof(ed.find) - 1) ed.find_len = (int)sizeof(ed.find) - 1;
    for (int i = 0; i < ed.find_len; i++) ed.find[i] = ed.mini[i];
    ed.find[ed.find_len] = 0;

    ed.mode = MODE_EDIT;
    if (ed.find_len > 0) {
        int start = ed.cursor;
        int text_len = gb_len(&ed.text);
        if (start < 0) start = 0;
        if (start > text_len) start = text_len;
        if (!editor_find_next_from(start)) status_set_col("Not found", C_UI_ERROR);
    }
}

void apply_goto_mode(void) {
    int line = atoi(ed.mini);
    ed.mode = MODE_EDIT;

    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    if (ed.lines.count <= 0) return;

    if (line < 1) line = 1;
    if (line > ed.lines.count) line = ed.lines.count;

    ed.cursor = ed.lines.starts[line - 1];
    ed.sel_bound = -1;
    update_pref_col();
}

void apply_open_mode(void) {
    char path[256];
    int n = ed.mini_len;
    if (n < 0) n = 0;
    if (n >= (int)sizeof(path)) n = (int)sizeof(path) - 1;
    for (int i = 0; i < n; i++) path[i] = ed.mini[i];
    path[n] = 0;

    char* p = path;
    while (*p == ' ' || *p == '\t') p++;
    int pn = (int)strlen(p);
    while (pn > 0 && (p[pn - 1] == ' ' || p[pn - 1] == '\t')) {
        p[pn - 1] = 0;
        pn--;
    }
    if (p[0] == 0) {
        status_set_col("Empty path", C_UI_ERROR);
        ed.open_confirm = 0;
        return;
    }

    if (ed.dirty && !ed.open_confirm) {
        status_set_col("Unsaved changes: press Enter again", C_UI_ERROR);
        ed.open_confirm = 1;
        return;
    }

    char old_filename[256];
    int old_fn_len = (int)strlen(ed.filename);
    if (old_fn_len < 0) old_fn_len = 0;
    if (old_fn_len >= (int)sizeof(old_filename)) old_fn_len = (int)sizeof(old_filename) - 1;
    for (int i = 0; i < old_fn_len; i++) old_filename[i] = ed.filename[i];
    old_filename[old_fn_len] = 0;
    const int old_lang = ed.lang;

    if (!editor_set_filename(p)) {
        ed.open_confirm = 0;
        return;
    }
    editor_update_lang_from_filename();

    int ok = load_file();
    if (ok) {
        ed.mode = MODE_EDIT;
        ed.open_confirm = 0;
        return;
    }

    (void)editor_set_filename(old_filename);
    ed.lang = old_lang;
    ed.mode = MODE_OPEN;
    ed.open_confirm = 0;
}

static int editor_insert_with_undo_at(int pos, const char* s, int len) {
    if (len <= 0) return 1;
    ustack_reset(&ed.redo);
    UndoAction ua;
    ua.type = UNDO_DELETE;
    ua.pos = pos;
    ua.len = len;
    ua.text = 0;
    if (!ustack_push(&ed.undo, ua)) return 0;
    editor_insert_raw(pos, s, len);
    return 1;
}

static int count_line_indent_spaces(int line_start) {
    int text_len = gb_len(&ed.text);
    int i = line_start;
    int spaces = 0;
    while (i < text_len) {
        char c = gb_char_at(&ed.text, i);
        if (c == ' ') { spaces++; i++; }
        else if (c == '\t') { spaces += 4; i++; }
        else break;
        if (spaces > 60) { spaces = 60; break; }
    }
    return spaces;
}

static int is_asm_label_line(int line_start) {
    int text_len = gb_len(&ed.text);
    int line_len = get_line_len(line_start);
    int line_end = line_start + line_len;
    if (line_end > text_len) line_end = text_len;

    int i = line_start;
    while (i < line_end && (gb_char_at(&ed.text, i) == ' ' || gb_char_at(&ed.text, i) == '\t')) i++;
    if (i >= line_end) return 0;

    int start = i;
    while (i < line_end) {
        char c = gb_char_at(&ed.text, i);
        if (c == ';') break;
        if (c == ':') {
            return i > start;
        }
        if (c == ' ' || c == '\t') return 0;
        if (!is_word_char(c)) return 0;
        i++;
    }
    return 0;
}

static int last_nonspace_before_in_line(int line_start, int pos) {
    int i = pos - 1;
    while (i >= line_start) {
        char c = gb_char_at(&ed.text, i);
        if (c != ' ' && c != '\t' && c != '\r') return (unsigned char)c;
        i--;
    }
    return 0;
}

static int next_nonspace_after_in_line(int pos, int line_end) {
    int i = pos;
    while (i < line_end) {
        char c = gb_char_at(&ed.text, i);
        if (c != ' ' && c != '\t' && c != '\r') return (unsigned char)c;
        i++;
    }
    return 0;
}

void editor_insert_newline_autoindent(void) {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    }

    int line_start = get_line_start(ed.cursor);
    int indent = count_line_indent_spaces(line_start);

    int line_len = get_line_len(line_start);
    int line_end = line_start + line_len;

    if (ed.lang == LANG_C) {
        int last = last_nonspace_before_in_line(line_start, ed.cursor);
        if (last == '{') {
            int inner = indent + 4;
            if (inner > 60) inner = 60;

            int next = next_nonspace_after_in_line(ed.cursor, line_end);
            if (next == '}') {
                char buf[2 + 2 * 64];
                int n = 0;
                buf[n++] = '\n';
                for (int i = 0; i < inner; i++) buf[n++] = ' ';
                buf[n++] = '\n';
                for (int i = 0; i < indent; i++) buf[n++] = ' ';

                if (!editor_insert_with_undo_at(ed.cursor, buf, n)) return;
                ed.cursor += 1 + inner;
                ed.sel_bound = -1;
                update_pref_col();
                return;
            }

            indent = inner;
        } else {
            int next = next_nonspace_after_in_line(ed.cursor, line_end);
            if (next == '}') {
                int inner = indent + 4;
                if (inner > 60) inner = 60;
                char buf[2 + 2 * 64];
                int n = 0;
                buf[n++] = '\n';
                for (int i = 0; i < inner; i++) buf[n++] = ' ';
                buf[n++] = '\n';
                for (int i = 0; i < indent; i++) buf[n++] = ' ';

                if (!editor_insert_with_undo_at(ed.cursor, buf, n)) return;
                ed.cursor += 1 + inner;
                ed.sel_bound = -1;
                update_pref_col();
                return;
            }
        }
    } else {
        if (is_asm_label_line(line_start)) indent = 4;
    }

    if (indent < 0) indent = 0;
    if (indent > 60) indent = 60;

    char buf[1 + 64];
    buf[0] = '\n';
    for (int i = 0; i < indent; i++) buf[1 + i] = ' ';
    int len = 1 + indent;

    if (!editor_insert_with_undo_at(ed.cursor, buf, len)) return;
    ed.cursor += len;
    ed.sel_bound = -1;
    update_pref_col();
}

void editor_insert_tab_smart(void) {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    }

    int line_start = get_line_start(ed.cursor);
    int col = ed.cursor - line_start;
    if (col < 0) col = 0;
    int add = 4 - (col % 4);
    if (add <= 0) add = 4;
    if (add > 16) add = 4;

    char spaces[16];
    for (int i = 0; i < add; i++) spaces[i] = ' ';
    editor_insert_with_undo_at(ed.cursor, spaces, add);
    ed.cursor += add;
    ed.sel_bound = -1;
    update_pref_col();
}

void delete_range(int start, int end) {
    if (start >= end) return;

    int len = end - start;
    char* deleted = gb_copy_range(&ed.text, start, end);
    if (!deleted) return;

    ustack_reset(&ed.redo);
    UndoAction ua;
    ua.type = UNDO_INSERT;
    ua.pos = start;
    ua.len = len;
    ua.text = deleted;
    if (!ustack_push(&ed.undo, ua)) return;

    editor_delete_raw(start, end);

    ed.cursor = start;
    ed.sel_bound = -1;
    update_pref_col();
}

void insert_str(const char* s, int len) {
    if (ed.sel_bound != -1) {
        int mn = min(ed.sel_bound, ed.cursor);
        int mx = max(ed.sel_bound, ed.cursor);
        delete_range(mn, mx);
    }

    if (!editor_insert_with_undo_at(ed.cursor, s, len)) return;
    ed.cursor += len;
    update_pref_col();
}

void insert_char(char c) {
    char buf[1] = {c};
    insert_str(buf, 1);
}

void backspace() {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    } else if (ed.cursor > 0) {
        delete_range(ed.cursor - 1, ed.cursor);
    }
}

void copy_selection() {
    if (ed.sel_bound == -1 || ed.sel_bound == ed.cursor) return;

    int s = min(ed.sel_bound, ed.cursor);
    int e = max(ed.sel_bound, ed.cursor);

    char* tmp = gb_copy_range(&ed.text, s, e);
    if (!tmp) return;

    clipboard_copy(tmp);
    free(tmp);
}

void paste_clipboard() {
    char* buf = malloc(4096);
    if (!buf) return;
    int len = clipboard_paste(buf, 4096);
    if (len > 0) {
        insert_str(buf, len);
    }
    free(buf);
}

void handle_selection(int select) {
    if (select) {
        if (ed.sel_bound == -1) ed.sel_bound = ed.cursor;
    } else {
        ed.sel_bound = -1;
    }
}

void move_left(int select) {
    handle_selection(select);
    if (ed.cursor > 0) ed.cursor--;
    update_pref_col();
}

void move_right(int select) {
    handle_selection(select);
    int len = gb_len(&ed.text);
    if (ed.cursor < len) ed.cursor++;
    update_pref_col();
}

void move_up(int select) {
    handle_selection(select);
    int curr_start = get_line_start(ed.cursor);
    if (curr_start == 0) {
        ed.cursor = 0;
    } else {
        int prev_start = get_line_start(curr_start - 1);
        int prev_len = get_line_len(prev_start);
        int target = min(prev_len, ed.pref_col);
        ed.cursor = prev_start + target;
    }
}

void move_down(int select) {
    handle_selection(select);
    int curr_start = get_line_start(ed.cursor);
    int curr_len = get_line_len(curr_start);
    int next_start = curr_start + curr_len + 1;
    int len = gb_len(&ed.text);
    if (next_start < len) {
        int next_len = get_line_len(next_start);
        int target = min(next_len, ed.pref_col);
        ed.cursor = next_start + target;
    } else {
        ed.cursor = len;
    }
}

void move_word_left(int select) {
    handle_selection(select);
    if (ed.cursor == 0) return;
    ed.cursor--;
    while (ed.cursor > 0 && !is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor--;
    while (ed.cursor > 0 && is_word_char(gb_char_at(&ed.text, ed.cursor - 1))) ed.cursor--;
    update_pref_col();
}

void move_word_right(int select) {
    handle_selection(select);
    int len = gb_len(&ed.text);
    if (ed.cursor >= len) return;
    while (ed.cursor < len && is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor++;
    while (ed.cursor < len && !is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor++;
    update_pref_col();
}

static int ext_eq_ci(const char* ext, const char* lit) {
    if (!ext || !lit) return 0;
    int i = 0;
    while (ext[i] && lit[i]) {
        if (lower_char(ext[i]) != lower_char(lit[i])) return 0;
        i++;
    }
    return ext[i] == 0 && lit[i] == 0;
}

void editor_update_lang_from_filename(void) {
    ed.lang = LANG_ASM;
    const char* ext = path_ext(ed.filename);
    if (ext) {
        if (ext_eq_ci(ext, "c") || ext_eq_ci(ext, "h")) {
            ed.lang = LANG_C;
        } else if (ext_eq_ci(ext, "asm") || ext_eq_ci(ext, "s") || ext_eq_ci(ext, "inc")) {
            ed.lang = LANG_ASM;
        }
    }
}

int editor_set_filename(const char* path) {
    if (!path) return 0;
    int n = (int)strlen(path);
    if (n <= 0) return 0;
    if (n >= (int)sizeof(ed.filename)) {
        status_set_col("Path too long", C_UI_ERROR);
        return 0;
    }
    for (int i = 0; i < n; i++) ed.filename[i] = path[i];
    ed.filename[n] = 0;
    return 1;
}

void status_set(const char* s) {
    if (!s) { ed.status[0] = 0; ed.status_len = 0; ed.status_color = C_UI_MUTED; return; }
    int n = (int)strlen(s);
    if (n > (int)sizeof(ed.status) - 1) n = (int)sizeof(ed.status) - 1;
    for (int i = 0; i < n; i++) ed.status[i] = s[i];
    ed.status[n] = 0;
    ed.status_len = n;
    ed.status_color = C_UI_MUTED;
}

void status_set_col(const char* s, uint32_t col) {
    status_set(s);
    ed.status_color = col;
}

int get_pos_from_coords(int mx, int my) {
    int row = (my - TAB_H) / LINE_H;
    int target_line = ed.scroll_y + row;

    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int text_len = gb_len(&ed.text);
    if (target_line < 0) target_line = 0;
    if (target_line >= ed.lines.count) return text_len;

    int line_start = ed.lines.starts[target_line];
    int line_end = (target_line + 1 < ed.lines.count) ? (ed.lines.starts[target_line + 1] - 1) : text_len;
    if (line_end < line_start) line_end = line_start;
    int len = line_end - line_start;

    int click_x = mx - (GUTTER_W + PAD_X);
    int col = (click_x + (CHAR_W/2)) / CHAR_W;
    if (col < 0) col = 0;
    if (col > len) col = len;
    return line_start + col;
}
