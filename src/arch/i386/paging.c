// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <kernel/cpu.h>
#include <drivers/fbdev.h>
#include "paging.h"

extern void smp_tlb_shootdown(uint32_t virt);

extern void load_page_directory(uint32_t*);
extern void enable_paging(void);

extern uint32_t kernel_end;

uint32_t* kernel_page_directory = 0;

uint32_t page_dir[1024] __attribute__((aligned(4096)));

static uint32_t paging_fixmap_pt[1024] __attribute__((aligned(4096)));

static spinlock_t paging_lock;

static uint32_t paging_ram_size_bytes = 0;

#define PAGING_FIXMAP_BASE 0xFFFFE000u
#define PAGING_FIXMAP_SLOTS 16u

static spinlock_t paging_fixmap_lock;

__attribute__((noreturn)) static void paging_halt(const char* msg);

static inline uint32_t* paging_pt_virt(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    if ((dir[pd_idx] & 1u) == 0u) {
        return 0;
    }
    return (uint32_t*)(dir[pd_idx] & ~0xFFFu);
}

static inline int paging_pde_pt_phys_valid(uint32_t pde) {
    if ((pde & 1u) == 0u) return 0;
    if ((pde & (1u << 7)) != 0u) return 0;

    uint32_t pt_phys = pde & ~0xFFFu;
    if (pt_phys == 0u) return 0;
    if (paging_ram_size_bytes == 0u) return 0;
    if (pt_phys >= paging_ram_size_bytes) return 0;

    return 1;
}

static inline uint32_t paging_fixmap_virt(void) {
    cpu_t* cpu = cpu_current();
    uint32_t idx = 0;

    if (cpu && cpu->index >= 0 && (uint32_t)cpu->index < PAGING_FIXMAP_SLOTS) {
        idx = (uint32_t)cpu->index;
    }

    return PAGING_FIXMAP_BASE - idx * 4096u;
}

static inline uint32_t paging_fixmap_map_locked(uint32_t phys) {
    if ((phys & 0xFFFu) != 0u) {
        paging_halt("paging_fixmap_map_locked: unaligned phys");
    }

    uint32_t virt = paging_fixmap_virt();
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    paging_fixmap_pt[pt_idx] = (phys & ~0xFFFu) | PTE_PRESENT | PTE_RW;
    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
    return virt;
}

static inline void paging_fixmap_unmap_locked(uint32_t virt) {
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    paging_fixmap_pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

static void paging_fixmap_set(uint32_t virt, uint32_t phys) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);

    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    paging_fixmap_pt[pt_idx] = (phys & ~0xFFFu) | PTE_PRESENT | PTE_RW;
    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");

    spinlock_release_safe(&paging_fixmap_lock, int_flags);
}

uint32_t paging_fixmap_map(uint32_t phys) {
    if ((phys & 0xFFFu) != 0u) {
        paging_halt("paging_fixmap_map: unaligned phys");
    }

    uint32_t virt = paging_fixmap_virt();
    paging_fixmap_set(virt, phys);
    return virt;
}

static void paging_fixmap_clear(uint32_t virt) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);

    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    paging_fixmap_pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");

    spinlock_release_safe(&paging_fixmap_lock, int_flags);
}

void paging_fixmap_unmap(uint32_t virt) {
    paging_fixmap_clear(virt);
}

static inline int paging_is_enabled(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return ((cr0 & (1u << 31)) != 0u) ? 1 : 0;
}

uint32_t paging_read_pt_entry(uint32_t pt_phys, uint32_t pt_idx) {
    if ((pt_phys & 0xFFFu) != 0u || pt_idx >= 1024u) {
        return 0u;
    }

    if (!paging_is_enabled()) {
        const uint32_t* pt = (const uint32_t*)(uintptr_t)pt_phys;
        return pt[pt_idx];
    }

    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);
    uint32_t virt = paging_fixmap_map_locked(pt_phys);

    const uint32_t* pt = (const uint32_t*)(uintptr_t)virt;
    uint32_t pte = pt[pt_idx];

    paging_fixmap_unmap_locked(virt);
    spinlock_release_safe(&paging_fixmap_lock, int_flags);
    return pte;
}

void paging_write_pt_entry(uint32_t pt_phys, uint32_t pt_idx, uint32_t pte) {
    if ((pt_phys & 0xFFFu) != 0u || pt_idx >= 1024u) {
        return;
    }

    if (!paging_is_enabled()) {
        uint32_t* pt = (uint32_t*)(uintptr_t)pt_phys;
        pt[pt_idx] = pte;
        return;
    }

    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);
    uint32_t virt = paging_fixmap_map_locked(pt_phys);

    uint32_t* pt = (uint32_t*)(uintptr_t)virt;
    pt[pt_idx] = pte;

    paging_fixmap_unmap_locked(virt);
    spinlock_release_safe(&paging_fixmap_lock, int_flags);
}

