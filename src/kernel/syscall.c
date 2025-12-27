#include <arch/i386/paging.h>
#include <kernel/window.h>
#include <lib/string.h>

#include <drivers/vga.h>
#include <drivers/keyboard.h>

#include <fs/yulafs.h>
#include <fs/pipe.h>

#include <mm/pmm.h>
#include <mm/heap.h>

#include "clipboard.h"
#include "syscall.h"
#include "sched.h"
#include "proc.h"

extern volatile uint32_t timer_ticks;

extern uint32_t* paging_get_dir(void); 

static int check_user_buffer(task_t* task, const void* buf, uint32_t size) {
    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;

    if (end > 0xC0000000) return 0; 
    
    if (size == 0) return 1;
    
    volatile char* p = (volatile char*)buf;
    
    volatile char touch;
    
    touch = p[0];       
    touch = p[size - 1];
    
    (void)touch;
    (void)task;

    return 1;
}
#define MAX_TASKS 32

#define MAP_SHARED  1
#define MAP_PRIVATE 2

typedef struct {
    uint32_t type;  // 1=FILE, 2=DIR
    uint32_t size;
} __attribute__((packed)) user_stat_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t block_size;
} __attribute__((packed)) user_fs_info_t;

void syscall_handler(registers_t* regs) {
    __asm__ volatile("sti"); 
    uint32_t sys_num = regs->eax;
    task_t* curr = proc_current();

    switch (sys_num) {
        case 0: // exit()
            proc_wake_up_waiters(curr->pid);
            proc_kill(curr);
            sched_yield();  
            break;
            
        case 1: // print(string)
        {
            char* s = (char*)regs->ebx;
            if (curr->terminal) {
                term_print((term_instance_t*)curr->terminal, s);
            } else {
                vga_print(s);
            }
        }
        break;

        case 2: // getpid()
            regs->eax = curr->pid;
            break;
            
        case 3: // open(path, flags)
            if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
                regs->eax = vfs_open((char*)regs->ebx, (int)regs->ecx);
            } else {
                regs->eax = -1;
            }
            break;

        case 4: // read(fd, buf, size)
            if (check_user_buffer(curr, (void*)regs->ecx, (uint32_t)regs->edx)) {
                int res = vfs_read((int)regs->ebx, (void*)regs->ecx, (uint32_t)regs->edx);
                if(res == -2)  {
                    regs->eax = -1;
                } else {
                    regs->eax = res;
                }
            } else {
                regs->eax = -1;
            }
            break;

        case 5: // write(fd, buf, size)
             if (check_user_buffer(curr, (void*)regs->ecx, (uint32_t)regs->edx)) {
                regs->eax = vfs_write((int)regs->ebx, (void*)regs->ecx, (uint32_t)regs->edx);
            } else {
                regs->eax = -1;
            }
            break;

        case 6: // close(fd)
            regs->eax = vfs_close((int)regs->ebx);
            break;
            
        case 7: // sleep(ms)
        {
            uint32_t ms = regs->ebx;
            curr->wake_tick = timer_ticks + (ms * 15); 
            curr->state = TASK_WAITING;
            sched_yield();
        }
        break;

        case 8: // sbrk(increment)
        {
            int incr = (int)regs->ebx;
            uint32_t old_brk = curr->prog_break;
            uint32_t new_brk = old_brk + incr;

            if (new_brk >= 0x80000000) { 
                regs->eax = -1; 
                break; 
            }
            
            if (new_brk < curr->heap_start) {
                regs->eax = -1;
                break;
            }

            if (incr < 0) {
                uint32_t start_free = (new_brk + 0xFFF) & ~0xFFF;
                uint32_t end_free   = (old_brk + 0xFFF) & ~0xFFF;
                
                for (uint32_t v = start_free; v < end_free; v += 4096) {
                    if (paging_is_user_accessible(curr->page_dir, v)) {
                        uint32_t phys = paging_get_phys(curr->page_dir, v);
                        pmm_free_block((void*)phys);
                    }
                }

                __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
            }
            
            curr->prog_break = new_brk;
            regs->eax = old_brk;
        }
        break;

        case 9: // kill(pid)
        {
            uint32_t target_pid = regs->ebx;
            int found = 0;
            for (uint32_t i = 0; i < MAX_TASKS; i++) {
                task_t* t = proc_task_at(i);
                if (t && t->pid == target_pid) {
                    proc_kill(t);
                    found = 1;
                    break;
                }
            }

            if (found) {
                if (target_pid == curr->pid) {
                    sched_yield();
                }
                regs->eax = 0;
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 11: // usleep(us)
        {
            uint32_t us = regs->ebx;
            uint32_t ticks = (us * 15) / 1000;
            if (ticks == 0) ticks = 1;

            curr->wake_tick = timer_ticks + ticks;
            curr->state = TASK_WAITING;
            sched_yield();
        }
        break;

        case 12: // get_mem_stats
        {
            uint32_t* u_ptr = (uint32_t*)regs->ebx;
            uint32_t* f_ptr = (uint32_t*)regs->ecx;
            
            if (check_user_buffer(curr, u_ptr, 4) && check_user_buffer(curr, f_ptr, 4)) {
                *u_ptr = pmm_get_used_blocks() * 4;
                *f_ptr = pmm_get_free_blocks() * 4;
                regs->eax = 0;
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 13: // mkdir(path)
        {
            if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
                regs->eax = yulafs_mkdir((char*)regs->ebx);
            } else regs->eax = -1;
        }
        break;

        case 14: // unlink/rm(path)
        {
            if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
                regs->eax = yulafs_unlink((char*)regs->ebx);
            } else regs->eax = -1;
        }
        break;

        case 15: // get_time(char* buf)
        {
            if (check_user_buffer(curr, (void*)regs->ebx, 9)) {
                extern void get_time_string(char* buf);
                get_time_string((char*)regs->ebx);
                regs->eax = 0;
            } else regs->eax = -1;
        }
        break;

        case 16: // reboot()
        {
            extern void kbd_reboot(void);
            kbd_reboot();
        }
        break;

        case 17: // signal(sig, handler)
        if (regs->ebx < NSIG) {
            curr->handlers[regs->ebx] = (sig_handler_t)regs->ecx;
            regs->eax = 0;
        } else regs->eax = -1;
        break;

        case 18: // sigreturn()
            memcpy(regs, &curr->signal_context, sizeof(registers_t));
            curr->is_running_signal = 0;
            break;

        case 20: // create_window(w, h, title)
        {
            int req_w = regs->ebx;
            int req_h = regs->ecx;
            char* user_title = (char*)regs->edx;

            char k_title[32];
            if (user_title) {
                strlcpy(k_title, user_title, 32);
            } else {
                strlcpy(k_title, "User Window", 32);
            }

            int total_w = req_w + 12;
            int total_h = req_h + 44;
            
            window_t* win = window_create(100, 100, total_w, total_h, k_title, 0);

            if (win) {
                int id = -1;
                for(int i=0; i<MAX_WINDOWS; i++) {
                    if (&window_list[i] == win) { id = i; break; }
                }
                regs->eax = id;
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 21: // map_window(win_id)
        {
            int id = regs->ebx;
            if (id < 0 || id >= MAX_WINDOWS || !window_list[id].is_active) {
                regs->eax = 0; break;
            }
            window_t* win = &window_list[id];
            if (win->owner_pid != (int)curr->pid) { regs->eax = 0; break; }

            uint32_t user_vaddr_start = 0xA0000000;
            
            int canvas_w = win->target_w - 12;
            int canvas_h = win->target_h - 44;
            
            uint32_t size_bytes = canvas_w * canvas_h * 4;
            uint32_t pages = (size_bytes + 0xFFF) / 4096;
            
            uint32_t kern_vaddr = (uint32_t)win->canvas;

            for (uint32_t i = 0; i < pages; i++) {
                uint32_t offset = i * 4096;
                uint32_t phys = paging_get_phys(kernel_page_directory, kern_vaddr + offset);
                if (phys) {
                    paging_map(curr->page_dir, user_vaddr_start + offset, phys, 0x207); 
                    curr->mem_pages++;
                }
            }
            __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
            regs->eax = user_vaddr_start;
        }
        break;
        
        case 22: // update_window(win_id)
        {
            int id = regs->ebx;
            if (id >= 0 && id < MAX_WINDOWS) {
                window_list[id].is_dirty = 1; 
            }
        }
        break;

        case 23: // get_event(win_id, buffer_ptr) -> returns 1 if event found, 0 if empty
        {
            int id = regs->ebx;
            yula_event_t* user_ev = (yula_event_t*)regs->ecx;
            
            if (id >= 0 && id < MAX_WINDOWS && window_list[id].is_active) {
                if (window_list[id].owner_pid == (int)curr->pid) {
                    yula_event_t k_ev;
                    if (window_pop_event(&window_list[id], &k_ev)) {
                        *user_ev = k_ev;
                        regs->eax = 1;
                    } else {
                        regs->eax = 0;
                    }
                } else regs->eax = 0;
            } else regs->eax = 0;
        }
        break;

        case 25: // set_clipboard(buf, len)
        {
            char* buf = (char*)regs->ebx;
            int len = (int)regs->ecx;
            if (check_user_buffer(curr, buf, len)) {
                regs->eax = clipboard_set(buf, len);
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 26: // get_clipboard(buf, max_len)
        {
            char* buf = (char*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (check_user_buffer(curr, buf, max_len)) {
                regs->eax = clipboard_get(buf, max_len);
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 27: // set_term_mode(int mode)
        {
            int mode = (int)regs->ebx;
            curr->term_mode = (mode == 1) ? 1 : 0;
            regs->eax = 0;
        }
        break;

        case 28: // set_console_color(fg, bg)
        {
            uint32_t fg = (uint32_t)regs->ebx;
            uint32_t bg = (uint32_t)regs->ecx;
            
            if (curr->terminal) {
                term_instance_t* term = (term_instance_t*)curr->terminal;
                term->curr_fg = fg;
                term->curr_bg = bg;
            }
            regs->eax = 0;
        }
        break;

        case 29: // pipe(int fds[2])
        {
            int* user_fds = (int*)regs->ebx;
            if (!check_user_buffer(curr, user_fds, sizeof(int) * 2)) {
                regs->eax = -1; break;
            }

            int fd_r = -1, fd_w = -1;
            for (int i = 0; i < MAX_PROCESS_FDS; i++) {
                if (!curr->fds[i].used) {
                    if (fd_r == -1) fd_r = i;
                    else { fd_w = i; break; }
                }
            }

            if (fd_r == -1 || fd_w == -1) {
                regs->eax = -1;
                break;
            }

            vfs_node_t *r_node, *w_node;
            if (vfs_create_pipe(&r_node, &w_node) != 0) {
                regs->eax = -1; break;
            }

            curr->fds[fd_r].node = r_node;
            curr->fds[fd_r].offset = 0;
            curr->fds[fd_r].used = 1;

            curr->fds[fd_w].node = w_node;
            curr->fds[fd_w].offset = 0;
            curr->fds[fd_w].used = 1;

            user_fds[0] = fd_r;
            user_fds[1] = fd_w;
            regs->eax = 0;
        }
        break;

        case 30: // dup2(oldfd, newfd)
        {
            int oldfd = (int)regs->ebx;
            int newfd = (int)regs->ecx;

            if (oldfd < 0 || oldfd >= MAX_PROCESS_FDS || !curr->fds[oldfd].used) {
                regs->eax = -1; break;
            }
            if (newfd < 0 || newfd >= MAX_PROCESS_FDS) {
                regs->eax = -1; break;
            }

            if (oldfd == newfd) {
                regs->eax = newfd; break;
            }

            if (curr->fds[newfd].used) {
                vfs_close(newfd);
            }

            curr->fds[newfd] = curr->fds[oldfd];
            
            if (curr->fds[newfd].node) {
                curr->fds[newfd].node->refs++;
            }
            
            regs->eax = newfd;
        }
        break;

        case 31: // mmap(fd, size, flags)
        {
            int fd = (int)regs->ebx;
            uint32_t size = (uint32_t)regs->ecx;
            int flags = (int)regs->edx;

            if (!(flags & MAP_PRIVATE)) {
                regs->eax = 0;
                break;
            }

            if (size == 0) { regs->eax = 0; break; }

            if (fd < 0 || fd >= MAX_PROCESS_FDS || !curr->fds[fd].used || !curr->fds[fd].node) {
                regs->eax = 0; 
                break;
            }

            uint32_t vaddr = curr->mmap_top;
            
            uint32_t size_aligned = (size + 4095) & ~4095;

            mmap_area_t* area = kmalloc(sizeof(mmap_area_t));
            if (!area) { regs->eax = 0; break; }

            area->vaddr_start = vaddr;
            area->vaddr_end = vaddr + size_aligned;
            area->file_offset = 0;
            area->length = size;
            area->file = curr->fds[fd].node;
            area->file_size = size;
            
            area->file->refs++;

            area->next = curr->mmap_list;
            curr->mmap_list = area;

            curr->mmap_top += size_aligned;

            regs->eax = vaddr;
        }
        break;

        case 32: // munmap(addr, length)
        {
            uint32_t vaddr = regs->ebx;
            uint32_t len = regs->ecx;
            
            if (vaddr & 0xFFF) { regs->eax = -1; break; } 
            if (len == 0) { regs->eax = -1; break; }
            if (vaddr + len < vaddr) { regs->eax = -1; break; } // Overflow

            uint32_t aligned_len = (len + 4095) & ~4095;
            uint32_t vaddr_end = vaddr + aligned_len;

            for (uint32_t i = 0; i < aligned_len; i += 4096) {
                uint32_t curr_v = vaddr + i;
                
                if (paging_is_user_accessible(curr->page_dir, curr_v)) {
                    uint32_t phys = paging_get_phys(curr->page_dir, curr_v);
                    
                    if (phys) {
                        pmm_free_block((void*)phys);
                        if (curr->mem_pages > 0) curr->mem_pages--;
                    }

                    paging_map(curr->page_dir, curr_v, 0, 0); // Unmap PTE
                    __asm__ volatile("invlpg (%0)" :: "r"(curr_v) : "memory");
                }
            }

            mmap_area_t* prev = 0;
            mmap_area_t* m = curr->mmap_list;

            while (m) {
                mmap_area_t* next_node = m->next;

                uint32_t u_start = vaddr;
                uint32_t u_end   = vaddr_end;
                uint32_t m_start = m->vaddr_start;
                uint32_t m_end   = m->vaddr_end;

                if (u_end <= m_start || u_start >= m_end) {
                    prev = m;
                    m = next_node;
                    continue;
                }

                if (u_start <= m_start && u_end >= m_end) {
                    if (prev) prev->next = next_node;
                    else curr->mmap_list = next_node;

                    if (m->file) m->file->refs--;
                    kfree(m);
                    
                    m = next_node;
                    continue;
                }

                if (u_start > m_start && u_end < m_end) {
                    mmap_area_t* new_right = (mmap_area_t*)kmalloc(sizeof(mmap_area_t));
                    if (!new_right) {
                        regs->eax = -1; 
                        break; 
                    }

                    new_right->vaddr_start = u_end;
                    new_right->vaddr_end   = m_end;
                    new_right->length      = m_end - u_end;
                    new_right->file        = m->file;
                    new_right->file_size   = m->file_size;
                    
                    if (m->file) {
                        new_right->file_offset = m->file_offset + (u_end - m_start);
                        m->file->refs++;
                    } else {
                        new_right->file_offset = 0;
                    }

                    new_right->next = m->next;
                    m->next = new_right;

                    m->vaddr_end = u_start;
                    m->length    = u_start - m_start;
                    
                    prev = new_right; 
                    m = new_right->next;
                    continue;
                }

                if (u_start <= m_start && u_end < m_end) {
                    uint32_t cut_len = u_end - m_start;
                    m->vaddr_start = u_end;
                    m->length -= cut_len;
                    m->file_offset += cut_len;
                    
                    prev = m;
                    m = next_node;
                    continue;
                }

                if (u_start > m_start && u_end >= m_end) {
                    m->vaddr_end = u_start;
                    m->length = u_start - m_start;
                    
                    prev = m;
                    m = next_node;
                    continue;
                }
                
                prev = m;
                m = next_node;
            }
            regs->eax = 0;
        }
        break;

        case 33: // stat(const char* path, stat_t* buf)
        {
            char* path = (char*)regs->ebx;
            user_stat_t* u_stat = (user_stat_t*)regs->ecx;
            
            if (!check_user_buffer(curr, path, 1) || !check_user_buffer(curr, u_stat, sizeof(user_stat_t))) {
                regs->eax = -1;
                break;
            }

            int inode_idx = yulafs_lookup(path);
            if (inode_idx < 0) {
                regs->eax = -1;
                break;
            }

            yfs_inode_t k_inode;
            if (yulafs_stat(inode_idx, &k_inode) != 0) {
                regs->eax = -1;
                break;
            }

            u_stat->type = k_inode.type;
            u_stat->size = k_inode.size;
            
            regs->eax = 0;
        }
        break;

        case 34: // get_fs_info(fs_info_t* buf)
        {
            user_fs_info_t* u_info = (user_fs_info_t*)regs->ebx;

            if (!check_user_buffer(curr, u_info, sizeof(user_fs_info_t))) {
                regs->eax = -1;
                break;
            }

            uint32_t t, f, b;
            yulafs_get_filesystem_info(&t, &f, &b);

            u_info->total_blocks = t;
            u_info->free_blocks = f;
            u_info->block_size = b;

            regs->eax = 0;
        }
        break;


        default:
            vga_print("Unknown syscall\n");
            break;
    }
}