/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <kernel/smp/cpu.h>
#include <kernel/proc.h>
#include "paging.h"

extern void smp_tlb_shootdown(uint32_t virt);
extern void smp_tlb_shootdown_range(uint32_t start, uint32_t end);
extern void smp_tlb_shootdown_range_dir(uint32_t* page_dir, uint32_t start, uint32_t end);

extern void load_page_directory(uint32_t*);
extern void enable_paging(void);

/*
 * Kernel page directory template.
 *
 * Early boot builds an identity map of RAM into this directory and installs it
 * in CR3 before enabling paging.
 *
 * Later on, process directories clone PDEs from this template to share the
 * kernel half.
 */
uint32_t* kernel_page_directory = 0;

uint32_t page_dir[1024] __attribute__((aligned(4096)));

/*
 * Paging updates are serialized.
 *
 * The kernel page tables are shared globally and can be modified concurrently
 * (e.g. VMM heap mappings, fixmap, AP startup). We keep the implementation
 * simple by guarding all table walks/updates with one lock.
 */
static spinlock_t paging_lock;

static uint32_t paging_ram_size_bytes = 0;

#define PAGING_FIXMAP_BASE 0xFFFFE000u
#define PAGING_FIXMAP_SLOTS 16u


typedef struct {
    volatile uintptr_t key;
    volatile uintptr_t lock;
} paging_dir_lock_entry_t;

#define PAGING_DIR_LOCK_EMPTY 0u
#define PAGING_DIR_LOCK_TOMBSTONE 1u

#define PAGING_DIR_LOCK_TABLE_SIZE 1024u
#define PAGING_DIR_LOCK_TABLE_MASK (PAGING_DIR_LOCK_TABLE_SIZE - 1u)

static paging_dir_lock_entry_t paging_dir_lock_table[PAGING_DIR_LOCK_TABLE_SIZE];
static spinlock_t paging_dir_lock_write_lock;

__attribute__((noreturn)) static void paging_halt(const char* msg);

static inline spinlock_t* paging_select_dir_lock(uint32_t* dir) {
    if (!dir) {
        return &paging_lock;
    }

    if (dir == kernel_page_directory) {
        return &paging_lock;
    }

    const uintptr_t key = (uintptr_t)dir;
    uint32_t idx = (uint32_t)((key >> 12) * 2654435761u) & PAGING_DIR_LOCK_TABLE_MASK;

    for (uint32_t probe = 0; probe < PAGING_DIR_LOCK_TABLE_SIZE; probe++) {
        paging_dir_lock_entry_t* e = &paging_dir_lock_table[(idx + probe) & PAGING_DIR_LOCK_TABLE_MASK];

        const uintptr_t ekey = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (ekey == PAGING_DIR_LOCK_EMPTY) {
            break;
        }

        if (ekey == key) {
            const uintptr_t lock_ptr = __atomic_load_n(&e->lock, __ATOMIC_RELAXED);
            if (lock_ptr != 0u) {
                return (spinlock_t*)lock_ptr;
            }
            break;
        }
    }

    cpu_t* cpu = cpu_current();
    task_t* t = cpu ? cpu->current_task : 0;
    proc_mem_t* mem = t ? t->mem : 0;

    if (mem && mem->page_dir == dir) {
        return &mem->pt_lock;
    }

    return &paging_lock;
}

