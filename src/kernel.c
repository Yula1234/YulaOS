#include <shell/shell.h>

#include <fs/yulafs.h>
#include <fs/bcache.h>

#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <drivers/mouse.h>
#include <drivers/ahci.h>
#include <drivers/acpi.h> 
#include <drivers/vga.h>

#include <kernel/clipboard.h>
#include <kernel/gui_task.h>
#include <kernel/window.h>
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>

#include <hal/apic.h>
#include <hal/simd.h>
#include <hal/pit.h>
#include <hal/io.h>

#include <mm/heap.h>
#include <mm/pmm.h>


#include <stdint.h>

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_memory_map_t;

typedef struct {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t unused[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed)) multiboot_info_t;

uint32_t* fb_ptr;
uint32_t  fb_width;
uint32_t  fb_height;
uint32_t  fb_pitch;

extern uint32_t kernel_end; 

extern void put_pixel(int x, int y, uint32_t color);

extern void smp_boot_aps(void);

static inline void sys_usleep(uint32_t us) {
    __asm__ volatile("int $0x80" : : "a"(11), "b"(us));
}

void idle_task_func(void* arg) {
    (void)arg;
    while(1) {
        __asm__ volatile("sti");
        cpu_hlt();
        sched_yield();
    }
}

void syncer_task(void* arg) {
    (void)arg;
    while(1) {
        sys_usleep(1000000); 
        
        bcache_sync();
    }
}

__attribute__((target("no-sse"))) void kmain(uint32_t magic, multiboot_info_t* mb_info) {
    if (magic != 0x2BADB002) return;

    kernel_init_simd(); 

    cpu_init_system();

    fb_ptr = (uint32_t*)(uint32_t)mb_info->framebuffer_addr;
    fb_width = mb_info->framebuffer_width;
    fb_height = mb_info->framebuffer_height;
    fb_pitch = mb_info->framebuffer_pitch;

    gdt_init();
    idt_init();


    lapic_init();
    lapic_timer_init(15000);

    uint32_t memory_end_addr = 0;

    if (mb_info->flags & (1 << 6)) {
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mb_info->mmap_addr;
        
        while((uint32_t)mmap < mb_info->mmap_addr + mb_info->mmap_length) {
            if (mmap->type == 1) {
                uint64_t end = mmap->addr + mmap->len;
                if (end > memory_end_addr) {
                    memory_end_addr = (uint32_t)end;
                }
            }
            mmap = (multiboot_memory_map_t*)((uint32_t)mmap + mmap->size + sizeof(uint32_t));
        }
    } 

    else if (mb_info->flags & (1 << 0)) {
        memory_end_addr = (mb_info->mem_upper * 1024) + 0x100000;
    }

    if (memory_end_addr == 0) {
        memory_end_addr = 1024 * 1024 * 64; 
    }
    
    // Master PIC (port 0x21): 
    // Bit 0 - Timer (closed, we had APIC)
    // Bit 1 - Keyboard (opened - 0)
    // Bit 2 - Cascade (opened - 0)
    outb(0x21, 0xF9); 

        
    // Slave PIC (port 0xA1):
    // Bit 4 - Mouse (IRQ 12, opened - 0)
    // Bit 6 - HDD (IRQ 14).
    outb(0xA1, 0xAF); // Opens IRQ 14 for Primary ATA
    
    pmm_init(memory_end_addr, (uint32_t)&kernel_end);
    paging_init(memory_end_addr);
    heap_init();


    // mapping video memory
    uint32_t fb_size = fb_width * fb_height * 4;
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        paging_map(kernel_page_directory, (uint32_t)fb_ptr + i, (uint32_t)fb_ptr + i, 3);
    }

    acpi_init();
    
    smp_boot_aps();

    vga_init();
    vga_init_graphics();
    clipboard_init();
    
    kbd_init();
    mouse_init();
    
    ahci_init();
    yulafs_init();
    yulafs_lookup("/"); 

    kbd_vfs_init();
    console_init();

    proc_init();
    sched_init();
    
    window_init_system(); 

    __attribute__((unused)) task_t* idle = proc_spawn_kthread("idle", PRIO_IDLE, idle_task_func, 0);
    __attribute__((unused)) task_t* sys_reaper = proc_spawn_kthread("reaper", PRIO_HIGH, reaper_task_func, 0);
    __attribute__((unused)) task_t* syncer = proc_spawn_kthread("syncer", PRIO_LOW, syncer_task, 0);

    task_t* gui_t = proc_spawn_kthread("gui", PRIO_GUI, gui_task, 0);
    
    __asm__ volatile("sti"); 
    sched_start(gui_t);
}