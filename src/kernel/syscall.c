#include "syscall.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../lib/string.h"
#include "proc.h"
#include "sched.h"
#include "../arch/i386/paging.h"
#include "../mm/pmm.h"
#include "../fs/yulafs.h"
#include "../kernel/window.h"
#include "../mm/heap.h" 
#include "clipboard.h"
#include "../fs/pipe.h"

extern volatile uint32_t timer_ticks;

extern uint32_t* paging_get_dir(void); 

static int check_user_buffer(task_t* task, const void* buf, uint32_t size) {
    extern uint32_t page_dir[]; 
    if (task->page_dir == (uint32_t*)page_dir || task->page_dir == 0) return 1;

    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;
    
    if (!paging_is_user_accessible(task->page_dir, start)) return 0;
    if (!paging_is_user_accessible(task->page_dir, end - 1)) return 0;
    return 1;
}

#define MAX_TASKS 32

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
            // 1 мс теперь это 15 тиков (при 15000 Гц)
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

            if (new_brk >= 0xC0000000) { regs->eax = -1; break; }

            if (incr > 0) {
                uint32_t page_start = (old_brk + 0xFFF) & ~0xFFF;
                uint32_t page_end   = (new_brk + 0xFFF) & ~0xFFF;

                for (uint32_t v = page_start; v < page_end; v += 4096) {
                    if (!paging_is_user_accessible(curr->page_dir, v)) {
                        void* phys = pmm_alloc_block();
                        if (!phys) { regs->eax = -1; return; }
                        paging_map(curr->page_dir, v, (uint32_t)phys, 7);
                        curr->mem_pages++;
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

            uint32_t* user_pd = paging_get_dir();
            
            paging_switch(kernel_page_directory);
            
            window_t* win = window_create(100, 100, total_w, total_h, k_title, 0);
            
            paging_switch(user_pd);

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

            uint32_t user_vaddr_start = 0x40000000;
            
            int canvas_w = win->target_w - 12;
            int canvas_h = win->target_h - 44;
            
            uint32_t size_bytes = canvas_w * canvas_h * 4;
            uint32_t pages = (size_bytes + 0xFFF) / 4096;
            
            uint32_t kern_vaddr = (uint32_t)win->canvas;

            for (uint32_t i = 0; i < pages; i++) {
                uint32_t offset = i * 4096;
                uint32_t phys = paging_get_phys(kernel_page_directory, kern_vaddr + offset);
                if (phys) {
                    paging_map(curr->page_dir, user_vaddr_start + offset, phys, 7); 
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


        default:
            vga_print("Unknown syscall\n");
            break;
    }
}