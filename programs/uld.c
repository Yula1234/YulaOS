// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234
// ULD - Micro Linker 

#include <yula.h>

#define MAX_OBJECTS     64
#define MAX_SYMBOLS     4096
#define BASE_ADDR       0x08048000
#define PAGE_ALIGN      4096
#define SECT_ALIGN      16
#define SHN_UNDEF 0

 #define SHT_REL 9

 #define MAX_RODATA_SECTIONS 64
 #define MAX_REL_SECTIONS    64

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_386  3
#define R_386_32   1
#define R_386_PC32 2
#define ELF32_R_SYM(i)    ((i)>>8)
#define ELF32_R_TYPE(i)   ((unsigned char)(i))

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    Elf32_Word    p_type;
    Elf32_Off     p_offset;
    Elf32_Addr    p_vaddr;
    Elf32_Addr    p_paddr;
    Elf32_Word    p_filesz;
    Elf32_Word    p_memsz;
    Elf32_Word    p_flags;
    Elf32_Word    p_align;
} __attribute__((packed)) Elf32_Phdr;

typedef struct {
    Elf32_Word    sh_name;
    Elf32_Word    sh_type;
    Elf32_Word    sh_flags;
    Elf32_Addr    sh_addr;
    Elf32_Off     sh_offset;
    Elf32_Word    sh_size;
    Elf32_Word    sh_link;
    Elf32_Word    sh_info;
    Elf32_Word    sh_addralign;
    Elf32_Word    sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    Elf32_Addr    r_offset;
    Elf32_Word    r_info;
} __attribute__((packed)) Elf32_Rel;

typedef struct {
    char name[64];
    uint8_t* raw_data;
    uint32_t raw_size;
    
    Elf32_Ehdr* ehdr;
    Elf32_Shdr* shdrs;
    
    Elf32_Shdr* sh_text;
    Elf32_Shdr* sh_rodata[MAX_RODATA_SECTIONS];
    int rodata_count;
    Elf32_Shdr* sh_data;
    Elf32_Shdr* sh_bss;
    Elf32_Shdr* sh_symtab;
    Elf32_Shdr* sh_strtab;
    Elf32_Shdr* sh_rel_text;
    Elf32_Shdr* sh_rel_data;

     Elf32_Shdr* sh_rel[MAX_REL_SECTIONS];
     int rel_count;
    
    uint32_t text_out_offset;
    uint32_t rodata_out_offset[MAX_RODATA_SECTIONS];
    uint32_t data_out_offset;
    uint32_t bss_out_offset;
} ObjectFile;

typedef struct {
    char name[64];
    uint32_t value;
    int defined;
} GlobalSymbol;

typedef struct {
    ObjectFile* objects[MAX_OBJECTS];
    int obj_count;
    
    GlobalSymbol symbols[MAX_SYMBOLS];
    int sym_count;
    
    uint32_t total_text_size;
    uint32_t total_rodata_size;
    uint32_t total_data_size;
    uint32_t total_bss_size;
    
    uint32_t entry_addr;
    
    uint8_t* out_buffer;
    uint32_t out_size;
} LinkerCtx;

