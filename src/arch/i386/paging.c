// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <hal/lock.h>
#include <hal/io.h>
#include "paging.h"

extern void smp_tlb_shootdown(uint32_t virt);

extern void load_page_directory(uint32_t*);
extern void enable_paging(void);

uint32_t* kernel_page_directory = 0;

uint32_t page_dir[1024] __attribute__((aligned(4096)));

static spinlock_t paging_lock;

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

    if (!(dir[pd_idx] & 1)) {
        void* new_pt_phys = pmm_alloc_block();
        if (!new_pt_phys) {
            spinlock_release_safe(&paging_lock, int_flags);
            paging_halt("pmm_alloc_block failed in paging_map");
        }

        uint32_t* new_pt_virt = (uint32_t*)new_pt_phys;

        memset(new_pt_virt, 0, 4096);

        dir[pd_idx] = ((uint32_t)new_pt_phys) | 7;
    }

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    pt[pt_idx] = (phys & ~0xFFF) | flags;

    spinlock_release_safe(&paging_lock, int_flags);

    if (dir == kernel_page_directory) {
        smp_tlb_shootdown(virt);
    } else {
        __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
    }
}

static void paging_allocate_table(uint32_t virt) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);
    
    uint32_t pd_idx = virt >> 22;
    if (!(kernel_page_directory[pd_idx] & 1)) {
        void* new_pt_phys = pmm_alloc_block();
        if (new_pt_phys) {
            memset(new_pt_phys, 0, 4096);
            kernel_page_directory[pd_idx] = ((uint32_t)new_pt_phys) | 7;
        }
    }
    
    spinlock_release_safe(&paging_lock, int_flags);
}

void paging_init(uint32_t ram_size_bytes) {
    paging_init_pat();
    spinlock_init(&paging_lock);

    for(int i = 0; i < 1024; i++) {
        page_dir[i] = 2; 
    }

    if (ram_size_bytes & 0xFFF) ram_size_bytes = (ram_size_bytes & ~0xFFF) + 4096;

    for(uint32_t i = 0; i < ram_size_bytes; i += 4096) { 
        uint32_t pd_idx = i >> 22;
        uint32_t pt_idx = (i >> 12) & 0x3FF;

        if (!(page_dir[pd_idx] & 1)) {
            void* pt_phys = pmm_alloc_block();
            if (!pt_phys) {
                paging_halt("pmm_alloc_block failed in paging_init");
            }
            memset(pt_phys, 0, 4096);
            page_dir[pd_idx] = ((uint32_t)pt_phys) | 3; // Supervisor | RW | Present
        }
        
        uint32_t* pt = (uint32_t*)(page_dir[pd_idx] & ~0xFFF);
        pt[pt_idx] = i | 3; // Supervisor | RW | Present
        
        if (i + 4096 < i) break;
    }

    paging_map(page_dir, 0xFEE00000, 0xFEE00000, 3);
    
    kernel_page_directory = page_dir;
    
    for(uint32_t addr = 0xC0000000; addr < 0xC1000000; addr += 0x400000) {
        paging_allocate_table(addr);
    }

    paging_switch(page_dir);
    enable_paging();
}

void paging_switch(uint32_t* dir_phys) {
    load_page_directory(dir_phys);
}

uint32_t* paging_get_dir(void) {
    return read_cr3();
}

uint32_t* paging_clone_directory(void) {
    uint32_t* new_dir = (uint32_t*)pmm_alloc_block();
    if (!new_dir) return 0;

    memset(new_dir, 0, 4096);

    for (int i = 0; i < 1024; i++) {
        if (kernel_page_directory[i] & 1) {
            new_dir[i] = kernel_page_directory[i];
        }
    }
    return new_dir;
}

int paging_is_user_accessible(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) return 0;
    if (!(dir[pd_idx] & 4)) return 0;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    if (!(pt[pt_idx] & 1)) return 0;
    if (!(pt[pt_idx] & 4)) return 0;

    return 1;
}

uint32_t paging_get_phys(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) return 0;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    if (!(pt[pt_idx] & 1)) return 0;

    return (pt[pt_idx] & ~0xFFF) + (virt & 0xFFF);
}