int paging_register_dir_lock(uint32_t* dir, void* lock) {
    if (!dir || !lock) {
        return 0;
    }

    if (dir == kernel_page_directory) {
        return 0;
    }

    const uintptr_t key = (uintptr_t)dir;
    const uintptr_t lock_ptr = (uintptr_t)lock;

    uint32_t int_flags = spinlock_acquire_safe(&paging_dir_lock_write_lock);

    uint32_t idx = (uint32_t)((key >> 12) * 2654435761u) & PAGING_DIR_LOCK_TABLE_MASK;
    uint32_t first_tombstone = PAGING_DIR_LOCK_TABLE_SIZE;

    for (uint32_t probe = 0; probe < PAGING_DIR_LOCK_TABLE_SIZE; probe++) {
        paging_dir_lock_entry_t* e = &paging_dir_lock_table[(idx + probe) & PAGING_DIR_LOCK_TABLE_MASK];

        const uintptr_t ekey = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (ekey == key) {
            __atomic_store_n(&e->lock, lock_ptr, __ATOMIC_RELAXED);
            spinlock_release_safe(&paging_dir_lock_write_lock, int_flags);
            return 1;
        }

        if (ekey == PAGING_DIR_LOCK_TOMBSTONE && first_tombstone == PAGING_DIR_LOCK_TABLE_SIZE) {
            first_tombstone = (idx + probe) & PAGING_DIR_LOCK_TABLE_MASK;
            continue;
        }

        if (ekey == PAGING_DIR_LOCK_EMPTY) {
            const uint32_t slot = (first_tombstone != PAGING_DIR_LOCK_TABLE_SIZE)
                ? first_tombstone
                : ((idx + probe) & PAGING_DIR_LOCK_TABLE_MASK);

            paging_dir_lock_entry_t* dst = &paging_dir_lock_table[slot];

            __atomic_store_n(&dst->lock, lock_ptr, __ATOMIC_RELAXED);
            __atomic_store_n(&dst->key, key, __ATOMIC_RELEASE);

            spinlock_release_safe(&paging_dir_lock_write_lock, int_flags);
            return 1;
        }
    }

    spinlock_release_safe(&paging_dir_lock_write_lock, int_flags);
    return 0;
}

void paging_unregister_dir_lock(uint32_t* dir) {
    if (!dir) {
        return;
    }

    if (dir == kernel_page_directory) {
        return;
    }

    const uintptr_t key = (uintptr_t)dir;

    uint32_t int_flags = spinlock_acquire_safe(&paging_dir_lock_write_lock);

    uint32_t idx = (uint32_t)((key >> 12) * 2654435761u) & PAGING_DIR_LOCK_TABLE_MASK;

    for (uint32_t probe = 0; probe < PAGING_DIR_LOCK_TABLE_SIZE; probe++) {
        paging_dir_lock_entry_t* e = &paging_dir_lock_table[(idx + probe) & PAGING_DIR_LOCK_TABLE_MASK];

        const uintptr_t ekey = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (ekey == PAGING_DIR_LOCK_EMPTY) {
            break;
        }

        if (ekey == key) {
            __atomic_store_n(&e->lock, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&e->key, PAGING_DIR_LOCK_TOMBSTONE, __ATOMIC_RELEASE);
            break;
        }
    }

    spinlock_release_safe(&paging_dir_lock_write_lock, int_flags);
}

static inline uint32_t paging_lock_dir_safe(uint32_t* dir) {
    return spinlock_acquire_safe(paging_select_dir_lock(dir));
}

static inline void paging_unlock_dir_safe(uint32_t* dir, uint32_t flags) {
    spinlock_release_safe(paging_select_dir_lock(dir), flags);
}

/*
 * Fixmap.
 *
 * Once paging is enabled, the kernel cannot blindly dereference a physical
 * address. For early boot helpers (zeroing page tables, etc.) we reserve a
 * small per-CPU virtual window (PAGING_FIXMAP_SLOTS) and map one physical page
 * there on demand.
 *
 * The fixmap is backed by the kernel page directory, so it is available
 * regardless of the currently active user directory.
 */

static inline uint32_t* paging_pt_virt(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    if ((dir[pd_idx] & 1u) == 0u) {
        return 0;
    }

    if ((dir[pd_idx] & (1u << 7)) != 0u) {
        return 0;
    }

    return (uint32_t*)(dir[pd_idx] & ~0xFFFu);
}

static inline int paging_pde_pt_phys_valid(uint32_t pde);

static inline void paging_tlb_flush_range_local(uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    start &= ~0xFFFu;
    end = (end + 0xFFFu) & ~0xFFFu;

    const uint32_t pages = (end - start) >> 12;
    if (pages > 16u) {
        uint32_t cr3;
        __asm__ volatile(
            "mov %%cr3, %0\n\t"
            "mov %0, %%cr3"
            : "=r"(cr3)
            :
            : "memory"
        );

        return;
    }

    for (uint32_t addr = start; addr < end; addr += 0x1000u) {
        __asm__ volatile("invlpg (%0)" :: "r" (addr) : "memory");
    }
}

