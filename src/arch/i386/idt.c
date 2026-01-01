// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/pmm.h>

#include <drivers/vga.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>

#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/kdb.h>

#include <hal/io.h>
#include <hal/apic.h>
#include <hal/irq.h>

#include "paging.h"
#include "idt.h"

struct idt_entry {
    uint16_t base_low; 
    uint16_t sel; 
    uint8_t always0; 
    uint8_t flags; 
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr { 
    uint16_t limit; 
    uint32_t base; 
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

volatile uint32_t system_uptime_seconds = 0;
volatile uint32_t timer_ticks = 0;

extern uint32_t isr_stub_table[];
extern void isr_stub_0x80(void);
extern void isr_stub_0xFF(void);
extern void isr_stub_0xF0(void);
extern uint32_t* kernel_page_directory;

extern void kernel_panic(const char* message, const char* file, uint32_t line, registers_t* regs);

static irq_handler_t irq_handlers[16] = {0};

extern void smp_tlb_ipi_handler(void);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

void irq_install_handler(int irq_no, irq_handler_t handler) {
    irq_handlers[irq_no] = handler;
}

__attribute__((unused)) static const char* exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved"
};

static inline uint32_t get_cr3() {
    uint32_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

extern void wake_up_gui();
extern void proc_check_sleepers(uint32_t current_tick);

void isr_handler(registers_t* regs) {
    if (regs->int_no == 0xFF) {
        return;
    }

    if (regs->int_no == IPI_TLB_VECTOR) {
        smp_tlb_ipi_handler();
        lapic_eoi();
        return;
    }
    cpu_t* cpu = cpu_current();
    task_t* curr = cpu->current_task;

    if (regs->int_no == 0x80) {
        syscall_handler(regs);
    }

    if (regs->int_no == 255) return; 

    else if (regs->int_no >= 32 && regs->int_no <= 47) {
        if (regs->int_no == 32) {
            if (cpu->index == 0) {
                timer_ticks++;

                if (timer_ticks % 100 == 0) {
                    wake_up_gui();
                }

                proc_check_sleepers(timer_ticks);
            }

            cpu->stat_total_ticks++;
            if (curr == cpu->idle_task) {
                cpu->stat_idle_ticks++;
            }

            if (((uint32_t)cpu->stat_total_ticks % 100) == 0) {
                uint64_t delta_total = cpu->stat_total_ticks - cpu->snap_total_ticks;
                uint64_t delta_idle  = cpu->stat_idle_ticks  - cpu->snap_idle_ticks;
                
                cpu->snap_total_ticks = cpu->stat_total_ticks;
                cpu->snap_idle_ticks  = cpu->stat_idle_ticks;

                uint32_t dt = (uint32_t)delta_total;
                uint32_t di = (uint32_t)delta_idle;

                if (dt > 0) {
                    uint32_t busy = dt - di;
                    cpu->load_percent = (busy * 100) / dt;
                } else {
                    cpu->load_percent = 0;
                }
            }

            if (curr && curr->state == TASK_RUNNING && curr->pid != 0) {
                if (curr->exec_start > 0) {
                    uint64_t delta_exec = timer_ticks - curr->exec_start;
                    if (delta_exec >= 1) {
                        uint32_t weight = calc_weight(curr->priority);
                        uint64_t delta_vruntime = calc_delta_vruntime(delta_exec, weight);
                        curr->vruntime += delta_vruntime;
                        curr->exec_start = timer_ticks;
                    }
                }
                
                if (curr->ticks_left > 0) curr->ticks_left--;
                if (curr->ticks_left == 0) {
                    curr->ticks_left = curr->quantum; 
                    lapic_eoi();
                    sched_yield();
                    return; 
                }
            }
            lapic_eoi();
            return;
        } 
        else {
            int irq_no = regs->int_no - 32;
            if (irq_handlers[irq_no]) irq_handlers[irq_no](regs);
            
            if (regs->int_no >= 40) outb(0xA0, 0x20);
            outb(0x20, 0x20);
            lapic_eoi(); 
        }
    }

    else if (regs->int_no < 32) {
        if (regs->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            int handled = 0;

            if (cr2 >= 0xC0000000) {
                uint32_t pd_idx = cr2 >> 22;
                uint32_t* current_dir = (uint32_t*)get_cr3();
                if ((kernel_page_directory[pd_idx] & 1) && !(current_dir[pd_idx] & 1)) {
                    current_dir[pd_idx] = kernel_page_directory[pd_idx];
                    __asm__ volatile("invlpg (%0)" :: "r"(cr2) : "memory");
                    return;
                }
            }
            
            int is_user_access = (regs->cs == 0x1B);
            int is_kernel_access_to_user = (regs->cs == 0x08 && cr2 < 0xC0000000);

            if (!handled && (is_user_access || is_kernel_access_to_user) && !(regs->err_code & 1) && curr) {
                
                if (!handled && cr2 >= curr->stack_bottom && cr2 < curr->stack_top) {
                    void* new_page = pmm_alloc_block();
                    if (new_page) {
                        uint32_t vaddr = cr2 & ~0xFFF;
                        paging_map(curr->page_dir, vaddr, (uint32_t)new_page, 7);
                        curr->mem_pages++;
                        __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
                        handled = 1;
                    } else {
                        proc_kill(curr);
                        sched_yield();
                        return;
                    }
                }

                if (!handled) {
                    mmap_area_t* m = curr->mmap_list;
                    while (m) {
                        if (cr2 >= m->vaddr_start && cr2 < m->vaddr_end) {
                            void* phys_page = pmm_alloc_block();
                            if (phys_page) {
                                uint32_t vaddr_page = cr2 & ~0xFFF;
                                paging_map(curr->page_dir, vaddr_page, (uint32_t)phys_page, 7);
                                curr->mem_pages++;

                                uint32_t old_cr3;
                                __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
                                __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_page_directory));

                                uint32_t offset_in_vma = vaddr_page - m->vaddr_start;
                                uint32_t file_pos = m->file_offset + offset_in_vma;
                                memset(phys_page, 0, 4096);

                                if (m->file->ops && m->file->ops->read) {
                                    if (offset_in_vma < m->file_size) {
                                        uint32_t bytes = m->file_size - offset_in_vma;
                                        if (bytes > 4096) bytes = 4096;
                                        __asm__ volatile("sti");
                                        m->file->ops->read(m->file, file_pos, bytes, (void*)phys_page);
                                        __asm__ volatile("cli");
                                    }
                                }
                                
                                __asm__ volatile("mov %0, %%cr3" :: "r"(old_cr3));
                                __asm__ volatile("invlpg (%0)" :: "r"(vaddr_page) : "memory");
                                handled = 1;
                            } else {
                                proc_kill(curr);
                                sched_yield();
                                return;
                            }
                            break; 
                        }
                        m = m->next;
                    }
                }

                if (!handled) {
                    if (cr2 >= curr->heap_start && cr2 < curr->prog_break) {
                        void* new_page = pmm_alloc_block();
                        if (new_page) {
                            uint32_t vaddr = cr2 & ~0xFFF;
                            paging_map(curr->page_dir, vaddr, (uint32_t)new_page, 7);
                            curr->mem_pages++;
                            __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
                            handled = 1;
                        } else {
                            proc_kill(curr);
                            sched_yield();
                            return;
                        }
                    }
                }
            }

            if (!handled && regs->cs == 0x08) {
                uint32_t* dir = (uint32_t*)get_cr3();
                uint32_t pd_idx = cr2 >> 22;
                uint32_t pt_idx = (cr2 >> 12) & 0x3FF;

                if (dir[pd_idx] & 1) { 
                    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
                    if (pt[pt_idx] & 1) { 
                        __asm__ volatile("invlpg (%0)" :: "r"(cr2) : "memory");
                        return;
                    }
                }
            }

            if (!handled) {

                if (curr && strcmp(curr->name, "gui") == 0) {
                    kdb_enter("GUI Thread Crashed", curr);
                    
                    proc_kill(curr);
                    sched_yield();
                    return;
                }

                int is_kernel_access_to_user = (regs->cs == 0x08 && cr2 < 0xC0000000);

                if (regs->cs != 0x1B && !is_kernel_access_to_user) {
                    kernel_panic("Kernel Page Fault", "idt.c", regs->int_no, regs);
                } else {
                    if (curr) {
                        proc_kill(curr);
                        sched_yield();
                    } else {
                        kernel_panic("Page Fault in Kernel", "idt.c", regs->int_no, regs);
                    }
                }
            }
        } 
        else {
            const char* msg = "Unknown Exception";
            if (regs->int_no < 32) msg = exception_messages[regs->int_no];
            
            if (regs->cs == 0x1B && curr) {
                proc_kill(curr);
                sched_yield();
            } else {
                kernel_panic(msg, "idt.c", regs->int_no, regs);
            }
        }
    }

