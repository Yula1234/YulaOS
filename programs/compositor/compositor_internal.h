#pragma once

#include <yula.h>
#include <font.h>
#include <comp_ipc.h>

extern volatile int g_should_exit;
extern volatile int g_fb_released;

extern uint32_t g_commit_gen;

extern int g_screen_w;
extern int g_screen_h;

void dbg_write(const char* s);
int pipe_try_write_frame(int fd, const void* buf, uint32_t size, int essential);

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
} comp_rect_t;

#define COMP_MAX_DAMAGE_RECTS 32

typedef struct {
    comp_rect_t rects[COMP_MAX_DAMAGE_RECTS];
    int n;
} comp_damage_t;

int rect_empty(const comp_rect_t* r);
comp_rect_t rect_make(int x, int y, int w, int h);
comp_rect_t rect_intersect(comp_rect_t a, comp_rect_t b);
comp_rect_t rect_union(comp_rect_t a, comp_rect_t b);
int rect_overlaps_or_touches(comp_rect_t a, comp_rect_t b);
comp_rect_t rect_clip_to_screen(comp_rect_t r, int w, int h);

void damage_reset(comp_damage_t* d);
void damage_add(comp_damage_t* d, comp_rect_t r, int w, int h);

void fill_rect(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, uint32_t color);

void blit_surface_clipped(uint32_t* dst, int dst_stride, int dst_w, int dst_h, int dx, int dy,
                          const uint32_t* src, int src_stride, int src_w, int src_h, comp_rect_t clip);

void present_damage_to_fb(uint32_t* fb, const uint32_t* src, int stride, comp_damage_t* dmg);

void draw_cursor_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, comp_rect_t clip);
void comp_cursor_restore(uint32_t* fb, int stride, int w, int h);
void comp_cursor_save_under_draw(uint32_t* fb, int stride, int w, int h, int x, int y);

void draw_text(uint32_t* fb, int stride, int w, int h, int x, int y, const char* s, uint32_t color);

#define COMP_CURSOR_SAVE_W 17
#define COMP_CURSOR_SAVE_H 17
#define COMP_CURSOR_SAVE_HALF 8

void draw_frame_rect_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, int t, uint32_t color, comp_rect_t clip);

typedef struct {
    uint8_t buf[4096];
    uint32_t r;
    uint32_t w;
} ipc_rx_ring_t;

uint32_t ipc_rx_count(const ipc_rx_ring_t* q);
void ipc_rx_reset(ipc_rx_ring_t* q);
void ipc_rx_push(ipc_rx_ring_t* q, const uint8_t* src, uint32_t n);
void ipc_rx_peek(const ipc_rx_ring_t* q, uint32_t off, void* dst, uint32_t n);
void ipc_rx_drop(ipc_rx_ring_t* q, uint32_t n);

#define COMP_MAX_SURFACES 8
#define COMP_CLIENTS_INIT 8

#define COMP_SURFACE_SHADOW_BUFS 2

typedef struct {
    int shm_fd;
    uint32_t* pixels;
    uint32_t size_bytes;
    int w;
    int h;
    int stride;
} comp_buffer_t;

typedef struct {
    int in_use;
    uint32_t id;
    int attached;
    int committed;
    uint32_t commit_gen;

    uint32_t z;

    int x;
    int y;

    uint32_t* pixels;
    int w;
    int h;
    int stride;

    uint32_t* shadow_pixels[COMP_SURFACE_SHADOW_BUFS];
    int shadow_stride;
    uint32_t shadow_size_bytes;
    int shadow_shm_fd[COMP_SURFACE_SHADOW_BUFS];
    int shadow_active;
    int shadow_valid;

    int owns_buffer;
    int shm_fd;
    uint32_t size_bytes;
    char shm_name[32];
} comp_surface_t;