uint32_t paging_read_pde(uint32_t* dir, uint32_t pd_idx) {
    if (!dir || pd_idx >= 1024u) {
        return 0u;
    }

    if (!paging_is_enabled() || dir == kernel_page_directory) {
        return dir[pd_idx];
    }

    uint32_t dir_phys = (uint32_t)(uintptr_t)dir;

    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);
    uint32_t virt = paging_fixmap_map_locked(dir_phys);

    const uint32_t* d = (const uint32_t*)(uintptr_t)virt;
    uint32_t pde = d[pd_idx];

    paging_fixmap_unmap_locked(virt);
    spinlock_release_safe(&paging_fixmap_lock, int_flags);
    return pde;
}

void paging_write_pde(uint32_t* dir, uint32_t pd_idx, uint32_t pde) {
    if (!dir || pd_idx >= 1024u) {
        return;
    }

    if (!paging_is_enabled() || dir == kernel_page_directory) {
        dir[pd_idx] = pde;
        return;
    }

    uint32_t dir_phys = (uint32_t)(uintptr_t)dir;

    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);
    uint32_t virt = paging_fixmap_map_locked(dir_phys);

    uint32_t* d = (uint32_t*)(uintptr_t)virt;
    d[pd_idx] = pde;

    paging_fixmap_unmap_locked(virt);
    spinlock_release_safe(&paging_fixmap_lock, int_flags);
}

int paging_get_present_pte_safe(uint32_t* dir, uint32_t virt, uint32_t* out_pte) {
    if (!dir) {
        return 0;
    }

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;

    uint32_t pde = paging_read_pde(dir, pd_idx);
    if ((pde & 1u) == 0u) {
        return 0;
    }

    if ((pde & (1u << 7)) != 0u) {
        uint32_t base = pde & 0xFFC00000u;
        uint32_t phys = base + (virt & 0x003FFFFFu);
        if (out_pte) {
            *out_pte = (phys & ~0xFFFu) | (pde & 0xFFFu);
        }
        return 1;
    }

    if (!paging_pde_pt_phys_valid(pde)) {
        return 0;
    }

    uint32_t pte = paging_read_pt_entry(pde & ~0xFFFu, pt_idx);
    if ((pte & 1u) == 0u) {
        return 0;
    }

    if (out_pte) {
        *out_pte = pte;
    }

    return 1;
}

void paging_zero_phys_page(uint32_t phys) {
    if ((phys & 0xFFFu) != 0u) {
        paging_halt("paging_zero_phys_page: unaligned phys");
    }

    if (!paging_is_enabled()) {
        memset((void*)phys, 0, 4096);
        return;
    }

    if (!kernel_page_directory) {
        paging_halt("paging_zero_phys_page: kernel_page_directory not set");
    }

    uint32_t int_flags = spinlock_acquire_safe(&paging_fixmap_lock);
    uint32_t virt = paging_fixmap_map_locked(phys);

    memset((void*)virt, 0, 4096);

    paging_fixmap_unmap_locked(virt);
    spinlock_release_safe(&paging_fixmap_lock, int_flags);
}

__attribute__((noreturn)) static void paging_halt(const char* msg) {
    (void)msg;
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}

#define IA32_PAT_MSR 0x277u
#define PAT_MEMTYPE_WC 1u

#define IA32_MTRRCAP_MSR 0xFEu
#define IA32_MTRR_DEF_TYPE_MSR 0x2FFu
#define IA32_MTRR_PHYSBASE0_MSR 0x200u
#define IA32_MTRR_PHYSMASK0_MSR 0x201u

static int paging_pat_supported_cached = -1;

static inline void paging_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}