void paging_unmap_range_ex(
    uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr,
    paging_unmap_visitor_t visitor, void* visitor_ctx
) {
    if (!dir) {
        return;
    }

    if (end_vaddr <= start_vaddr) {
        return;
    }

    const uint32_t start = start_vaddr & ~0xFFFu;
    const uint32_t end = (end_vaddr + 0xFFFu) & ~0xFFFu;

    if (end <= start) {
        return;
    }

    uint32_t int_flags = paging_lock_dir_safe(dir);

    int any_unmapped = 0;

    for (uint32_t virt = start; virt < end; virt += 0x1000u) {
        const uint32_t pd_idx = virt >> 22;

        const uint32_t pde = dir[pd_idx];
        if ((pde & 1u) == 0u) {
            continue;
        }

        if ((pde & (1u << 7)) != 0u) {
            if (visitor) {
                if (!visitor(virt & ~0x3FFFFFu, pde, visitor_ctx)) {
                    virt = (virt & ~0x3FFFFFu) + 0x400000u - 0x1000u;
                    continue;
                }
            }
            
            any_unmapped = 1;

            dir[pd_idx] = 0;
            virt = (virt & ~0x3FFFFFu) + 0x400000u - 0x1000u;
            
            continue;
        }

        if (!paging_pde_pt_phys_valid(pde)) {
            continue;
        }

        uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
        const uint32_t pt_idx = (virt >> 12) & 0x3FFu;

        const uint32_t pte = pt[pt_idx];
        if ((pte & 1u) == 0u) {
            continue;
        }

        if (visitor) {
            if (!visitor(virt, pte, visitor_ctx)) {
                continue;
            }
        }

        pt[pt_idx] = 0u;
        any_unmapped = 1;
    }

    paging_unlock_dir_safe(dir, int_flags);

    if (!any_unmapped) {
        return;
    }

    if (dir == kernel_page_directory) {
        smp_tlb_shootdown_range(start, end);
    }

    paging_tlb_flush_range_local(start_vaddr, end_vaddr);

    cpu_t* me = cpu_current();
    if (me && me->current_task && me->current_task->mem && me->current_task->mem->refcount > 1) {
        smp_tlb_shootdown_range_dir(dir, start_vaddr, end_vaddr);
    }
}

void paging_unmap_range(uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr) {
    paging_unmap_range_ex(dir, start_vaddr, end_vaddr, 0, 0);
}

void paging_unmap_range_no_tlb(
    uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr,
    paging_unmap_visitor_t visitor, void* visitor_ctx
) {
    if (!dir) return;
    if (end_vaddr <= start_vaddr) return;

    const uint32_t start = start_vaddr & ~0xFFFu;
    const uint32_t end = (end_vaddr + 0xFFFu) & ~0xFFFu;
    
    if (end <= start) return;

    uint32_t int_flags = paging_lock_dir_safe(dir);

    for (uint32_t virt = start; virt < end; virt += 0x1000u) {
        const uint32_t pd_idx = virt >> 22;

        const uint32_t pde = dir[pd_idx];
        if ((pde & 1u) == 0u) continue;

        if ((pde & (1u << 7)) != 0u) {
            if (visitor) {
                if (!visitor(virt & ~0x3FFFFFu, pde, visitor_ctx)) {
                    virt = (virt & ~0x3FFFFFu) + 0x400000u - 0x1000u;
                    continue;
                }
            }

            dir[pd_idx] = 0;
            virt = (virt & ~0x3FFFFFu) + 0x400000u - 0x1000u;
            continue;
        }
        
        if (!paging_pde_pt_phys_valid(pde)) continue;

        uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
        const uint32_t pt_idx = (virt >> 12) & 0x3FFu;

        const uint32_t pte = pt[pt_idx];
        if ((pte & 1u) == 0u) continue;

        if (visitor) {
            if (!visitor(virt, pte, visitor_ctx)) {
                continue;
            }
        }

        pt[pt_idx] = 0u;
    }

    paging_unlock_dir_safe(dir, int_flags);
}

static inline int paging_pde_pt_phys_valid(uint32_t pde) {
    /*
     * Defensive validation for pointers coming from page tables.
     *
     * We only expect 4KiB page tables here (no PSE), and we reject obviously
     * bogus PT addresses.
     */
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
        /*
         * Per-CPU slot selection keeps fixmap usage contention-free for the
         * common case (each CPU clears/initializes its own freshly allocated
         * tables).
         */
        idx = (uint32_t)cpu->index;
    }

    return PAGING_FIXMAP_BASE - idx * 4096u;
}