void fatal(const char* fmt, ...) {
    set_console_color(0xF44747, 0x141414);
    printf("\n[LINKER ERROR] ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    set_console_color(0xD4D4D4, 0x141414);
    exit(1);
}

void* safe_malloc(uint32_t size) {
    void* p = malloc(size);
    if (!p) fatal("Out of memory");
    memset(p, 0, size);
    return p;
}

const char* get_str(ObjectFile* obj, uint32_t offset) {
    if (!obj->sh_strtab) return "";
    return (const char*)(obj->raw_data + obj->sh_strtab->sh_offset + offset);
}

ObjectFile* load_object(const char* filename) {
    int fd = open(filename, 0);
    if (fd < 0) fatal("Cannot open file: %s", filename);
    
    ObjectFile* obj = safe_malloc(sizeof(ObjectFile));
    strcpy(obj->name, filename);
    
    obj->raw_data = safe_malloc(65536); 
    obj->raw_size = read(fd, obj->raw_data, 65536);
    close(fd);
    
    if (obj->raw_size < sizeof(Elf32_Ehdr)) fatal("File too small: %s", filename);
    
    obj->ehdr = (Elf32_Ehdr*)obj->raw_data;
    if (obj->ehdr->e_ident[0] != 0x7F || obj->ehdr->e_ident[1] != 'E') 
        fatal("Not an ELF file: %s", filename);
        
    obj->shdrs = (Elf32_Shdr*)(obj->raw_data + obj->ehdr->e_shoff);
    
    Elf32_Shdr* sh_shstr = &obj->shdrs[obj->ehdr->e_shstrndx];
    char* strtab = (char*)(obj->raw_data + sh_shstr->sh_offset);
    
    for (int i = 0; i < obj->ehdr->e_shnum; i++) {
        Elf32_Shdr* sh = &obj->shdrs[i];
        const char* name = strtab + sh->sh_name;
        
        if (strcmp(name, ".text") == 0) obj->sh_text = sh;
        else if (strncmp(name, ".rodata", 7) == 0) {
            if (obj->rodata_count < MAX_RODATA_SECTIONS) {
                obj->sh_rodata[obj->rodata_count++] = sh;
            }
        }
        else if (strcmp(name, ".data") == 0) obj->sh_data = sh;
        else if (strcmp(name, ".bss") == 0) obj->sh_bss = sh;
        else if (strcmp(name, ".symtab") == 0) obj->sh_symtab = sh;
        else if (strcmp(name, ".strtab") == 0) obj->sh_strtab = sh;
        else if (strcmp(name, ".rel.text") == 0) obj->sh_rel_text = sh;
        else if (strcmp(name, ".rel.data") == 0) obj->sh_rel_data = sh;

        if (sh->sh_type == SHT_REL) {
            if (obj->rel_count < MAX_REL_SECTIONS) {
                obj->sh_rel[obj->rel_count++] = sh;
            }
        }
    }
    
    return obj;
}

void calculate_layout(LinkerCtx* ctx) {
    uint32_t text_off = 0, rodata_off = 0, data_off = 0, bss_off = 0;
    
    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        if (obj->sh_text) {
            text_off = (text_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);
            obj->text_out_offset = text_off;
            text_off += obj->sh_text->sh_size;
        }
    }
    ctx->total_text_size = (text_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);

    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        for (int k = 0; k < obj->rodata_count; k++) {
            rodata_off = (rodata_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);
            obj->rodata_out_offset[k] = rodata_off;
            rodata_off += obj->sh_rodata[k]->sh_size;
        }
    }
    ctx->total_rodata_size = (rodata_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);

    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        if (obj->sh_data) {
            data_off = (data_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);
            obj->data_out_offset = data_off;
            data_off += obj->sh_data->sh_size;
        }
    }
    ctx->total_data_size = (data_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);

    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        if (obj->sh_bss) {
            bss_off = (bss_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);
            obj->bss_out_offset = bss_off;
            bss_off += obj->sh_bss->sh_size;
        }
    }
    ctx->total_bss_size = (bss_off + (SECT_ALIGN-1)) & ~(SECT_ALIGN-1);
}

GlobalSymbol* find_global(LinkerCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->sym_count; i++) {
        if (strcmp(ctx->symbols[i].name, name) == 0) return &ctx->symbols[i];
    }
    return 0;
}