static inline uint64_t paging_rdmsr_u64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void paging_wrmsr_u64(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

int paging_pat_is_supported(void) {
    if (paging_pat_supported_cached >= 0) {
        return paging_pat_supported_cached;
    }

    uint32_t a, b, c, d;
    paging_cpuid(1, 0, &a, &b, &c, &d);
    paging_pat_supported_cached = ((d & (1u << 16)) != 0u) ? 1 : 0;
    return paging_pat_supported_cached;
}

void paging_init_pat(void) {
    if (!paging_pat_is_supported()) {
        return;
    }

    uint64_t pat = paging_rdmsr_u64(IA32_PAT_MSR);
    uint64_t new_pat = pat;

    new_pat &= ~(0xFFull << 32);
    new_pat |= ((uint64_t)PAT_MEMTYPE_WC) << 32;

    if (new_pat != pat) {
        paging_wrmsr_u64(IA32_PAT_MSR, new_pat);
    }
}

static inline uint32_t paging_read_cr0(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void paging_write_cr0(uint32_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline void paging_wbinvd(void) {
    __asm__ volatile("wbinvd" : : : "memory");
}

static inline uint32_t paging_largest_pow2_le(uint32_t x) {
    if (x == 0) return 0;
    uint32_t p = 1;
    while (p <= x / 2u) p <<= 1;
    return p;
}

static inline uint32_t paging_phys_addr_bits(void) {
    uint32_t max_ext, b, c, d;
    paging_cpuid(0x80000000u, 0, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000008u) {
        uint32_t eax;
        paging_cpuid(0x80000008u, 0, &eax, &b, &c, &d);
        uint32_t bits = eax & 0xFFu;
        if (bits >= 32u && bits <= 52u) {
            return bits;
        }
    }
    return 32u;
}

void paging_init_mtrr_wc(uint32_t phys_base, uint32_t size) {
    if (size == 0) return;

    uint32_t a, b, c, d;
    paging_cpuid(1, 0, &a, &b, &c, &d);
    if ((d & (1u << 12)) == 0u) {
        return;
    }

    uint32_t base = phys_base & ~0xFFFu;
    uint32_t end = phys_base + size;
    uint32_t end_aligned = (end + 0xFFFu) & ~0xFFFu;
    uint32_t len = end_aligned - base;
    if (len == 0) return;

    uint64_t cap = paging_rdmsr_u64(IA32_MTRRCAP_MSR);
    uint32_t vcnt = (uint32_t)(cap & 0xFFu);
    if (vcnt == 0) return;

    if ((cap & (1ull << 10)) == 0ull) {
        return;
    }

    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    uint32_t cr0 = paging_read_cr0();
    paging_write_cr0((cr0 | (1u << 30)) & ~(1u << 29));
    paging_wbinvd();

    uint64_t def_type = paging_rdmsr_u64(IA32_MTRR_DEF_TYPE_MSR);
    uint64_t def_type_disabled = def_type & ~(1ull << 11);
    paging_wrmsr_u64(IA32_MTRR_DEF_TYPE_MSR, def_type_disabled);
    paging_wbinvd();

    uint32_t remaining = len;
    uint32_t curr = base;

    uint32_t phys_bits = paging_phys_addr_bits();
    uint64_t phys_mask = (phys_bits >= 64u) ? ~0ull : ((1ull << phys_bits) - 1ull);

    for (uint32_t reg = 0; reg < vcnt && remaining; reg++) {
        uint32_t mask_msr = IA32_MTRR_PHYSMASK0_MSR + reg * 2u;
        uint64_t mask_val = paging_rdmsr_u64(mask_msr);
        if ((mask_val & (1ull << 11)) != 0ull) {
            continue;
        }

        uint32_t chunk = paging_largest_pow2_le(remaining);
        while (chunk >= 4096u && (curr & (chunk - 1u)) != 0u) {
            chunk >>= 1;
        }
        if (chunk < 4096u) {
            break;
        }

        uint64_t base_addr = ((uint64_t)(curr & ~0xFFFu)) & phys_mask;
        uint64_t base_val = base_addr | (uint64_t)PAT_MEMTYPE_WC;

        uint64_t new_mask = ~(uint64_t)(chunk - 1u);
        new_mask &= phys_mask;
        new_mask &= ~(uint64_t)0xFFFull;
        new_mask |= (1ull << 11);

        paging_wrmsr_u64(IA32_MTRR_PHYSBASE0_MSR + reg * 2u, base_val);
        paging_wrmsr_u64(IA32_MTRR_PHYSMASK0_MSR + reg * 2u, new_mask);

        curr += chunk;
        remaining -= chunk;
    }

    paging_wrmsr_u64(IA32_MTRR_DEF_TYPE_MSR, def_type);
    paging_wbinvd();

    paging_write_cr0(cr0);
    paging_wbinvd();

    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}

static inline uint32_t* read_cr3(void) {
    uint32_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return (uint32_t*)val;
}

void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = 0u;
    uint32_t dir_phys = 0u;
    uint32_t* dir_virt = dir;

    const int paging_on = paging_is_enabled();
    const int is_kernel_dir = (dir == kernel_page_directory);

    if (paging_on && !is_kernel_dir) {
        dir_phys = (uint32_t)(uintptr_t)dir;
    }

    if (!paging_on || is_kernel_dir) {
        pde = dir[pd_idx];
    } else {
        uint32_t fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);
        dir_virt = (uint32_t*)(uintptr_t)paging_fixmap_map_locked(dir_phys);
        pde = dir_virt[pd_idx];
        paging_fixmap_unmap_locked((uint32_t)(uintptr_t)dir_virt);
        spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
    }

    if ((pde & 1u) == 0u) {
        void* new_pt_phys = pmm_alloc_block();
        if (!new_pt_phys) {
            spinlock_release_safe(&paging_lock, int_flags);
            paging_halt("pmm_alloc_block failed in paging_map");
        }

        paging_zero_phys_page((uint32_t)new_pt_phys);

        pde = ((uint32_t)new_pt_phys) | 7u;

        if (!paging_on || is_kernel_dir) {
            dir[pd_idx] = pde;
        } else {
            uint32_t fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);
            dir_virt = (uint32_t*)(uintptr_t)paging_fixmap_map_locked(dir_phys);
            dir_virt[pd_idx] = pde;
            paging_fixmap_unmap_locked((uint32_t)(uintptr_t)dir_virt);
            spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
        }
    }

    const uint32_t pt_phys = pde & ~0xFFFu;
    if (!paging_is_enabled()) {
        uint32_t* pt = (uint32_t*)(uintptr_t)pt_phys;
        pt[pt_idx] = (phys & ~0xFFFu) | flags;
    } else {
        uint32_t fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);

        uint32_t pt_virt = paging_fixmap_map_locked(pt_phys);
        uint32_t* pt = (uint32_t*)(uintptr_t)pt_virt;
        pt[pt_idx] = (phys & ~0xFFFu) | flags;
        paging_fixmap_unmap_locked(pt_virt);

        spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
    }

    spinlock_release_safe(&paging_lock, int_flags);

    if (dir == kernel_page_directory) {
        smp_tlb_shootdown(virt);
        return;
    }

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

