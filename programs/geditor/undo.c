// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "undo.h"

void uaction_free(UndoAction* a) {
    if (a->text) {
        free(a->text);
    }
    a->text = 0;
    a->len = 0;
    a->pos = 0;
    a->type = 0;
}

void ustack_init(UndoStack* st) {
    st->items = 0;
    st->count = 0;
    st->cap = 0;
}

void ustack_reset(UndoStack* st) {
    for (int i = 0; i < st->count; i++) {
        uaction_free(&st->items[i]);
    }
    st->count = 0;
}

void ustack_destroy(UndoStack* st) {
    ustack_reset(st);
    if (st->items) {
        free(st->items);
    }
    st->items = 0;
    st->cap = 0;
}

int ustack_push(UndoStack* st, UndoAction a) {
    if (st->count >= st->cap) {
        int new_cap = st->cap ? st->cap * 2 : 64;
        UndoAction* ni = (UndoAction*)realloc(st->items, (size_t)new_cap * sizeof(UndoAction));
        if (!ni) {
            uaction_free(&a);
            return 0;
        }
        st->items = ni;
        st->cap = new_cap;
    }
    st->items[st->count] = a;
    st->count++;
    return 1;
}

UndoAction ustack_pop(UndoStack* st) {
    UndoAction a;
    a.type = 0;
    a.pos = 0;
    a.len = 0;
    a.text = 0;

    if (st->count <= 0) {
        return a;
    }

    st->count--;
    a = st->items[st->count];
    st->items[st->count].text = 0;
    return a;
}