static void paging_fixmap_set(uint32_t virt, uint32_t phys) {
    uint32_t* pt = paging_pt_virt(kernel_page_directory, virt);
    if (!pt) {
        paging_halt("paging_fixmap_set: missing PT");
    }

    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    pt[pt_idx] = (phys & ~0xFFFu) | PTE_PRESENT | PTE_RW;

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

static void paging_fixmap_clear(uint32_t virt) {
    uint32_t* pt = paging_pt_virt(kernel_page_directory, virt);
    if (!pt) {
        paging_halt("paging_fixmap_clear: missing PT");
    }

    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    pt[pt_idx] = 0;

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

static inline int paging_is_enabled(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return ((cr0 & (1u << 31)) != 0u) ? 1 : 0;
}

void paging_zero_phys_page(uint32_t phys) {
    /*
     * This is used for freshly allocated page tables/directories.
     *
     * Before paging is enabled, the kernel still runs with a trivial physical
     * identity map, so phys is directly writable. After paging, we bounce
     * through the fixmap.
     */
    if ((phys & 0xFFFu) != 0u) {
        paging_halt("paging_zero_phys_page: unaligned phys");
    }

    if (!paging_is_enabled()) {
        memzero_nt_page((void*)phys);
        return;
    }

    if (!kernel_page_directory) {
        paging_halt("paging_zero_phys_page: kernel_page_directory not set");
    }

    uint32_t virt = paging_fixmap_virt();

    paging_fixmap_set(virt, phys);
    memzero_nt_page((void*)virt);
    paging_fixmap_clear(virt);
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
    /*
     * Keep PAT programming local and idempotent.
     *
     * APs call paging_init_pat() as part of their bring-up. The code patches one
     * PAT entry to WC and leaves the rest intact.
     */
    if (!paging_pat_is_supported()) {
        return;
    }

    uint64_t pat = paging_rdmsr_u64(IA32_PAT_MSR);
    uint64_t new_pat = pat;

    /*
     * Replace one PAT entry with WC.
     *
     * The exact entry index is part of the kernel ABI with itself; mapping code
     * is expected to set the corresponding PTE bits when WC is desired.
     */
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
    /* Write back + invalidate caches. Required by the MTRR programming rules. */
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
    /*
     * Variable MTRRs are global CPU state.
     *
     * The code follows the standard sequence:
     *  - disable caching (CR0.CD=1, CR0.NW=0) + wbinvd
     *  - disable MTRRs via IA32_MTRR_DEF_TYPE
     *  - program one or more free variable MTRRs
     *  - restore IA32_MTRR_DEF_TYPE and caching state
     *
     * If the system has no free variable MTRRs, we silently do nothing.
     */
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

        /* Variable MTRRs require a power-of-two size aligned to its size. */
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

void paging_map_ex(
    uint32_t* dir, uint32_t virt, uint32_t phys,
    uint32_t flags, uint32_t map_flags
) {
    /*
     * Map a single 4KiB page.
     *
     * The implementation allocates page tables on demand. Page directories and
     * page tables are assumed to live in identity-mapped memory at this stage.
     */
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    void* new_pt_phys = 0;
    uint32_t int_flags = paging_lock_dir_safe(dir);

    if ((dir[pd_idx] & 1u) == 0u) {
        paging_unlock_dir_safe(dir, int_flags);

        new_pt_phys = pmm_alloc_block();
        if (!new_pt_phys) {
            paging_halt("pmm_alloc_block failed in paging_map");
        }

        paging_zero_phys_page((uint32_t)new_pt_phys);

        int_flags = paging_lock_dir_safe(dir);
        if ((dir[pd_idx] & 1u) == 0u) {
            /* Present + RW + USER: kernel and userspace can share PDEs policy-wise. */
            dir[pd_idx] = ((uint32_t)new_pt_phys) | 7u;
            new_pt_phys = 0;
        }
    }

    /* PT address comes from the PDE. This assumes identity-mapped tables. */
    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFFu);
    pt[pt_idx] = (phys & ~0xFFFu) | flags;

    paging_unlock_dir_safe(dir, int_flags);

    if (new_pt_phys) {
        pmm_free_block(new_pt_phys);
    }

    if ((map_flags & PAGING_MAP_NO_TLB_FLUSH) != 0u) {
        return;
    }

    if (dir == kernel_page_directory) {
        /*
         * Kernel mappings are global: other CPUs may have this address cached.
         *
         * The shootdown handler is expected to invalidate the mapping on the
         * remote CPUs, and also take care of the local invalidation.
         */
        smp_tlb_shootdown(virt);
        return;
    }

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    paging_map_ex(dir, virt, phys, flags, 0u);
}

void paging_map_batch(
    uint32_t* dir, uint32_t virt_start,
    const uint32_t* phys_array, uint32_t count,
    uint32_t flags, uint32_t map_flags
) {
    if (count == 0 || !dir || !phys_array) {
        return;
    }

    uint32_t pages_done = 0;

    while (pages_done < count) {
        uint32_t virt = virt_start + (pages_done << 12);
        uint32_t pd_idx = virt >> 22;
        uint32_t pt_idx = (virt >> 12) & 0x3FFu;

        uint32_t chunk_size = 1024u - pt_idx;
        if (chunk_size > count - pages_done) {
            chunk_size = count - pages_done;
        }

        void* new_pt_phys = 0;
        uint32_t int_flags = paging_lock_dir_safe(dir);

        if ((dir[pd_idx] & 1u) == 0u) {
            paging_unlock_dir_safe(dir, int_flags);

            new_pt_phys = pmm_alloc_block();
            if (!new_pt_phys) {
                paging_halt("pmm_alloc_block failed in paging_map_batch");
            }
            paging_zero_phys_page((uint32_t)new_pt_phys);

            int_flags = paging_lock_dir_safe(dir);
            if ((dir[pd_idx] & 1u) == 0u) {
                dir[pd_idx] = ((uint32_t)new_pt_phys) | 7u;
                new_pt_phys = 0;
            }
        }

        uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFFu);
        for (uint32_t i = 0; i < chunk_size; i++) {
            uint32_t phys = phys_array[pages_done + i];
            pt[pt_idx + i] = (phys & ~0xFFFu) | flags;
        }

        paging_unlock_dir_safe(dir, int_flags);

        if (new_pt_phys) {
            pmm_free_block(new_pt_phys);
        }

        if ((map_flags & PAGING_MAP_NO_TLB_FLUSH) == 0u) {
            if (dir == kernel_page_directory) {
                smp_tlb_shootdown_range(virt, virt + (chunk_size << 12));
            } else {
                for (uint32_t i = 0; i < chunk_size; i++) {
                    __asm__ volatile("invlpg (%0)" :: "r" (virt + (i << 12)) : "memory");
                }
            }
        }

        pages_done += chunk_size;
    }
}

void paging_map_4m(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    if (!dir) return;

    uint32_t pd_idx = virt >> 22;
    uint32_t int_flags = paging_lock_dir_safe(dir);

    dir[pd_idx] = (phys & ~0x3FFFFFu) | flags | (1u << 7);

    paging_unlock_dir_safe(dir, int_flags);

    if (dir == kernel_page_directory) {
        smp_tlb_shootdown_range(virt, virt + 0x400000u);
    } else {
        __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");
    }
}

static void paging_allocate_table(uint32_t virt) {
    /*
     * Ensure the kernel directory has a page table for `virt`.
     *
     * This is used to pre-create the fixmap tables and a kernel window used by
     * higher-level allocators.
     */
    uint32_t pd_idx = virt >> 22;
    if ((kernel_page_directory[pd_idx] & 1u) != 0u) {
        return;
    }

    void* new_pt_phys = pmm_alloc_block();
    if (!new_pt_phys) {
        return;
    }

    paging_zero_phys_page((uint32_t)new_pt_phys);

    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);
    if ((kernel_page_directory[pd_idx] & 1u) == 0u) {
        kernel_page_directory[pd_idx] = ((uint32_t)new_pt_phys) | 7u;
        new_pt_phys = 0;
    }
    spinlock_release_safe(&paging_lock, int_flags);

    if (new_pt_phys) {
        pmm_free_block(new_pt_phys);
    }
}

