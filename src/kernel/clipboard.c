#include <lib/string.h>
#include <hal/lock.h>

#include "clipboard.h"

#define CLIPBOARD_SIZE 4096

static char clipboard_data[CLIPBOARD_SIZE];
static int clipboard_len = 0;
static spinlock_t clip_lock;

void clipboard_init() {
    spinlock_init(&clip_lock);
    memset(clipboard_data, 0, CLIPBOARD_SIZE);
    clipboard_len = 0;
}

int clipboard_set(const char* data, int len) {
    if (len > CLIPBOARD_SIZE - 1) len = CLIPBOARD_SIZE - 1;
    
    uint32_t flags = spinlock_acquire_safe(&clip_lock);
    
    memcpy(clipboard_data, data, len);
    clipboard_data[len] = '\0';
    clipboard_len = len;
    
    spinlock_release_safe(&clip_lock, flags);
    return len;
}

int clipboard_get(char* buf, int max_len) {
    uint32_t flags = spinlock_acquire_safe(&clip_lock);
    
    int to_copy = clipboard_len;
    if (to_copy > max_len - 1) to_copy = max_len - 1;
    
    memcpy(buf, clipboard_data, to_copy);
    buf[to_copy] = '\0';
    
    spinlock_release_safe(&clip_lock, flags);
    return to_copy;
}