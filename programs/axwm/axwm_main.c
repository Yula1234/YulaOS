#include "axwm_internal.h"

static volatile int g_should_exit;

static void on_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
    sigreturn();
    for (;;) {
    }
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(2, (void*)on_signal);
    signal(15, (void*)on_signal);

    comp_conn_t c;
    comp_conn_reset(&c);

    wm_state_t st;
    memset(&st, 0, sizeof(st));
    st.active_ws = 0;
    st.focused_idx = -1;
    st.screen_w = 0;
    st.screen_h = 0;
    st.have_screen = 0;
    st.gap_outer = 10;
    st.gap_inner = 10;
    st.float_step = 20;
    st.super_down = 0;
    st.pointer_buttons = 0;
    st.pointer_x = 0;
    st.pointer_y = 0;
    st.drag_active = 0;
    st.drag_view_idx = -1;
    st.drag_off_x = 0;
    st.drag_off_y = 0;
    st.drag_button_mask = 0;
    st.drag_requires_super = 0;

    st.ui.client_id = COMP_WM_CLIENT_NONE;
    st.ui.surface_id = WM_UI_BAR_SURFACE_ID;
    st.ui.shm_fd = -1;

    while (!g_should_exit) {
        if (!c.connected) {
            if (comp_wm_connect(&c) == 0) {
                dbg_write("axwm: connected\n");
                wm_reset_session_state(&st);
            } else {
                comp_wait_events(&c, 100000u);
                continue;
            }
        }

        if (!st.ui.connected) {
            if (wm_ui_init(&st) != 0) {
                comp_wait_events(&c, 100000u);
            }
        }

        if (st.ui.connected) {
            wm_ui_pump(&st.ui);
        }

        wm_flush_pending_cmds(&c, &st);

        comp_ipc_hdr_t hdr;
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        int r = comp_try_recv(&c, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) {
            dbg_write("axwm: disconnected\n");
            comp_disconnect(&c);
            wm_reset_session_state(&st);
            comp_wait_events(&c, 100000u);
            continue;
        }
        if (r == 0) {
            if (st.ui.connected) {
                wm_ui_pump(&st.ui);
            }
            wm_flush_pending_cmds(&c, &st);
            comp_wait_events(&c, 1000u);
            continue;
        }

        if (hdr.type == (uint16_t)COMP_IPC_MSG_WM_EVENT && hdr.len == (uint32_t)sizeof(comp_ipc_wm_event_t)) {
            comp_ipc_wm_event_t ev;
            memcpy(&ev, payload, sizeof(ev));
            (void)wm_handle_event(&c, &st, &ev);
        }
    }

    wm_ui_cleanup(&st.ui);
    comp_disconnect(&c);
    return 0;
}
