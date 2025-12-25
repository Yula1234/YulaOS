#include <lib/string.h>
#include <mm/pmm.h>

#include <drivers/vga.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>

#include <kernel/syscall.h>
#include <kernel/sched.h>


#include <hal/io.h>
#include <hal/apic.h>
#include <hal/irq.h>

#include "paging.h"
#include "idt.h"

struct idt_entry {
    uint16_t base_low; uint16_t sel; uint8_t always0; uint8_t flags; uint16_t base_high;
} __attribute__((packed));

struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

volatile uint32_t system_uptime_seconds = 0;

extern uint32_t isr_stub_table[];
extern void isr_stub_0x80(void);

extern void kernel_panic(const char* message, const char* file, uint32_t line, registers_t* regs);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

__attribute__((unused)) static const char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

extern void proc_kill(task_t* t);

volatile uint32_t timer_ticks = 0;

static irq_handler_t irq_handlers[16] = {0};

void irq_install_handler(int irq_no, irq_handler_t handler) {
    irq_handlers[irq_no] = handler;
}

extern void syscall_handler(registers_t* regs);
extern volatile uint32_t timer_ticks;

void isr_handler(registers_t* regs) {
    task_t* curr = proc_current();

    if (regs->int_no == 0x80) {
        syscall_handler(regs);
    }

    else if (regs->int_no >= 32 && regs->int_no <= 47) {
        if (regs->int_no == 32) {
            timer_ticks++;

            for (uint32_t i = 0; i < proc_task_count(); i++) {
                task_t* t = proc_task_at(i);
                if (t && t->state == TASK_WAITING && t->wake_tick > 0) {
                    if (timer_ticks >= t->wake_tick) {
                        t->wake_tick = 0;
                        t->state = TASK_RUNNABLE;
                    }
                }
            }

            task_t* curr = proc_current();
            if (curr && curr->state == TASK_RUNNING) {
                if (curr->ticks_left > 0) {
                    curr->ticks_left--;
                }

                if (curr->ticks_left == 0) {
                    curr->ticks_left = curr->quantum; 
                    lapic_eoi();

                    sched_yield();
                    
                    return; 
                }
            }

            lapic_eoi();
            return;
        } else {
            int irq_no = regs->int_no - 32;
            if (irq_handlers[irq_no]) irq_handlers[irq_no](regs);
            
            if (regs->int_no >= 40) outb(0xA0, 0x20);
            outb(0x20, 0x20);
            lapic_eoi(); 
        }
    }

    else if (regs->int_no < 32) {
        if (regs->cs == 0x1B) {
            if (regs->int_no == 14) {
                uint32_t cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

                int handled = 0;

                if (!(regs->err_code & 1) && curr) {
                    if (cr2 >= curr->stack_bottom && cr2 < curr->stack_top) {
                        
                        void* new_page = pmm_alloc_block();
                        
                        if (new_page) {
                            uint32_t vaddr = cr2 & ~0xFFF;
                            paging_map(curr->page_dir, vaddr, (uint32_t)new_page, 7); // User, RW, Present
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

                if (handled) {
                    return;
                } else {
                    curr->pending_signals |= (1 << 11); // SIGSEGV
                }
            } else {
                proc_kill(curr);
                lapic_eoi();
                sched_yield();
            }
        } else {
            const char* msg = "Unknown Exception";
            if (regs->int_no < 32) msg = exception_messages[regs->int_no];
            kernel_panic(msg, "idt.c", regs->int_no, regs);
        }
    }

    if (curr && regs->cs == 0x1B && !curr->is_running_signal) {
        if (curr->pending_signals != 0) {
            for (int i = 0; i < 32; i++) {
                if (curr->pending_signals & (1 << i)) {
                    if (curr->handlers[i]) {
                        memcpy(&curr->signal_context, regs, sizeof(registers_t));

                        uint32_t* user_esp = (uint32_t*)regs->useresp;
                        
                        user_esp--; 
                        *user_esp = (uint32_t)i;
                        
                        user_esp--; 
                        *user_esp = 0x0; 

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

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    }

    idt_set_gate(0x80, (uint32_t)isr_stub_0x80, 0x08, 0xEE);

    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFC); outb(0xA1, 0xFF);

    outb(0x21, 0xFF); 
    outb(0xA1, 0xFF);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}