static void paging_allocate_table(uint32_t virt) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);
    
    uint32_t pd_idx = virt >> 22;
    if (!(kernel_page_directory[pd_idx] & 1)) {
        void* new_pt_phys = pmm_alloc_block();
        if (new_pt_phys) {
            paging_zero_phys_page((uint32_t)new_pt_phys);
            kernel_page_directory[pd_idx] = ((uint32_t)new_pt_phys) | 7;
        }
    }
    
    spinlock_release_safe(&paging_lock, int_flags);
}

void paging_init(uint32_t ram_size_bytes) {
    paging_init_pat();
    spinlock_init(&paging_lock);
    spinlock_init(&paging_fixmap_lock);

    if (ram_size_bytes & 0xFFFu) {
        ram_size_bytes = (ram_size_bytes & ~0xFFFu) + 4096u;
    }

    {
        uint32_t esp;
        __asm__ volatile("mov %%esp, %0" : "=r"(esp));

        uint32_t need_end = esp;
        uint32_t kernel_end_addr = (uint32_t)(uintptr_t)&kernel_end;
        if (kernel_end_addr > need_end) {
            need_end = kernel_end_addr;
        }

        need_end = (need_end + 0xFFFu) & ~0xFFFu;
        need_end += 4096u;

        if (need_end > ram_size_bytes) {
            ram_size_bytes = need_end;
        }
    }
    paging_ram_size_bytes = ram_size_bytes;

    kernel_page_directory = page_dir;

    for(int i = 0; i < 1024; i++) {
        page_dir[i] = 2; 
    }

    for(uint32_t i = 0; i < ram_size_bytes; i += 4096) { 
        uint32_t pd_idx = i >> 22;
        uint32_t pt_idx = (i >> 12) & 0x3FF;

        if (!(page_dir[pd_idx] & 1)) {
            void* pt_phys = pmm_alloc_block();
            if (!pt_phys) {
                paging_halt("pmm_alloc_block failed in paging_init");
            }
            paging_zero_phys_page((uint32_t)pt_phys);
            page_dir[pd_idx] = ((uint32_t)pt_phys) | 3; // Supervisor | RW | Present
        }
        
        uint32_t* pt = (uint32_t*)(page_dir[pd_idx] & ~0xFFF);
        pt[pt_idx] = i | 3; // Supervisor | RW | Present
        
        if (i + 4096 < i) break;
    }

    memset(paging_fixmap_pt, 0, sizeof(paging_fixmap_pt));

    const uint32_t fixmap_pt_phys = paging_get_phys(
        page_dir,
        (uint32_t)(uintptr_t)paging_fixmap_pt
    ) & ~0xFFFu;

    page_dir[1023] = fixmap_pt_phys | 3u;

    paging_map(page_dir, 0xFEE00000, 0xFEE00000, 3);

    for (uint32_t i = 0; i < PAGING_FIXMAP_SLOTS; i++) {
        uint32_t virt = PAGING_FIXMAP_BASE - i * 4096u;
        paging_allocate_table(virt);
        paging_fixmap_clear(virt);
    }
    
    for(uint32_t addr = 0xC0000000; addr < 0xC1000000; addr += 0x400000) {
        paging_allocate_table(addr);
    }

    paging_switch(page_dir);
    enable_paging();
}