typedef struct {
    int connected;
    int pid;
    int fd_c2s;
    int fd_s2c;
    ipc_rx_ring_t rx;

    int input_ring_shm_fd;
    uint32_t input_ring_size_bytes;
    char input_ring_shm_name[32];
    comp_input_ring_t* input_ring;
    int input_ring_enabled;

    uint32_t input_ring_mouse_seq;
    int input_ring_mouse_seq_valid;
    int input_ring_mouse_pending;
    comp_ipc_input_t input_ring_mouse_pending_ev;

    uint32_t focus_surface_id;
    uint32_t pointer_grab_surface_id;
    int pointer_grab_active;
    uint32_t prev_buttons;

    uint32_t last_mx;
    uint32_t last_my;
    uint32_t last_mb;

    uint32_t last_input_surface_id;
    uint32_t seq_out;

    uint32_t z_counter;

    comp_surface_t surfaces[COMP_MAX_SURFACES];
} comp_client_t;

typedef struct {
    int focus_client;
    uint32_t focus_surface_id;

    int grab_active;
    int grab_client;
    uint32_t grab_surface_id;

    int wm_pointer_grab_active;
    int wm_pointer_grab_client;
    uint32_t wm_pointer_grab_surface_id;

    int wm_keyboard_grab_active;

    uint32_t prev_buttons;

    uint32_t wm_last_mx;
    uint32_t wm_last_my;
    uint32_t wm_last_mb;
    int wm_last_client;
    uint32_t wm_last_surface_id;

    uint32_t last_mx;
    uint32_t last_my;
    uint32_t last_mb;
    int last_client;
    uint32_t last_surface_id;
} comp_input_state_t;

typedef struct {
    int active;
    uint32_t client_id;
    uint32_t surface_id;
    int32_t w;
    int32_t h;
} comp_preview_t;

typedef struct {
    int connected;
    int fd_c2s;
    int fd_s2c;
    ipc_rx_ring_t rx;
    uint32_t seq_out;

    uint32_t tx_r;
    uint32_t tx_w;
    struct {
        uint32_t len;
        uint32_t off;
        uint8_t frame[sizeof(comp_ipc_hdr_t) + sizeof(comp_ipc_wm_event_t)];
    } tx[128];
} wm_conn_t;

void wm_disconnect(wm_conn_t* w);
void wm_init(wm_conn_t* w, int fd_c2s, int fd_s2c);
int wm_send_event(wm_conn_t* w, const comp_ipc_wm_event_t* ev, int essential);
void wm_flush_tx(wm_conn_t* w);
void wm_replay_state(wm_conn_t* wm, const comp_client_t* clients, int nclients);
void wm_pump(wm_conn_t* w, comp_client_t* clients, int nclients, comp_input_state_t* input, uint32_t* z_counter, comp_preview_t* preview, int* preview_dirty, int* scene_dirty);

void comp_buffer_destroy(comp_buffer_t* b);
void comp_client_disconnect(comp_client_t* c);
comp_surface_t* comp_client_surface_get(comp_client_t* c, uint32_t id, int create);
void comp_client_init(comp_client_t* c, int pid, int fd_c2s, int fd_s2c);
comp_surface_t* comp_client_surface_find(const comp_client_t* c, uint32_t id);
int comp_client_surface_id_valid(const comp_client_t* c, uint32_t id);
int comp_pick_surface_at(comp_client_t* clients, int nclients, int x, int y, int* out_client, uint32_t* out_sid, comp_surface_t** out_s);

void comp_input_state_init(comp_input_state_t* st);
void comp_send_wm_pointer(wm_conn_t* wm, comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms);
void comp_update_focus(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms, uint32_t* z_counter, wm_conn_t* wm);
int comp_send_mouse(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms);
int comp_send_key(comp_client_t* clients, int nclients, comp_input_state_t* st, uint32_t keycode, uint32_t key_state);
int comp_client_send_input(comp_client_t* c, const comp_ipc_input_t* in, int essential);

void comp_client_pump(comp_client_t* c,
                       const comp_buffer_t* buf,
                       uint32_t* z_counter,
                       wm_conn_t* wm,
                       uint32_t client_id,
                       comp_input_state_t* input);
