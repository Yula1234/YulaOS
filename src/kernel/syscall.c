// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/paging.h>
#include <kernel/window.h>
#include <lib/string.h>

#include <drivers/vga.h>
#include <drivers/keyboard.h>

#include <fs/vfs.h>
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
    if (!buf) return 0;
    if (size == 0) return 1;

    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;

    if (end < start) return 0; 
    if (start < 0x08000000 || end > 0xC0000000) return 0; 

    if (!task || !task->page_dir) return 0;

    return 1;
}

static int paging_get_present_pte(uint32_t* dir, uint32_t virt, uint32_t* out_pte) {
    if (!dir) return 0;

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = dir[pd_idx];
    if (!(pde & 1)) return 0;

    uint32_t* pt = (uint32_t*)(pde & ~0xFFF);
    uint32_t pte = pt[pt_idx];
    if (!(pte & 1)) return 0;

    if (out_pte) *out_pte = pte;
    return 1;
}

static mmap_area_t* mmap_find_area(task_t* t, uint32_t vaddr) {
    if (!t) return 0;
    mmap_area_t* m = t->mmap_list;
    while (m) {
        if (vaddr >= m->vaddr_start && vaddr < m->vaddr_end) return m;
        m = m->next;
    }
    return 0;
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

extern void wake_up_gui();

void syscall_handler(registers_t* regs) {
    __asm__ volatile("sti"); 
    uint32_t sys_num = regs->eax;
    task_t* curr = proc_current();

    switch (sys_num) {
        case 0: // exit()
            curr->exit_status = (int)regs->ebx;
            proc_kill(curr);
            sched_yield();  
            break;
            
        case 1: // print(string)
        {
            char* s = (char*)regs->ebx;
            if (curr->terminal) {
                term_print((term_instance_t*)curr->terminal, s);
            } else {
                regs->eax = -1;
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
            uint32_t target = timer_ticks + (ms * 15);
            
            extern void proc_sleep_add(task_t* t, uint32_t tick);
            proc_sleep_add(curr, target);
        }
        break;

        case 8: // sbrk(increment)
        {
            int incr = (int)regs->ebx;
            uint32_t old_brk = curr->prog_break;
            int64_t new_brk64 = (int64_t)(uint64_t)old_brk + (int64_t)incr;
            if (new_brk64 < 0 || new_brk64 >= 0x80000000ll) {
                regs->eax = -1;
                break;
            }
            uint32_t new_brk = (uint32_t)new_brk64;
            
            if (new_brk < curr->heap_start) {
                regs->eax = -1;
                break;
            }

            if (incr < 0) {
                uint32_t start_free = (new_brk + 0xFFF) & ~0xFFF;
                uint32_t end_free   = (old_brk + 0xFFF) & ~0xFFF;
                
                for (uint32_t v = start_free; v < end_free; v += 4096) {
                    uint32_t pte;
                    if (!paging_get_present_pte(curr->page_dir, v, &pte)) continue;
                    if ((pte & 4) == 0) continue;

                    paging_map(curr->page_dir, v, 0, 0);

                    uint32_t phys = pte & ~0xFFF;
                    if (curr->mem_pages > 0) curr->mem_pages--;
                    if (phys && (pte & 0x200) == 0) {
                        pmm_free_block((void*)phys);
                    }
                }
            }
            
            curr->prog_break = new_brk;
            regs->eax = old_brk;
        }
        break;

        case 9: // kill(pid)
        {
            uint32_t target_pid = regs->ebx;
            task_t* t = proc_find_by_pid(target_pid);
            if (t) {
                proc_kill(t);
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

            uint32_t target = timer_ticks + ticks;
            
            extern void proc_sleep_add(task_t* t, uint32_t tick);
            proc_sleep_add(curr, target);
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
                curr->term_mode = 0; 
                regs->eax = win->window_id;
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 21: // map_window(win_id)
        {
            int id = regs->ebx;
            window_t* win = window_find_by_id(id);
            if (!win || !win->is_active) {
                regs->eax = 0; break;
            }
            
            sem_wait(&win->lock);
            if (win->owner_pid != (int)curr->pid || !win->canvas) {
                sem_signal(&win->lock);
                regs->eax = 0; 
                break;
            }

            uint32_t user_vaddr_start = 0xA0000000;
            
            int canvas_w = win->target_w - 12;
            int canvas_h = win->target_h - 44;
            
            if (canvas_w <= 0 || canvas_h <= 0) {
                sem_signal(&win->lock);
                regs->eax = 0;
                break;
            }
            
            uint32_t size_bytes = canvas_w * canvas_h * 4;
            uint32_t pages = (size_bytes + 0xFFF) / 4096;
            
            uint32_t kern_vaddr = (uint32_t)win->canvas;

            sem_signal(&win->lock);

            if (curr->winmap_pages > 0) {
                for (uint32_t i = 0; i < curr->winmap_pages; i++) {
                    uint32_t v = user_vaddr_start + i * 4096;
                    if (paging_is_user_accessible(curr->page_dir, v)) {
                        paging_map(curr->page_dir, v, 0, 0);
                    }
                }
                if (curr->mem_pages >= curr->winmap_pages) curr->mem_pages -= curr->winmap_pages;
                else curr->mem_pages = 0;
                curr->winmap_pages = 0;
                curr->winmap_win_id = 0;
            }

            uint32_t mapped_pages = 0;

            for (uint32_t i = 0; i < pages; i++) {
                uint32_t offset = i * 4096;
                uint32_t phys = paging_get_phys(kernel_page_directory, kern_vaddr + offset);
                if (phys) {
                    paging_map(curr->page_dir, user_vaddr_start + offset, phys, 0x207);
                    curr->mem_pages++;
                    mapped_pages++;
                }
            }

            curr->winmap_pages = mapped_pages;
            curr->winmap_win_id = id;

            win = window_find_by_id(id);
            if (win && win->is_active) {
                sem_wait(&win->lock);
                if (win->owner_pid == (int)curr->pid && win->old_canvas) {
                    kfree(win->old_canvas);
                    win->old_canvas = 0;
                }
                sem_signal(&win->lock);
            }

            __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
            regs->eax = user_vaddr_start;
        }
        break;
        
        case 22: // update_window(win_id)
        {
            int id = regs->ebx;
            window_t* win = window_find_by_id(id);
            if (win && win->is_active) {
                sem_wait(&win->lock);
                if (win->is_active && win->owner_pid == (int)curr->pid) {
                    win->is_dirty = 1;
                }
                sem_signal(&win->lock);
                wake_up_gui();
            }
        }
        break;

        case 23: // get_event(win_id, buffer_ptr) -> returns 1 if event found, 0 if empty
        {
            int id = regs->ebx;
            yula_event_t* user_ev = (yula_event_t*)regs->ecx;
            
            if (!user_ev || !check_user_buffer(curr, user_ev, sizeof(yula_event_t))) {
                regs->eax = 0;
                break;
            }
            
            window_t* win = window_find_by_id(id);
            if (win && win->is_active) {
                sem_wait(&win->lock);
                int is_owner = (win->owner_pid == (int)curr->pid);
                sem_signal(&win->lock);
                
                if (is_owner) {
                    yula_event_t k_ev;
                    if (window_pop_event(win, &k_ev)) {
                        *user_ev = k_ev;
                        regs->eax = 1;
                    } else {
                        regs->eax = 0;
                    }
                } else {
                    regs->eax = 0;
                }
            } else {
                regs->eax = 0;
            }
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

            file_t* fr = 0;
            file_t* fw = 0;

            int fd_r = proc_fd_alloc(curr, &fr);
            if (fd_r < 0 || !fr) {
                regs->eax = -1;
                break;
            }

            int fd_w = proc_fd_alloc(curr, &fw);
            if (fd_w < 0 || !fw) {
                file_t tmp;
                memset(&tmp, 0, sizeof(tmp));
                proc_fd_remove(curr, fd_r, &tmp);
                regs->eax = -1;
                break;
            }

            vfs_node_t *r_node, *w_node;
            if (vfs_create_pipe(&r_node, &w_node) != 0) {
                file_t tmp;
                memset(&tmp, 0, sizeof(tmp));
                proc_fd_remove(curr, fd_r, &tmp);
                proc_fd_remove(curr, fd_w, &tmp);
                regs->eax = -1; break;
            }

            fr->node = r_node;
            fr->offset = 0;
            fr->used = 1;

            fw->node = w_node;
            fw->offset = 0;
            fw->used = 1;

            user_fds[0] = fd_r;
            user_fds[1] = fd_w;
            regs->eax = 0;
        }
        break;

        case 30: // dup2(oldfd, newfd)
        {
            int oldfd = (int)regs->ebx;
            int newfd = (int)regs->ecx;

            if (oldfd < 0 || newfd < 0) {
                regs->eax = -1; break;
            }

            file_t* of = proc_fd_get(curr, oldfd);
            if (!of || !of->used) {
                regs->eax = -1; break;
            }

            if (oldfd == newfd) {
                regs->eax = newfd; break;
            }

            if (proc_fd_get(curr, newfd)) {
                vfs_close(newfd);
            }

            file_t* nf = 0;
            if (proc_fd_add_at(curr, newfd, &nf) < 0 || !nf) {
                regs->eax = -1;
                break;
            }
            *nf = *of;
            if (nf->node) {
                __sync_fetch_and_add(&nf->node->refs, 1);
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

            file_t* f = proc_fd_get(curr, fd);
            if (fd < 0 || !f || !f->used || !f->node) {
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
            area->file = f->node;
            area->file_size = (f->node->size < size) ? f->node->size : size;

            if (vaddr + size_aligned < vaddr || vaddr + size_aligned > 0xC0000000) {
                kfree(area);
                regs->eax = 0;
                break;
            }

            __sync_fetch_and_add(&area->file->refs, 1);

            uint32_t mapped = 0;
            for (uint32_t off = 0; off < size_aligned; off += 4096) {
                void* phys_page = pmm_alloc_block();
                if (!phys_page) break;

                memset(phys_page, 0, 4096);
                if (area->file && area->file->ops && area->file->ops->read) {
                    if (off < area->file_size) {
                        uint32_t bytes = area->file_size - off;
                        if (bytes > 4096) bytes = 4096;
                        area->file->ops->read(area->file, area->file_offset + off, bytes, phys_page);
                    }
                }

                paging_map(curr->page_dir, vaddr + off, (uint32_t)phys_page, 7);
                curr->mem_pages++;
                mapped += 4096;
            }

            if (mapped != size_aligned) {
                for (uint32_t off = 0; off < mapped; off += 4096) {
                    uint32_t v = vaddr + off;
                    uint32_t pte;
                    if (!paging_get_present_pte(curr->page_dir, v, &pte)) continue;
                    if ((pte & 4) == 0) continue;

                    paging_map(curr->page_dir, v, 0, 0);

                    uint32_t phys = pte & ~0xFFF;
                    if (curr->mem_pages > 0) curr->mem_pages--;
                    if (phys && (pte & 0x200) == 0) {
                        pmm_free_block((void*)phys);
                    }
                }

                if (area->file) {
                    uint32_t new_refs = __sync_sub_and_fetch(&area->file->refs, 1);
                    if (new_refs == 0) {
                        if (area->file->ops && area->file->ops->close) area->file->ops->close(area->file);
                        else kfree(area->file);
                    }
                }
                kfree(area);
                regs->eax = 0;
                break;
            }

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

            uint32_t scan = vaddr;
            while (scan < vaddr_end) {
                mmap_area_t* a = mmap_find_area(curr, scan);
                if (!a || a->vaddr_end <= scan) {
                    regs->eax = -1;
                    break;
                }
                if (a->vaddr_end > scan) {
                    scan = a->vaddr_end;
                }
            }
            if (scan < vaddr_end) break;

            int result = 0;

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

                result = 0;

                uint32_t o_start = (u_start > m_start) ? u_start : m_start;
                uint32_t o_end   = (u_end < m_end) ? u_end : m_end;

                if (o_start > m_start && o_end < m_end) {
                    mmap_area_t* new_right = (mmap_area_t*)kmalloc(sizeof(mmap_area_t));
                    if (!new_right) {
                        result = -1;
                        break;
                    }

                    uint32_t orig_file_size = m->file_size;
                    uint32_t left_len = o_start - m_start;
                    uint32_t right_len = m_end - o_end;
                    uint32_t cut_before_right = o_end - m_start;

                    new_right->vaddr_start = o_end;
                    new_right->vaddr_end   = m_end;
                    new_right->length      = right_len;
                    new_right->file        = m->file;
                    new_right->next        = m->next;

                    if (m->file) {
                        new_right->file_offset = m->file_offset + cut_before_right;
                        __sync_fetch_and_add(&m->file->refs, 1);
                    } else {
                        new_right->file_offset = 0;
                    }

                    uint32_t right_file_size = 0;
                    if (orig_file_size > cut_before_right) right_file_size = orig_file_size - cut_before_right;
                    if (right_file_size > right_len) right_file_size = right_len;
                    new_right->file_size = right_file_size;

                    m->vaddr_end = o_start;
                    m->length = left_len;

                    uint32_t left_file_size = orig_file_size;
                    if (left_file_size > left_len) left_file_size = left_len;
                    m->file_size = left_file_size;

                    m->next = new_right;

                    for (uint32_t curr_v = o_start; curr_v < o_end; curr_v += 4096) {
                        uint32_t pte;
                        if (!paging_get_present_pte(curr->page_dir, curr_v, &pte)) continue;
                        if ((pte & 4) == 0) continue;

                        paging_map(curr->page_dir, curr_v, 0, 0);

                        uint32_t phys = pte & ~0xFFF;
                        if (curr->mem_pages > 0) curr->mem_pages--;
                        if (phys && (pte & 0x200) == 0) {
                            pmm_free_block((void*)phys);
                        }
                    }

                    prev = new_right;
                    m = new_right->next;
                    continue;
                }

                for (uint32_t curr_v = o_start; curr_v < o_end; curr_v += 4096) {
                    uint32_t pte;
                    if (!paging_get_present_pte(curr->page_dir, curr_v, &pte)) continue;
                    if ((pte & 4) == 0) continue;

                    paging_map(curr->page_dir, curr_v, 0, 0);

                    uint32_t phys = pte & ~0xFFF;
                    if (curr->mem_pages > 0) curr->mem_pages--;
                    if (phys && (pte & 0x200) == 0) {
                        pmm_free_block((void*)phys);
                    }
                }

                if (o_start == m_start && o_end == m_end) {
                    if (prev) prev->next = next_node;
                    else curr->mmap_list = next_node;

                    if (m->file) {
                        uint32_t new_refs = __sync_sub_and_fetch(&m->file->refs, 1);
                        if (new_refs == 0) {
                            if (m->file->ops && m->file->ops->close) m->file->ops->close(m->file);
                            else kfree(m->file);
                        }
                    }
                    kfree(m);

                    m = next_node;
                    continue;
                }

                if (o_start == m_start && o_end < m_end) {
                    uint32_t cut_len = o_end - m_start;
                    uint32_t new_len = m_end - o_end;

                    m->vaddr_start = o_end;
                    m->length = new_len;

                    if (m->file) m->file_offset += cut_len;

                    if (m->file_size > cut_len) m->file_size -= cut_len;
                    else m->file_size = 0;
                    if (m->file_size > new_len) m->file_size = new_len;

                    prev = m;
                    m = next_node;
                    continue;
                }

                if (o_start > m_start && o_end == m_end) {
                    uint32_t new_len = o_start - m_start;

                    m->vaddr_end = o_start;
                    m->length = new_len;

                    if (m->file_size > new_len) m->file_size = new_len;

                    prev = m;
                    m = next_node;
                    continue;
                }

                prev = m;
                m = next_node;
            }

            regs->eax = result;
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

        case 35: // rename(old_path, new_path)
        {
            char* oldp = (char*)regs->ebx;
            char* newp = (char*)regs->ecx;
            
            if (check_user_buffer(curr, oldp, 1) && check_user_buffer(curr, newp, 1)) {
                regs->eax = yulafs_rename(oldp, newp);
            } else {
                regs->eax = -1;
            }
        }
        break;

        case 36: // spawn_process(path, argc, argv)
        {
            const char* path = (const char*)regs->ebx;
            int argc = (int)regs->ecx;
            char** argv = (char**)regs->edx;

            if (argc < 0 || argc > 64) {
                regs->eax = -1;
                break;
            }

            if (!check_user_buffer(curr, path, 1)) {
                regs->eax = -1;
                break;
            }

            if (argc > 0) {
                if (!argv || !check_user_buffer(curr, argv, (uint32_t)argc * sizeof(char*))) {
                    regs->eax = -1;
                    break;
                }
                for (int i = 0; i < argc; i++) {
                    char* a = argv[i];
                    if (!check_user_buffer(curr, a, 1)) {
                        regs->eax = -1;
                        break;
                    }
                }
                if (regs->eax == (uint32_t)-1) break;
            }

            task_t* child = proc_spawn_elf(path, argc, argv);
            regs->eax = child ? (int)child->pid : -1;
        }
        break;

        case 37: // waitpid(pid, status_ptr)
        {
            uint32_t pid = (uint32_t)regs->ebx;
            int* status_ptr = (int*)regs->ecx;

            if (pid == 0) {
                regs->eax = -1;
                break;
            }

            if (status_ptr && !check_user_buffer(curr, status_ptr, sizeof(int))) {
                regs->eax = -1;
                break;
            }

            int rc = proc_waitpid(pid, status_ptr);
            regs->eax = (rc == 0) ? (int)pid : -1;
        }
        break;

        case 38: // getdents(fd, buf, size)
        {
            int fd = (int)regs->ebx;
            void* buf = (void*)regs->ecx;
            uint32_t size = (uint32_t)regs->edx;

            if (!check_user_buffer(curr, buf, size)) {
                regs->eax = -1;
                break;
            }

            regs->eax = vfs_getdents(fd, buf, size);
        }
        break;

        case 39: // fstatat(dirfd, name, stat_t* buf)
        {
            int dirfd = (int)regs->ebx;
            char* name = (char*)regs->ecx;
            user_stat_t* u_stat = (user_stat_t*)regs->edx;

            if (!check_user_buffer(curr, name, 1) || !check_user_buffer(curr, u_stat, sizeof(user_stat_t))) {
                regs->eax = -1;
                break;
            }

            regs->eax = vfs_fstatat(dirfd, name, u_stat);
        }
        break;

        default:
            regs->eax = -1;
            break;
    }
}