void collect_symbols(LinkerCtx* ctx) {
    uint32_t base_text = BASE_ADDR + sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    uint32_t base_rodata = base_text + ctx->total_text_size;
    uint32_t base_data = base_rodata + ctx->total_rodata_size;
    uint32_t base_bss  = base_data + ctx->total_data_size;
    
    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        if (!obj->sh_symtab) continue;
        
        Elf32_Sym* syms = (Elf32_Sym*)(obj->raw_data + obj->sh_symtab->sh_offset);
        int count = obj->sh_symtab->sh_size / sizeof(Elf32_Sym);
        
        for (int k = 0; k < count; k++) {
            Elf32_Sym* s = &syms[k];
            unsigned char bind = (s->st_info >> 4);
            
            if (bind == 1 && s->st_shndx != SHN_UNDEF) { 
                const char* name = get_str(obj, s->st_name);
                
                if (find_global(ctx, name)) continue;
                
                GlobalSymbol* gs = &ctx->symbols[ctx->sym_count++];
                strcpy(gs->name, name);
                gs->defined = 1;
                
                uint32_t section_base = 0;
                Elf32_Shdr* sec = &obj->shdrs[s->st_shndx];
                
                if (sec == obj->sh_text) section_base = base_text + obj->text_out_offset;
                else if (sec == obj->sh_data) section_base = base_data + obj->data_out_offset;
                else if (sec == obj->sh_bss)  section_base = base_bss  + obj->bss_out_offset;
                else {
                    for (int r = 0; r < obj->rodata_count; r++) {
                        if (sec == obj->sh_rodata[r]) {
                            section_base = base_rodata + obj->rodata_out_offset[r];
                            break;
                        }
                    }
                }
                
                gs->value = section_base + s->st_value;
                if (strcmp(name, "_start") == 0) ctx->entry_addr = gs->value;
            }
        }
    }
}

void apply_relocations(LinkerCtx* ctx, ObjectFile* obj, Elf32_Shdr* sh_rel) {
    if (!sh_rel) return;
    if (!obj->sh_symtab) return;
    
    if (sh_rel->sh_info >= (uint32_t)obj->ehdr->e_shnum) return;

    uint32_t base_text = BASE_ADDR + sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    uint32_t base_rodata = base_text + ctx->total_text_size;
    uint32_t base_data = base_rodata + ctx->total_rodata_size;
    uint32_t base_bss  = base_data + ctx->total_data_size;

    uint32_t headers_sz = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);

    Elf32_Shdr* target = &obj->shdrs[sh_rel->sh_info];

    uint32_t section_base_addr = 0;
    uint8_t* buffer_ptr = 0;

    if (target == obj->sh_text) {
        section_base_addr = base_text + obj->text_out_offset;
        buffer_ptr = ctx->out_buffer + headers_sz + obj->text_out_offset;
    } else if (target == obj->sh_data) {
        section_base_addr = base_data + obj->data_out_offset;
        buffer_ptr = ctx->out_buffer + headers_sz + ctx->total_text_size + ctx->total_rodata_size + obj->data_out_offset;
    } else if (target == obj->sh_bss) {
        return;
    } else {
        for (int r = 0; r < obj->rodata_count; r++) {
            if (target == obj->sh_rodata[r]) {
                section_base_addr = base_rodata + obj->rodata_out_offset[r];
                buffer_ptr = ctx->out_buffer + headers_sz + ctx->total_text_size + obj->rodata_out_offset[r];
                break;
            }
        }
    }

    if (!buffer_ptr) return;

    if (sh_rel->sh_offset + sh_rel->sh_size > obj->raw_size) {
        fatal("Corrupt relocation section (offset/size out of file) in %s", obj->name);
    }
    if (obj->sh_symtab->sh_offset + obj->sh_symtab->sh_size > obj->raw_size) {
        fatal("Corrupt symtab section (offset/size out of file) in %s", obj->name);
    }
    if (obj->sh_strtab && (obj->sh_strtab->sh_offset + obj->sh_strtab->sh_size > obj->raw_size)) {
        fatal("Corrupt strtab section (offset/size out of file) in %s", obj->name);
    }

    Elf32_Rel* rels = (Elf32_Rel*)(obj->raw_data + sh_rel->sh_offset);
    int count = sh_rel->sh_size / sizeof(Elf32_Rel);
    Elf32_Sym* syms = (Elf32_Sym*)(obj->raw_data + obj->sh_symtab->sh_offset);
    int sym_count = (int)(obj->sh_symtab->sh_size / sizeof(Elf32_Sym));

    const char* rel_name = "<rel>";
    if (obj->ehdr->e_shstrndx < obj->ehdr->e_shnum) {
        Elf32_Shdr* sh_shstr = &obj->shdrs[obj->ehdr->e_shstrndx];
        if (sh_shstr->sh_offset < obj->raw_size) {
            const char* shstr = (const char*)(obj->raw_data + sh_shstr->sh_offset);
            rel_name = shstr + sh_rel->sh_name;
        }
    }

    for (int i = 0; i < count; i++) {
        Elf32_Rel* r = &rels[i];
        int type = ELF32_R_TYPE(r->r_info);
        int sym_idx = ELF32_R_SYM(r->r_info);

        if (type == 0) continue;
        if (type != R_386_32 && type != R_386_PC32) {
            fatal("Unsupported relocation type %d (r_info=0x%08x) in %s (%s)", type, r->r_info, obj->name, rel_name);
        }
        if (sym_idx < 0 || sym_idx >= sym_count) {
            fatal("Bad relocation symbol index %d/%d (r_info=0x%08x) in %s (%s)", sym_idx, sym_count, r->r_info, obj->name, rel_name);
        }
        if (r->r_offset + sizeof(uint32_t) > target->sh_size) {
            fatal("Relocation offset out of range (off=0x%08x, sec_size=0x%08x) in %s (%s)", r->r_offset, target->sh_size, obj->name, rel_name);
        }
        
        Elf32_Sym* s = &syms[sym_idx];
        uint32_t sym_val = 0;
        
        if (s->st_shndx == SHN_UNDEF) {
            const char* name = get_str(obj, s->st_name);
            GlobalSymbol* gs = find_global(ctx, name);
            if (!gs) fatal("Undefined reference to '%s' in %s", name, obj->name);
            sym_val = gs->value;
        } else {
            Elf32_Shdr* sec = &obj->shdrs[s->st_shndx];
            uint32_t s_base = 0;
            if (sec == obj->sh_text) s_base = base_text + obj->text_out_offset;
            else if (sec == obj->sh_data) s_base = base_data + obj->data_out_offset;
            else if (sec == obj->sh_bss)  s_base = base_bss  + obj->bss_out_offset;
            else {
                for (int r = 0; r < obj->rodata_count; r++) {
                    if (sec == obj->sh_rodata[r]) {
                        s_base = base_rodata + obj->rodata_out_offset[r];
                        break;
                    }
                }
            }
            sym_val = s_base + s->st_value;
        }
        
        uint32_t* patch_loc = (uint32_t*)(buffer_ptr + r->r_offset);
        uint32_t P = section_base_addr + r->r_offset;
        uint32_t S = sym_val;
        uint32_t A = *patch_loc;
        
        if (type == R_386_32) {
            *patch_loc = S + A;
        } else if (type == R_386_PC32) {
            *patch_loc = S + A - P;
        }
    }
}

