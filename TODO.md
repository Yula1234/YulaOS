# Kernel GUI -> TTY Refactor

## Done

- Boot init: start `tty_task` + `shell_task` instead of `gui_task`.
- Added `src/kernel/tty.{c,h}`: terminal renderer to framebuffer via `vga_render_terminal_instance()`.
- Exposed minimal devices:
  - `/dev/fb0`
  - `/dev/mouse`
- `src/shell/shell.c`: converted to fullscreen TTY mode (no `window_t`, no `window_*`, no `wake_up_gui`).
- `src/shell/shell.c`: fixed bad `spinlock_release(&my_term->lock)` and made term interaction safe vs `tty_task` by guarding terminal mutations under `term->lock` and avoiding holding `term->lock` across `proc_wait()`.
- Removed GUI wakeups / dirty-marking from core drivers (`kbd`, `mouse`, `console`).
- Boot now does not initialize the window system (`window_init_system()` removed); window subsystem is guarded and remains dormant in TTY mode.

## Next

- Remove/disable remaining GUI-only kernel codepaths (GUI window syscalls, `monitor_task`, `gui_task`) once TTY mode is stable.
- Implement proper framebuffer mapping for user space compositor (likely `mmap` support for `MAP_SHARED` + mapping `back_buffer`/fb memory).
- Move compositor / window manager to user space.