void paging_switch(uint32_t* dir_phys) {
    uint32_t v = (uint32_t)(uintptr_t)dir_phys;
    if ((v & 0xFFFu) != 0u) {
        paging_halt("paging_switch: unaligned CR3");
    }

    load_page_directory((uint32_t*)(uintptr_t)(v & ~0xFFFu));
}

uint32_t* paging_get_dir(void) {
    uint32_t v = (uint32_t)(uintptr_t)read_cr3();
    return (uint32_t*)(uintptr_t)(v & ~0xFFFu);
}

uint32_t* paging_clone_directory(void) {
    uint32_t* new_dir_phys = (uint32_t*)pmm_alloc_block();
    if (!new_dir_phys) {
        return 0;
    }

    paging_zero_phys_page((uint32_t)new_dir_phys);

    uint32_t* new_dir = new_dir_phys;
    uint32_t fixmap_flags = 0u;
    if (paging_is_enabled()) {
        fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);
        new_dir = (uint32_t*)(uintptr_t)paging_fixmap_map_locked((uint32_t)new_dir_phys);
    }

    for (int i = 0; i < 1024; i++) {
        uint32_t pde = kernel_page_directory[i];
        if ((pde & 1u) != 0u) {
            new_dir[i] = pde;
        }
    }

    if (paging_is_enabled()) {
        paging_fixmap_unmap_locked((uint32_t)(uintptr_t)new_dir);
        spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
    }

    return new_dir_phys;
}

int paging_is_user_accessible(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t* dir_virt = dir;

    uint32_t fixmap_flags = 0u;
    if (paging_is_enabled()) {
        fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);
        dir_virt = (uint32_t*)(uintptr_t)paging_fixmap_map_locked((uint32_t)(uintptr_t)dir);
    }

    uint32_t pde = dir_virt[pd_idx];

    if (paging_is_enabled()) {
        paging_fixmap_unmap_locked((uint32_t)(uintptr_t)dir_virt);
        spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
    }

    if ((pde & 1u) == 0u) return 0;
    if ((pde & 4u) == 0u) return 0;

    if ((pde & (1u << 7)) != 0u) {
        return 1;
    }

    if (!paging_pde_pt_phys_valid(pde)) return 0;

    uint32_t pte = paging_read_pt_entry(pde & ~0xFFFu, pt_idx);
    if ((pte & 1u) == 0u) return 0;
    if ((pte & 4u) == 0u) return 0;

    return 1;
}

uint32_t paging_get_phys(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t* dir_virt = dir;

    uint32_t fixmap_flags = 0u;
    if (paging_is_enabled()) {
        fixmap_flags = spinlock_acquire_safe(&paging_fixmap_lock);
        dir_virt = (uint32_t*)(uintptr_t)paging_fixmap_map_locked((uint32_t)(uintptr_t)dir);
    }

    uint32_t pde = dir_virt[pd_idx];

    if (paging_is_enabled()) {
        paging_fixmap_unmap_locked((uint32_t)(uintptr_t)dir_virt);
        spinlock_release_safe(&paging_fixmap_lock, fixmap_flags);
    }

    if ((pde & 1u) == 0u) return 0;

    if ((pde & (1u << 7)) != 0u) {
        uint32_t base = pde & 0xFFC00000u;
        return base + (virt & 0x003FFFFFu);
    }

    if (!paging_pde_pt_phys_valid(pde)) return 0;

    uint32_t pte = paging_read_pt_entry(pde & ~0xFFFu, pt_idx);
    if ((pte & 1u) == 0u) return 0;

    return (pte & ~0xFFFu) + (virt & 0xFFFu);
}