void paging_init(uint32_t ram_size_bytes) {
    /*
     * Early identity map.
     *
     * Boot code expects RAM to be reachable by physical addresses while we are
     * still bringing the memory management up. We map [0..ram_size_bytes) as
     * supervisor RW, then switch to this directory and enable paging.
     */
    paging_init_pat();
    spinlock_init(&paging_lock);
    spinlock_init(&paging_dir_lock_write_lock);

    if (ram_size_bytes & 0xFFFu) {
        ram_size_bytes = (ram_size_bytes & ~0xFFFu) + 4096u;
    }
    paging_ram_size_bytes = ram_size_bytes;

    kernel_page_directory = page_dir;

    for(int i = 0; i < 1024; i++) {
        /*
         * Default to supervisor, read-only, not-present (bit1=RW).
         *
         * This keeps the directory clean while still allowing a cheap
         * non-zero sentinel for debugging.
         */
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

            /*
             * PDE flags here mirror the basic kernel mapping policy:
             * present + RW, supervisor-only.
             */
            page_dir[pd_idx] = ((uint32_t)pt_phys) | 3; /* Supervisor | RW | Present */
        }
        
        /*
         * Populate identity mapping at 4KiB granularity.
         *
         * This intentionally does not use 4MiB pages (PSE) to keep the
         * implementation uniform with later fine-grained mappings.
         */
        uint32_t* pt = (uint32_t*)(page_dir[pd_idx] & ~0xFFF);
        pt[pt_idx] = i | 3; /* Supervisor | RW | Present */
        
        if (i + 4096 < i) break;
    }

    /* Local APIC MMIO is accessed via a fixed physical address on x86. */
    paging_map(page_dir, 0xFEE00000, 0xFEE00000, PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT);

    for (uint32_t i = 0; i < PAGING_FIXMAP_SLOTS; i++) {
        uint32_t virt = PAGING_FIXMAP_BASE - i * 4096u;

        /* Ensure fixmap slots have PTs pre-allocated before paging turns on. */
        paging_allocate_table(virt);
        paging_fixmap_clear(virt);
    }
    
    for(uint32_t addr = 0xC0000000; addr != 0; addr += 0x400000) {
        /* Pre-allocate kernel PTs for the heap / early dynamic mappings. */
        paging_allocate_table(addr);
    }

    paging_switch(page_dir);
    enable_paging();
}