void build_image(LinkerCtx* ctx, const char* outfile) {
    uint32_t headers_sz = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    uint32_t file_sz = headers_sz + ctx->total_text_size + ctx->total_rodata_size + ctx->total_data_size;
    
    char shstrtab[128];
    memset(shstrtab, 0, 128);
    int shstr_sz = 1;
    int n_txt = shstr_sz; strcpy(shstrtab + shstr_sz, ".text"); shstr_sz += 6;
    int n_dat = shstr_sz; strcpy(shstrtab + shstr_sz, ".data"); shstr_sz += 6;
    int n_bss = shstr_sz; strcpy(shstrtab + shstr_sz, ".bss"); shstr_sz += 5;
    int n_shstr = shstr_sz; strcpy(shstrtab + shstr_sz, ".shstrtab"); shstr_sz += 10;

    ctx->out_buffer = safe_malloc(file_sz);
    ctx->out_size = file_sz;
    
    Elf32_Ehdr* eh = (Elf32_Ehdr*)ctx->out_buffer;
    eh->e_ident[0]=0x7F; eh->e_ident[1]='E'; eh->e_ident[2]='L'; eh->e_ident[3]='F';
    eh->e_ident[4]=1; eh->e_ident[5]=1; eh->e_ident[6]=1;
    eh->e_type = ET_EXEC; eh->e_machine = EM_386; eh->e_version = 1;
    eh->e_entry = ctx->entry_addr;
    eh->e_phoff = sizeof(Elf32_Ehdr); eh->e_ehsize = sizeof(Elf32_Ehdr);
    eh->e_phentsize = sizeof(Elf32_Phdr); eh->e_phnum = 1;
    eh->e_shentsize = sizeof(Elf32_Shdr); eh->e_shnum = 5;
    eh->e_shstrndx = 4;
    eh->e_shoff = file_sz;
    
    Elf32_Phdr* ph = (Elf32_Phdr*)(ctx->out_buffer + sizeof(Elf32_Ehdr));
    ph->p_type = 1; ph->p_offset = 0;
    ph->p_vaddr = BASE_ADDR; ph->p_paddr = BASE_ADDR;
    ph->p_filesz = file_sz; 
    ph->p_memsz = file_sz + ctx->total_bss_size;
    ph->p_flags = 7;
    ph->p_align = PAGE_ALIGN;
    
    uint8_t* ptr_text = ctx->out_buffer + headers_sz;
    uint8_t* ptr_rodata = ptr_text + ctx->total_text_size;
    uint8_t* ptr_data = ptr_rodata + ctx->total_rodata_size;
    
    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        if (obj->sh_text) memcpy(ptr_text + obj->text_out_offset, obj->raw_data + obj->sh_text->sh_offset, obj->sh_text->sh_size);
        for (int r = 0; r < obj->rodata_count; r++) {
            Elf32_Shdr* sh = obj->sh_rodata[r];
            memcpy(ptr_rodata + obj->rodata_out_offset[r], obj->raw_data + sh->sh_offset, sh->sh_size);
        }
        if (obj->sh_data) memcpy(ptr_data + obj->data_out_offset, obj->raw_data + obj->sh_data->sh_offset, obj->sh_data->sh_size);
    }
    
    for (int i = 0; i < ctx->obj_count; i++) {
        ObjectFile* obj = ctx->objects[i];
        for (int r = 0; r < obj->rel_count; r++) {
            apply_relocations(ctx, obj, obj->sh_rel[r]);
        }
    }
    
    Elf32_Shdr sh[5] = {0};
    sh[1].sh_name = n_txt; sh[1].sh_type = 1; sh[1].sh_flags = 6;
    sh[1].sh_addr = BASE_ADDR + headers_sz; sh[1].sh_offset = headers_sz; sh[1].sh_size = ctx->total_text_size;
    
    sh[2].sh_name = n_dat; sh[2].sh_type = 1; sh[2].sh_flags = 3;
    sh[2].sh_addr = BASE_ADDR + headers_sz + ctx->total_text_size; sh[2].sh_offset = headers_sz + ctx->total_text_size; sh[2].sh_size = ctx->total_rodata_size + ctx->total_data_size;
    
    sh[3].sh_name = n_bss; sh[3].sh_type = 8; sh[3].sh_flags = 3;
    sh[3].sh_addr = sh[2].sh_addr + sh[2].sh_size; sh[3].sh_offset = sh[2].sh_offset + sh[2].sh_size; sh[3].sh_size = ctx->total_bss_size;
    
    sh[4].sh_name = n_shstr; sh[4].sh_type = 3; sh[4].sh_flags = 0;
    sh[4].sh_addr = 0; sh[4].sh_offset = file_sz + sizeof(sh); sh[4].sh_size = shstr_sz;

    int fd = open(outfile, 1);
    if (fd < 0) fatal("Cannot write output: %s", outfile);
    
    write(fd, ctx->out_buffer, ctx->out_size);
    write(fd, sh, sizeof(sh));
    write(fd, shstrtab, shstr_sz);
    
    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: uld -o output.exe input1.o ...\n");
        return 1;
    }

    LinkerCtx* ctx = safe_malloc(sizeof(LinkerCtx));
    
    const char* outfile = "a.out";
    int inputs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) outfile = argv[++i];
        } else {
            if (ctx->obj_count >= MAX_OBJECTS) fatal("Too many input files");
            ctx->objects[ctx->obj_count++] = load_object(argv[i]);
            inputs++;
        }
    }

    if (inputs == 0) fatal("No input files");

    calculate_layout(ctx);
    collect_symbols(ctx);
    if (ctx->entry_addr == 0) printf("Warning: _start symbol not found.\n");

    build_image(ctx, outfile);
    
    set_console_color(0x00FF00, 0x141414);
    printf("Success: Linked %s\n", outfile);
    set_console_color(0xD4D4D4, 0x141414);

    free(ctx);
    return 0;
}