#include <kernel/symbols.h>

#include <kernel/ksyms.h>

#include <kernel/boot.h>

#include <stdint.h>

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

enum {
    SHT_SYMTAB = 2u,
};

enum {
    SHN_UNDEF = 0u,
};

static const Elf32_Sym* g_symtab;
static uint32_t g_sym_count;

static const char* g_strtab;
static uint32_t g_strtab_size;

__attribute__((no_instrument_function))
static inline uint8_t elf32_sym_type(uint8_t info) {
    return info & 0x0Fu;
}

__attribute__((no_instrument_function))
static inline int sym_is_func(const Elf32_Sym* s) {
    return elf32_sym_type(s->st_info) == 2u;
}

__attribute__((no_instrument_function))
static inline int sym_is_defined(const Elf32_Sym* s) {
    return s->st_shndx != SHN_UNDEF;
}

__attribute__((no_instrument_function))
static inline uint32_t sym_addr(const Elf32_Sym* s) {
    return s->st_value;
}

__attribute__((no_instrument_function))
static inline const char* sym_name(const Elf32_Sym* s) {
    if (!g_strtab || s->st_name >= g_strtab_size) {
        return 0;
    }

    const char* n = &g_strtab[s->st_name];
    return *n ? n : 0;
}

__attribute__((no_instrument_function))
static const Elf32_Sym* find_best_symbol(uint32_t addr) {
    const Elf32_Sym* best = 0;
    uint32_t best_addr = 0;

    if (!g_symtab || g_sym_count == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < g_sym_count; ++i) {
        const Elf32_Sym* s = &g_symtab[i];

        if (!sym_is_defined(s) || !sym_is_func(s)) {
            continue;
        }

        const uint32_t a = sym_addr(s);
        if (a == 0u || a > addr) {
            continue;
        }

        if (!best || a > best_addr) {
            best = s;
            best_addr = a;
        }
    }

    return best;
}

__attribute__((no_instrument_function))
void symbols_init(const multiboot_info_t* mb) {
    g_symtab = 0;
    g_sym_count = 0;
    g_strtab = 0;
    g_strtab_size = 0;

    if (!mb) {
        return;
    }

    const multiboot_info_t* info = mb;
    if ((info->flags & (1u << 11)) == 0u) {
        return;
    }

    if (info->elf_num == 0 || info->elf_size == 0 || info->elf_addr == 0) {
        return;
    }

    const Elf32_Shdr* shdrs = (const Elf32_Shdr*)(uint32_t)info->elf_addr;

    for (uint32_t i = 0; i < info->elf_num; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)((const uint8_t*)shdrs + i * info->elf_size);

        if (sh->sh_type != SHT_SYMTAB) {
            continue;
        }

        if (sh->sh_entsize == 0 || sh->sh_size < sh->sh_entsize) {
            continue;
        }

        if (sh->sh_link >= info->elf_num) {
            continue;
        }

        const Elf32_Shdr* str_sh = (const Elf32_Shdr*)((const uint8_t*)shdrs + sh->sh_link * info->elf_size);

        g_symtab = (const Elf32_Sym*)(uint32_t)sh->sh_addr;
        g_sym_count = sh->sh_size / sh->sh_entsize;

        g_strtab = (const char*)(uint32_t)str_sh->sh_addr;
        g_strtab_size = str_sh->sh_size;

        return;
    }
}

__attribute__((no_instrument_function))
const char* symbols_resolve(uint32_t addr, uint32_t* out_sym_addr) {
    if (out_sym_addr) {
        *out_sym_addr = 0;
    }

    const char* kname = ksyms_resolve(addr, out_sym_addr);
    if (kname) {
        return kname;
    }

    const Elf32_Sym* best = find_best_symbol(addr);
    if (!best) {
        return 0;
    }

    const char* name = sym_name(best);
    if (!name) {
        return 0;
    }

    if (out_sym_addr) {
        *out_sym_addr = sym_addr(best);
    }

    return name;
}