void paging_switch(uint32_t* dir_phys) {
    /* CR3 expects a physical address aligned to 4KiB. */
    load_page_directory(dir_phys);
}

uint32_t* paging_get_dir(void) {
    return read_cr3();
}

uint32_t* paging_clone_directory(void) {
    /*
     * Clone only present PDEs from the kernel template.
     *
     * This creates a new address space that shares the kernel half with the
     * global directory.
     */
    uint32_t* new_dir = (uint32_t*)pmm_alloc_block();
    if (!new_dir) return 0;

    paging_zero_phys_page((uint32_t)new_dir);

    for (int i = 0; i < 1024; i++) {
        if (kernel_page_directory[i] & 1) {
            new_dir[i] = kernel_page_directory[i];
        }
    }
    return new_dir;
}

int paging_is_user_accessible(uint32_t* dir, uint32_t virt) {
    /*
     * A mapping is considered user-accessible only if both PDE and PTE have the
     * user bit set.
     */
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) return 0;
    if (!(dir[pd_idx] & 4)) return 0;

    if ((dir[pd_idx] & (1u << 7)) != 0u) {
        /* 4MiB PDE (PSE): user bit is checked at the PDE level. */
        return 1;
    }

    uint32_t pde = dir[pd_idx];
    if (!paging_pde_pt_phys_valid(pde)) return 0;

    uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
    if (!(pt[pt_idx] & 1)) return 0;
    if (!(pt[pt_idx] & 4)) return 0;

    return 1;
}

uint32_t paging_get_phys(uint32_t* dir, uint32_t virt) {
    /* Translate a virtual address under a specific directory. */
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = dir[pd_idx];
    if ((pde & 1u) == 0u) return 0;

    if ((pde & (1u << 7)) != 0u) {
        /* 4MiB page: physical base is in 4MiB units; offset is low 22 bits. */
        uint32_t base = pde & 0xFFC00000u;
        return base + (virt & 0x003FFFFFu);
    }

    if (!paging_pde_pt_phys_valid(pde)) return 0;

    uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
    uint32_t pte = pt[pt_idx];
    if ((pte & 1u) == 0u) return 0;

    return (pte & ~0xFFFu) + (virt & 0xFFFu);
}

int paging_get_present_pte(uint32_t* dir, uint32_t virt, uint32_t* out_pte) {
    if (!dir) {
        return 0;
    }

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;

    uint32_t pde = dir[pd_idx];
    if ((pde & 1u) == 0u) {
        return 0;
    }

    if ((pde & (1u << 7)) != 0u) {
        return 0;
    }

    if (!paging_pde_pt_phys_valid(pde)) {
        return 0;
    }

    uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
    uint32_t pte = pt[pt_idx];
    if ((pte & 1u) == 0u) {
        return 0;
    }

    if (out_pte) {
        *out_pte = pte;
    }

    return 1;
}