    if (curr && regs->cs == 0x1B && !curr->is_running_signal) {
        if (curr->pending_signals != 0) {
            for (int i = 0; i < 32; i++) {
                if (curr->pending_signals & (1 << i)) {
                    if (curr->handlers[i]) {
                        memcpy(&curr->signal_context, regs, sizeof(registers_t));
                        uint32_t* user_esp = (uint32_t*)regs->useresp;
                        user_esp--; *user_esp = (uint32_t)i;
                        user_esp--; *user_esp = 0x0;
                        regs->useresp = (uint32_t)user_esp;
                        regs->eip = (uint32_t)curr->handlers[i];
                        curr->pending_signals &= ~(1 << i);
                        curr->is_running_signal = 1;
                        break;
                    } else {
                        if (i == 2 || i == 11 || i == 15) {
                            proc_kill(curr);
                            sched_yield();
                        }
                    }
                }
            }
        }
    }
}

irq_handler_t irq_get_handler(int irq_no) {
    if (irq_no >= 0 && irq_no < 16) return irq_handlers[irq_no];
    return 0;
}

void idt_init(void) {
    idtp.limit = sizeof(struct idt_entry) * 256 - 1;
    idtp.base = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt));

    for (int i = 0; i < 48; i++) idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    idt_set_gate(0x80, (uint32_t)isr_stub_0x80, 0x08, 0xEE);
    
    idt_set_gate(0xFF, (uint32_t)isr_stub_0xFF, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_VECTOR, (uint32_t)isr_stub_0xF0, 0x08, 0x8E);

    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    
    // ICW2 (Remap Offset: 32 / 40)
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    
    // ICW3 (Cascade)
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    
    // ICW4 (8086 mode)
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    
    // Master: MASK ALL except IRQ 1 (bit 1) and IRQ 2 (Cascade, bit 2). 
    // 1111 1001 = 0xF9
    outb(0x21, 0xF9); 
    
    // Slave: MASK ALL except IRQ 12 (Mouse, bit 4).
    // 1110 1111 = 0xEF
    outb(0xA1, 0xEF);

    idt_load();
}