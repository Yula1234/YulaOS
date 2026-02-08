#pragma once

#include "geditor_defs.h"

void uaction_free(UndoAction* a);
void ustack_init(UndoStack* st);
void ustack_reset(UndoStack* st);
void ustack_destroy(UndoStack* st);
int ustack_push(UndoStack* st, UndoAction a);
UndoAction ustack_pop(UndoStack* st);
