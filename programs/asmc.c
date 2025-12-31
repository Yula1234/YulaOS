// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define MAX_LINE_LEN    256
#define MAX_TOKEN_LEN   64
#define MAX_SYMBOLS     2048

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT 16

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

#define ET_REL 1
#define EM_386 3
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define R_386_32 1
#define R_386_PC32 2
#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xf))
#define ELF32_R_INFO(s,t)  (((s)<<8)+(unsigned char)(t))

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
} Buffer;

typedef enum { SEC_NULL=0, SEC_TEXT, SEC_DATA, SEC_BSS, SEC_ABS } SectionID;
typedef enum { SYM_UNDEF, SYM_LOCAL, SYM_GLOBAL, SYM_EXTERN } SymBind;

typedef struct {
    char name[64];
    SymBind bind;
    SectionID section;
    uint32_t value;
    uint32_t elf_idx;
} Symbol;

typedef struct {
    int pass;
    int line_num;
    SectionID cur_sec;
    
    Buffer text;
    Buffer data;
    Buffer bss;
    Buffer rel_text;
    Buffer rel_data;
    
    Symbol symbols[MAX_SYMBOLS];
    int sym_count;
} AssemblerCtx;

void panic(AssemblerCtx* ctx, const char* msg) {
    set_console_color(0xF44747, 0x141414);
    printf("\n[ASMC ERROR] Line %d: %s\n", ctx->line_num, msg);
    set_console_color(0xD4D4D4, 0x141414);
    exit(1);
}

void buf_init(Buffer* b, uint32_t cap) {
    if (cap == 0) cap = 64;
    b->data = malloc(cap);
    if (!b->data) exit(1);
    b->size = 0;
    b->capacity = cap;
}

void buf_free(Buffer* b) {
    if (b->data) free(b->data);
    b->data = 0; b->size = 0;
}

void buf_push(Buffer* b, uint8_t byte) {
    if (b->size >= b->capacity) {
        b->capacity *= 2;
        uint8_t* new_data = malloc(b->capacity);
        if (!new_data) exit(1);
        memcpy(new_data, b->data, b->size);
        free(b->data);
        b->data = new_data;
    }
    b->data[b->size++] = byte;
}

void buf_push_u32(Buffer* b, uint32_t val) {
    buf_push(b, val & 0xFF);
    buf_push(b, (val >> 8) & 0xFF);
    buf_push(b, (val >> 16) & 0xFF);
    buf_push(b, (val >> 24) & 0xFF);
}

void buf_write(Buffer* b, void* src, uint32_t len) {
    uint8_t* p = (uint8_t*)src;
    for(uint32_t i=0; i<len; i++) buf_push(b, p[i]);
}

uint32_t buf_add_string(Buffer* b, const char* str) {
    uint32_t offset = b->size;
    while (*str) buf_push(b, *str++);
    buf_push(b, 0);
    return offset;
}

Symbol* sym_find(AssemblerCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->sym_count; i++) {
        if (strcmp(ctx->symbols[i].name, name) == 0) return &ctx->symbols[i];
    }
    return 0;
}

int eval_number(AssemblerCtx* ctx, const char* s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    
    if (s[0] == '0' && s[1] == 'x') {
        s += 2;
        uint32_t val = 0;
        while (*s) {
            val <<= 4;
            if (*s >= '0' && *s <= '9') val |= (*s - '0');
            else if (*s >= 'a' && *s <= 'f') val |= (*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') val |= (*s - 'A' + 10);
            else break;
            s++;
        }
        return (int)val * sign;
    }
    
    if (*s >= '0' && *s <= '9') {
        int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        return val * sign;
    }

    Symbol* sym = sym_find(ctx, s);
    if (sym && sym->section == SEC_ABS) {
        return sym->value * sign;
    }

    return 0;
}

Symbol* sym_add(AssemblerCtx* ctx, const char* name) {
    Symbol* s = sym_find(ctx, name);
    if (s) return s;
    if (ctx->sym_count >= MAX_SYMBOLS) panic(ctx, "Symbol table overflow");
    s = &ctx->symbols[ctx->sym_count++];
    strcpy(s->name, name);
    s->bind = SYM_UNDEF;
    s->section = SEC_NULL;
    s->value = 0;
    return s;
}

void sym_define_label(AssemblerCtx* ctx, const char* name) {
    Symbol* s = sym_find(ctx, name);
    
    if (ctx->pass == 1) {
        if (!s) s = sym_add(ctx, name);
        if (s->bind == SYM_UNDEF) s->bind = SYM_LOCAL;
        s->section = ctx->cur_sec;
    }
    
    if (s) {
        if (ctx->cur_sec == SEC_TEXT) s->value = ctx->text.size;
        else if (ctx->cur_sec == SEC_DATA) s->value = ctx->data.size;
        else if (ctx->cur_sec == SEC_BSS) s->value = ctx->bss.size;
    }
}

typedef enum { OP_NONE, OP_REG, OP_MEM, OP_IMM } OpType;

typedef struct {
    OpType type;
    int reg;
    int size; // 1, 2, 4
    int32_t disp;
    char label[64];
    int has_label;
} Operand;

const char* reg_names32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
const char* reg_names16[] = { "ax",  "cx",  "dx",  "bx",  "sp",  "bp",  "si",  "di"  };
const char* reg_names8[]  = { "al",  "cl",  "dl",  "bl",  "ah",  "ch",  "dh",  "bh"  };

int get_reg_info(const char* s, int* size) {
    for(int i=0; i<8; i++) {
        if(strcmp(s, reg_names32[i])==0) { *size = 4; return i; }
        if(strcmp(s, reg_names16[i])==0) { *size = 2; return i; }
        if(strcmp(s, reg_names8[i])==0)  { *size = 1; return i; }
    }
    return -1;
}

typedef enum {
    ENC_NONE, ENC_R, ENC_I, ENC_M, ENC_MR, ENC_RM, ENC_MI, ENC_OI, ENC_J, ENC_SHIFT,
    ENC_0F, ENC_0F_MR
} EncMode;

typedef struct {
    const char* mnem;
    uint8_t op_base;
    uint8_t op_ext;
    EncMode mode;
    int size; // 0=any, 1=byte, 2=word, 4=dword
} InstrDef;

InstrDef isa[] = {
    { "ret",   0xC3, 0, ENC_NONE, 0 }, { "nop",   0x90, 0, ENC_NONE, 0 },
    { "hlt",   0xF4, 0, ENC_NONE, 0 }, { "cli",   0xFA, 0, ENC_NONE, 0 },
    { "sti",   0xFB, 0, ENC_NONE, 0 }, { "pusha", 0x60, 0, ENC_NONE, 0 },
    { "popa",  0x61, 0, ENC_NONE, 0 }, { "leave", 0xC9, 0, ENC_NONE, 0 },
    { "cld",   0xFC, 0, ENC_NONE, 0 }, { "std",   0xFD, 0, ENC_NONE, 0 },
    { "int3",  0xCC, 0, ENC_NONE, 0 },
    
    { "ud2",   0x0B, 0, ENC_0F, 0 },   { "rdtsc", 0x31, 0, ENC_0F, 0 },

    { "push",  0x50, 0, ENC_R, 4 },    { "pop",   0x58, 0, ENC_R, 4 },
    { "push",  0x68, 0, ENC_I, 4 },    { "int",   0xCD, 0, ENC_I, 0 }, 
    { "push",  0x6A, 0, ENC_I, 1 },    

    { "inc",   0x40, 0, ENC_R, 4 },    { "dec",   0x48, 0, ENC_R, 4 },
    { "inc",   0xFE, 0, ENC_M, 1 },    { "dec",   0xFE, 1, ENC_M, 1 },
    { "inc",   0xFF, 0, ENC_M, 4 },    { "dec",   0xFF, 1, ENC_M, 4 },

    { "mul",   0xF6, 4, ENC_M, 1 },    { "imul",  0xF6, 5, ENC_M, 1 },
    { "div",   0xF6, 6, ENC_M, 1 },    { "idiv",  0xF6, 7, ENC_M, 1 },
    { "neg",   0xF6, 3, ENC_M, 1 },    { "not",   0xF6, 2, ENC_M, 1 },
    
    { "mul",   0xF7, 4, ENC_M, 4 },    { "imul",  0xF7, 5, ENC_M, 4 },
    { "div",   0xF7, 6, ENC_M, 4 },    { "idiv",  0xF7, 7, ENC_M, 4 },
    { "neg",   0xF7, 3, ENC_M, 4 },    { "not",   0xF7, 2, ENC_M, 4 },

    { "call",  0xE8, 0, ENC_J, 0 },    { "jmp",   0xE9, 0, ENC_J, 0 },
    { "call",  0xFF, 2, ENC_M, 4 }, 
    { "je",    0x84, 0, ENC_J, 0 },    { "jz",    0x84, 0, ENC_J, 0 },
    { "jne",   0x85, 0, ENC_J, 0 },    { "jnz",   0x85, 0, ENC_J, 0 },
    { "jg",    0x8F, 0, ENC_J, 0 },    { "jge",   0x8D, 0, ENC_J, 0 },
    { "jl",    0x8C, 0, ENC_J, 0 },    { "jle",   0x8E, 0, ENC_J, 0 },
    { "ja",    0x87, 0, ENC_J, 0 },    { "jae",   0x83, 0, ENC_J, 0 },
    { "jb",    0x82, 0, ENC_J, 0 },    { "jbe",   0x86, 0, ENC_J, 0 },
    { "loop",  0xE2, 0, ENC_J, 0 }, 

    { "mov",   0x88, 0, ENC_MR, 1 },   { "mov",   0x8A, 0, ENC_RM, 1 },
    { "mov",   0xB0, 0, ENC_OI, 1 },   { "mov",   0xC6, 0, ENC_MI, 1 },
    
    { "mov",   0x89, 0, ENC_MR, 4 },   { "mov",   0x8B, 0, ENC_RM, 4 },
    { "mov",   0xB8, 0, ENC_OI, 4 },   { "mov",   0xC7, 0, ENC_MI, 4 },
    { "lea",   0x8D, 0, ENC_RM, 4 },   

    { "xchg",  0x86, 0, ENC_MR, 1 },   { "xchg",  0x87, 0, ENC_MR, 4 },
    { "xchg",  0x90, 0, ENC_R,  4 }, 

    { "movzx", 0xB6, 0, ENC_0F_MR, 1 }, { "movzx", 0xB7, 0, ENC_0F_MR, 4 }, 
    { "movsx", 0xBE, 0, ENC_0F_MR, 1 }, { "movsx", 0xBF, 0, ENC_0F_MR, 4 },

    { "movb",  0xC6, 0, ENC_MI, 1 },   { "movb",  0x88, 0, ENC_MR, 1 },   { "movb",  0x8A, 0, ENC_RM, 1 },   

    { "add",   0x00, 0, ENC_MR, 1 }, { "add",   0x02, 0, ENC_RM, 1 }, { "add",   0x80, 0, ENC_MI, 1 },
    { "or",    0x08, 0, ENC_MR, 1 }, { "or",    0x0A, 0, ENC_RM, 1 }, { "or",    0x80, 1, ENC_MI, 1 },
    { "adc",   0x10, 0, ENC_MR, 1 }, { "adc",   0x12, 0, ENC_RM, 1 }, { "adc",   0x80, 2, ENC_MI, 1 },
    { "sbb",   0x18, 0, ENC_MR, 1 }, { "sbb",   0x1A, 0, ENC_RM, 1 }, { "sbb",   0x80, 3, ENC_MI, 1 },
    { "and",   0x20, 0, ENC_MR, 1 }, { "and",   0x22, 0, ENC_RM, 1 }, { "and",   0x80, 4, ENC_MI, 1 },
    { "sub",   0x28, 0, ENC_MR, 1 }, { "sub",   0x2A, 0, ENC_RM, 1 }, { "sub",   0x80, 5, ENC_MI, 1 },
    { "xor",   0x30, 0, ENC_MR, 1 }, { "xor",   0x32, 0, ENC_RM, 1 }, { "xor",   0x80, 6, ENC_MI, 1 },
    { "cmp",   0x38, 0, ENC_MR, 1 }, { "cmp",   0x3A, 0, ENC_RM, 1 }, { "cmp",   0x80, 7, ENC_MI, 1 },
    { "test",  0x84, 0, ENC_MR, 1 }, { "test",  0xF6, 0, ENC_MI, 1 }, 

    { "add",   0x01, 0, ENC_MR, 4 }, { "add",   0x03, 0, ENC_RM, 4 }, { "add",   0x81, 0, ENC_MI, 4 }, { "add", 0x83, 0, ENC_MI, 4 },
    { "or",    0x09, 0, ENC_MR, 4 }, { "or",    0x0B, 0, ENC_RM, 4 }, { "or",    0x81, 1, ENC_MI, 4 }, { "or",  0x83, 1, ENC_MI, 4 },
    { "adc",   0x11, 0, ENC_MR, 4 }, { "adc",   0x13, 0, ENC_RM, 4 }, { "adc",   0x81, 2, ENC_MI, 4 }, { "adc", 0x83, 2, ENC_MI, 4 },
    { "sbb",   0x19, 0, ENC_MR, 4 }, { "sbb",   0x1B, 0, ENC_RM, 4 }, { "sbb",   0x81, 3, ENC_MI, 4 }, { "sbb", 0x83, 3, ENC_MI, 4 },
    { "and",   0x21, 0, ENC_MR, 4 }, { "and",   0x23, 0, ENC_RM, 4 }, { "and",   0x81, 4, ENC_MI, 4 }, { "and", 0x83, 4, ENC_MI, 4 },
    { "sub",   0x29, 0, ENC_MR, 4 }, { "sub",   0x2B, 0, ENC_RM, 4 }, { "sub",   0x81, 5, ENC_MI, 4 }, { "sub", 0x83, 5, ENC_MI, 4 },
    { "xor",   0x31, 0, ENC_MR, 4 }, { "xor",   0x33, 0, ENC_RM, 4 }, { "xor",   0x81, 6, ENC_MI, 4 }, { "xor", 0x83, 6, ENC_MI, 4 },
    { "cmp",   0x39, 0, ENC_MR, 4 }, { "cmp",   0x3B, 0, ENC_RM, 4 }, { "cmp",   0x81, 7, ENC_MI, 4 }, { "cmp", 0x83, 7, ENC_MI, 4 },
    { "test",  0x85, 0, ENC_MR, 4 }, { "test",  0xF7, 0, ENC_MI, 4 }, 

    { "shl",   0xC1, 4, ENC_SHIFT, 4 }, { "shr",   0xC1, 5, ENC_SHIFT, 4 },
    { "sal",   0xC1, 4, ENC_SHIFT, 4 }, { "sar",   0xC1, 7, ENC_SHIFT, 4 },
    { "rol",   0xC1, 0, ENC_SHIFT, 4 }, { "ror",   0xC1, 1, ENC_SHIFT, 4 },
    { "shl",   0xD1, 4, ENC_SHIFT, 4 }, { "shr",   0xD1, 5, ENC_SHIFT, 4 },
    { "sal",   0xD1, 4, ENC_SHIFT, 4 }, { "sar",   0xD1, 7, ENC_SHIFT, 4 },

    { 0,0,0,0,0 }
};

void parse_operand(AssemblerCtx* ctx, char* text, Operand* op) {
    memset(op, 0, sizeof(Operand));
    op->reg = -1;
    op->size = 0; 
    if (!text || !*text) { op->type = OP_NONE; return; }

    if (text[0] == '[') {
        op->type = OP_MEM;
        int len = strlen(text);
        if (text[len-1] != ']') panic(ctx, "Missing ']'");
        text[len-1] = 0;
        char* content = text + 1;
        
        char* plus = 0; char* ptr = content;
        while(*ptr) { if(*ptr=='+' || *ptr=='-') plus=ptr; ptr++; }

        char* base_part = content;
        char* disp_part = 0;
        int sign = 1;

        if (plus) {
            if (*plus == '-') sign = -1;
            *plus = 0;
            disp_part = plus + 1;
        }

        while (*base_part == ' ') base_part++;
        int sz;
        int r = get_reg_info(base_part, &sz);
        
        if (r != -1) {
            if (sz != 4) panic(ctx, "Memory base register must be 32-bit");
            op->reg = r;
        } else {
            if ((base_part[0] >= '0' && base_part[0] <= '9') || base_part[0] == '-') 
                op->disp = eval_number(ctx, base_part);
            else { strcpy(op->label, base_part); op->has_label = 1; }
        }

        if (disp_part) {
            while (*disp_part == ' ') disp_part++;
            op->disp += (sign * eval_number(ctx, disp_part));
        }
        return;
    }

    int sz;
    int r = get_reg_info(text, &sz);
    if (r != -1) { 
        op->type = OP_REG; 
        op->reg = r; 
        op->size = sz; 
        return; 
    }

    op->type = OP_IMM;
    
    if (text[0] == '\'' && text[2] == '\'' && text[1] != 0) {
        op->disp = (int)text[1];
        op->size = 1; 
        return;
    }

    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-') {
        op->disp = eval_number(ctx, text);
        if (op->disp >= -128 && op->disp <= 255) op->size = 1; 
        else op->size = 4;
    } else {
        Symbol* s = sym_find(ctx, text);
        if (s && s->section == SEC_ABS) {
            op->disp = s->value;
            if (op->disp >= -128 && op->disp <= 255) op->size = 1;
            else op->size = 4;
        } else {
            strcpy(op->label, text);
            op->has_label = 1;
            op->size = 4; 
        }
    }
}

Buffer* get_cur_buffer(AssemblerCtx* ctx) {
    if (ctx->cur_sec == SEC_DATA) return &ctx->data;
    if (ctx->cur_sec == SEC_BSS) return &ctx->bss;
    return &ctx->text;
}

void emit_byte(AssemblerCtx* ctx, uint8_t b) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size++; return; }
    buf_push(buf, b);
}

void emit_word(AssemblerCtx* ctx, uint16_t w) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size += 2; return; }
    buf_push(buf, w & 0xFF);
    buf_push(buf, (w >> 8) & 0xFF);
}

void emit_dword(AssemblerCtx* ctx, uint32_t d) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size += 4; return; }
    buf_push_u32(buf, d);
}

void emit_reloc(AssemblerCtx* ctx, int type, char* label, uint32_t offset) {
    if (ctx->pass != 2) return;
    Symbol* s = sym_find(ctx, label);
    if (!s) panic(ctx, "Undefined symbol");
    
    Elf32_Rel r;
    r.r_offset = offset;
    r.r_info = ELF32_R_INFO(s->elf_idx, type);
    
    Buffer* target = (ctx->cur_sec == SEC_TEXT) ? &ctx->rel_text : &ctx->rel_data;
    buf_write(target, &r, sizeof(r));
}

void emit_modrm(AssemblerCtx* ctx, int reg_opcode, Operand* rm) {
    if (ctx->pass != 2) return;
    
    Buffer* buf = get_cur_buffer(ctx);
    
    if (rm->type == OP_REG) {
        emit_byte(ctx, 0xC0 | ((reg_opcode & 7) << 3) | (rm->reg & 7));
    } else {
        if (rm->reg == -1) {
            emit_byte(ctx, (0 << 6) | ((reg_opcode & 7) << 3) | 5);
            uint32_t val = rm->disp;
            if (rm->has_label) { emit_reloc(ctx, R_386_32, rm->label, buf->size); val = 0; }
            emit_dword(ctx, val);
        } else {
            if (rm->disp == 0 && rm->reg != 5) {
                emit_byte(ctx, (0 << 6) | ((reg_opcode & 7) << 3) | (rm->reg & 7));
                if (rm->reg == 4) emit_byte(ctx, 0x24);
            } else {
                if (rm->disp >= -128 && rm->disp <= 127) {
                    emit_byte(ctx, (1 << 6) | ((reg_opcode & 7) << 3) | (rm->reg & 7));
                    if (rm->reg == 4) emit_byte(ctx, 0x24);
                    emit_byte(ctx, (uint8_t)rm->disp);
                } else {
                    emit_byte(ctx, (2 << 6) | ((reg_opcode & 7) << 3) | (rm->reg & 7));
                    if (rm->reg == 4) emit_byte(ctx, 0x24);
                    emit_dword(ctx, rm->disp);
                }
            }
        }
    }
}

void assemble_instr(AssemblerCtx* ctx, char* name, int explicit_size, Operand* o1, Operand* o2) {
    int size = explicit_size;
    if (size == 0) {
        if (o1->type == OP_REG) size = o1->size;
        else if (o2->type == OP_REG) size = o2->size;
    }
    if (size == 0) size = 4;

    if (size == 2) emit_byte(ctx, 0x66);

    for (int i = 0; isa[i].mnem; i++) {
        InstrDef* d = &isa[i];
        if (strcmp(d->mnem, name) != 0) continue;
        
        int match_size = d->size;
        if (match_size == 4 && size == 2) match_size = 2; 

        if (d->size != 0 && match_size != size) continue;

        if (d->mode == ENC_NONE) {
            if (o1->type != OP_NONE) continue;
            emit_byte(ctx, d->op_base);
            return;
        }
        if (d->mode == ENC_0F) {
            if (o1->type != OP_NONE) continue;
            emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);
            return;
        }
        if (d->mode == ENC_0F_MR) {
            if (o2->type != OP_REG || o1->type == OP_IMM) continue;
            emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base); 
            emit_modrm(ctx, o2->reg, o1);
            return;
        }
        if (d->mode == ENC_R) {
            if (o1->type != OP_REG) continue;
            emit_byte(ctx, d->op_base + o1->reg);
            return;
        }
        if (d->mode == ENC_I) {
            if (o1->type != OP_IMM) continue;
            if (d->op_base == 0xCD) { emit_byte(ctx, d->op_base); emit_byte(ctx, (uint8_t)o1->disp); }
            else {
                emit_byte(ctx, d->op_base);
                uint32_t val = o1->disp;
                Buffer* buf = get_cur_buffer(ctx);
                if (o1->has_label) { emit_reloc(ctx, R_386_32, o1->label, buf->size); val = 0; }
                
                if (size == 2) emit_word(ctx, (uint16_t)val);
                else emit_dword(ctx, val);
            }
            return;
        }
        if (d->mode == ENC_J) {
            if (o1->type != OP_IMM) continue;
            Buffer* buf = get_cur_buffer(ctx);
            
            if (d->op_base == 0xE2) { 
                 emit_byte(ctx, d->op_base);
                 int32_t delta = -2;
                 if (ctx->pass == 2 && o1->has_label) {
                     Symbol* s = sym_find(ctx, o1->label);
                     if (s && s->section == ctx->cur_sec) delta = s->value - (buf->size + 1);
                 }
                 emit_byte(ctx, (int8_t)delta);
                 return;
            }

            if (d->op_base >= 0x80 && d->op_base <= 0x8F) emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);
            
            uint32_t val = 0;
            if (ctx->pass == 2) {
                if (o1->has_label) {
                    emit_reloc(ctx, R_386_PC32, o1->label, buf->size);
                    val = -4; 
                } else {
                    val = o1->disp;
                }
            }
            emit_dword(ctx, val);
            return;
        }
        if (d->mode == ENC_OI) {
            if (o1->type != OP_REG || o2->type != OP_IMM) continue;
            emit_byte(ctx, d->op_base + o1->reg);
            uint32_t val = o2->disp;
            Buffer* buf = get_cur_buffer(ctx);
            if (o2->has_label && ctx->pass == 2) { emit_reloc(ctx, R_386_32, o2->label, buf->size); val = 0; }
            
            if (size == 1) emit_byte(ctx, (uint8_t)val);
            else if (size == 2) emit_word(ctx, (uint16_t)val);
            else emit_dword(ctx, val);
            return;
        }
        if (d->mode == ENC_MR) {
            if (o2->type != OP_REG || o1->type == OP_IMM) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, o2->reg, o1);
            return;
        }
        if (d->mode == ENC_RM) {
            if (o1->type != OP_REG || o2->type == OP_IMM) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, o1->reg, o2);
            return;
        }
        if (d->mode == ENC_MI) {
            if (o2->type != OP_IMM || o1->type == OP_IMM) continue;
            if (d->op_base == 0x83) {
                if (o2->disp < -128 || o2->disp > 127) continue;
            }
            
            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);
            
            if (size == 1 || d->op_base == 0x83) {
                emit_byte(ctx, (uint8_t)o2->disp);
            } else {
                if (size == 2) emit_word(ctx, (uint16_t)o2->disp);
                else emit_dword(ctx, o2->disp);
            }
            return;
        }
        if (d->mode == ENC_M) {
            if (o1->type == OP_IMM || o2->type != OP_NONE) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);
            return;
        }
        if (d->mode == ENC_SHIFT) {
            if (o1->type == OP_IMM || o2->type != OP_IMM) continue;
            if (d->op_base == 0xD1 || d->op_base == 0xD0) {
                if (o2->disp != 1) continue; 
                emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);
                return;
            }
            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1); emit_byte(ctx, (uint8_t)o2->disp);
            return;
        }
    }
    panic(ctx, "Unknown instruction");
}

void process_line(AssemblerCtx* ctx, char* line) {
    char* tokens[16]; int count = 0;
    char* p = line;
    while(*p) {
        while(*p == ' ' || *p == '\t' || *p == ',' || *p == '\r') *p++ = 0;
        if(*p == 0 || *p == ';') break;
        char* start = p;
        if (*p == '"') { p++; while(*p && *p != '"') p++; if(*p == '"') p++; }
        else if (*p == '\'') { p++; while(*p && *p != '\'') p++; if(*p == '\'') p++; }
        else if (*p == '[') { while(*p && *p != ']') p++; if(*p == ']') p++; }
        else { while(*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' && *p != '\r') p++; }
        tokens[count++] = start;
        if(count >= 16) break;
    }
    if(count == 0) return;

    int len = strlen(tokens[0]);
    if(tokens[0][len-1] == ':') {
        tokens[0][len-1] = 0;
        sym_define_label(ctx, tokens[0]);
        for(int k=0; k<count-1; k++) tokens[k] = tokens[k+1];
        count--;
        if(count == 0) return;
    }

    char* cmd_name = tokens[0];
    int force_size = 0;
    char* clean_tokens[3];
    int c_idx = 0;
    
    if (count >= 3 && strcmp(tokens[1], "equ") == 0) {
        if (ctx->pass == 1) {
            Symbol* s = sym_add(ctx, tokens[0]);
            s->value = eval_number(ctx, tokens[2]);
            s->section = SEC_ABS;
            s->bind = SYM_LOCAL;
        }
        return;
    }

    for (int i=1; i<count && c_idx < 2; i++) {
        if (strcmp(tokens[i], "byte") == 0) { force_size = 1; continue; }
        if (strcmp(tokens[i], "word") == 0) { force_size = 2; continue; }
        if (strcmp(tokens[i], "dword") == 0) { force_size = 4; continue; }
        if (strcmp(tokens[i], "ptr") == 0) { continue; }
        clean_tokens[c_idx++] = tokens[i];
    }
    
    if (strcmp(cmd_name, "movb") == 0) { cmd_name = "mov"; force_size = 1; }

    if(strcmp(cmd_name, "section") == 0) {
        if(strcmp(tokens[1], ".text") == 0) ctx->cur_sec = SEC_TEXT;
        else if(strcmp(tokens[1], ".data") == 0) ctx->cur_sec = SEC_DATA;
        else if(strcmp(tokens[1], ".bss") == 0) ctx->cur_sec = SEC_BSS;
        return;
    }
    if(strcmp(cmd_name, "global") == 0) {
        if(ctx->pass == 1) { Symbol* s = sym_add(ctx, tokens[1]); s->bind = SYM_GLOBAL; }
        return;
    }
    if(strcmp(cmd_name, "extern") == 0) {
        if(ctx->pass == 1) { Symbol* s = sym_add(ctx, tokens[1]); s->bind = SYM_EXTERN; }
        return;
    }
    if(strcmp(cmd_name, "db") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        for(int k=1; k<count; k++) {
            if(tokens[k][0] == '"') {
                char* s = tokens[k]+1;
                while(*s && *s != '"') {
                    if(ctx->pass == 2) buf_push(b, *s); else b->size++;
                    s++;
                }
            } else {
                if(ctx->pass == 2) buf_push(b, eval_number(ctx, tokens[k])); else b->size++;
            }
        }
        return;
    }
    if(strcmp(cmd_name, "dw") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        if(ctx->pass == 2) {
            int val = eval_number(ctx, tokens[1]);
            buf_push(b, val & 0xFF);
            buf_push(b, (val >> 8) & 0xFF);
        } else b->size += 2;
        return;
    }
    if(strcmp(cmd_name, "dd") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        if(ctx->pass == 2) {
            if ((tokens[1][0] >= '0' && tokens[1][0] <= '9') || tokens[1][0] == '-') 
                buf_push_u32(b, eval_number(ctx, tokens[1]));
            else {
                Symbol* s = sym_find(ctx, tokens[1]);
                if (s && s->section == SEC_ABS) {
                    buf_push_u32(b, s->value);
                } else if (s) { 
                    emit_reloc(ctx, R_386_32, tokens[1], b->size); buf_push_u32(b, 0); 
                } else {
                    buf_push_u32(b, eval_number(ctx, tokens[1]));
                }
            }
        } else b->size += 4;
        return;
    }
    if(strcmp(cmd_name, "resb") == 0 || strcmp(cmd_name, "rb") == 0) {
        if(ctx->cur_sec != SEC_BSS) panic(ctx, "resb only in .bss");
        ctx->bss.size += eval_number(ctx, tokens[1]);
        return;
    }

    Operand o1 = {0}, o2 = {0};
    if(c_idx > 0) parse_operand(ctx, clean_tokens[0], &o1);
    if(c_idx > 1) parse_operand(ctx, clean_tokens[1], &o2);
    assemble_instr(ctx, cmd_name, force_size, &o1, &o2);
}

void write_elf(AssemblerCtx* ctx, const char* filename) {
    Buffer strtab; buf_init(&strtab, 512); buf_push(&strtab, 0);
    Buffer symtab; buf_init(&symtab, 1024);
    Elf32_Sym null_sym = {0}; buf_write(&symtab, &null_sym, sizeof(null_sym));

    for(int i=0; i<ctx->sym_count; i++) {
        Symbol* s = &ctx->symbols[i];
        if (s->section == SEC_ABS) continue; 

        Elf32_Sym es;
        es.st_name = buf_add_string(&strtab, s->name);
        es.st_value = s->value;
        es.st_size = 0;
        es.st_other = 0;
        int bind = (s->bind == SYM_GLOBAL || s->bind == SYM_EXTERN) ? STB_GLOBAL : STB_LOCAL;
        int type = (s->section == SEC_TEXT) ? STT_FUNC : (s->section != SEC_NULL ? STT_OBJECT : STT_NOTYPE);
        es.st_info = ELF32_ST_INFO(bind, type);
        if (s->bind == SYM_EXTERN) es.st_shndx = SHN_UNDEF;
        else if (s->section == SEC_TEXT) es.st_shndx = 1;
        else if (s->section == SEC_DATA) es.st_shndx = 2;
        else if (s->section == SEC_BSS) es.st_shndx = 3;
        else es.st_shndx = SHN_UNDEF;
        buf_write(&symtab, &es, sizeof(es));
    }

    Buffer shstr; buf_init(&shstr, 256); buf_push(&shstr, 0);
    int n_txt = buf_add_string(&shstr, ".text");
    int n_dat = buf_add_string(&shstr, ".data");
    int n_bss = buf_add_string(&shstr, ".bss");
    int n_sym = buf_add_string(&shstr, ".symtab");
    int n_str = buf_add_string(&shstr, ".strtab");
    int n_shs = buf_add_string(&shstr, ".shstrtab");
    int n_rt  = buf_add_string(&shstr, ".rel.text");
    int n_rd  = buf_add_string(&shstr, ".rel.data");

    uint32_t offset = sizeof(Elf32_Ehdr);
    uint32_t off_txt = offset; offset += ctx->text.size;
    uint32_t off_dat = offset; offset += ctx->data.size;
    uint32_t off_bss = offset;
    uint32_t off_sym = offset; offset += symtab.size;
    uint32_t off_str = offset; offset += strtab.size;
    uint32_t off_shs = offset; offset += shstr.size;
    uint32_t off_rt  = offset; offset += ctx->rel_text.size;
    uint32_t off_rd  = offset; offset += ctx->rel_data.size;
    uint32_t off_shdr = offset;

    Elf32_Ehdr eh = {0};
    eh.e_ident[0]=0x7F; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
    eh.e_ident[4]=1; eh.e_ident[5]=1; eh.e_ident[6]=1;
    eh.e_type = ET_REL; eh.e_machine = EM_386; eh.e_version = 1;
    eh.e_shoff = off_shdr; eh.e_ehsize = sizeof(eh);
    eh.e_shentsize = sizeof(Elf32_Shdr); eh.e_shnum = 9; eh.e_shstrndx = 6;

    int fd = open(filename, 1);
    if(fd < 0) { printf("Error creating file\n"); return; }

    write(fd, &eh, sizeof(eh));
    if(ctx->text.size) write(fd, ctx->text.data, ctx->text.size);
    if(ctx->data.size) write(fd, ctx->data.data, ctx->data.size);
    write(fd, symtab.data, symtab.size);
    write(fd, strtab.data, strtab.size);
    write(fd, shstr.data, shstr.size);
    if(ctx->rel_text.size) write(fd, ctx->rel_text.data, ctx->rel_text.size);
    if(ctx->rel_data.size) write(fd, ctx->rel_data.data, ctx->rel_data.size);

    Elf32_Shdr sh[9] = {0};
    sh[1].sh_name=n_txt; sh[1].sh_type=SHT_PROGBITS; sh[1].sh_flags=SHF_ALLOC|SHF_EXECINSTR; sh[1].sh_offset=off_txt; sh[1].sh_size=ctx->text.size; sh[1].sh_addralign=4;
    sh[2].sh_name=n_dat; sh[2].sh_type=SHT_PROGBITS; sh[2].sh_flags=SHF_ALLOC|SHF_WRITE; sh[2].sh_offset=off_dat; sh[2].sh_size=ctx->data.size; sh[2].sh_addralign=4;
    sh[3].sh_name=n_bss; sh[3].sh_type=SHT_NOBITS; sh[3].sh_flags=SHF_ALLOC|SHF_WRITE; sh[3].sh_offset=off_bss; sh[3].sh_size=ctx->bss.size; sh[3].sh_addralign=4;
    sh[4].sh_name=n_sym; sh[4].sh_type=SHT_SYMTAB; sh[4].sh_offset=off_sym; sh[4].sh_size=symtab.size; sh[4].sh_link=5; sh[4].sh_entsize=sizeof(Elf32_Sym); sh[4].sh_addralign=4; sh[4].sh_info=ctx->sym_count;
    sh[5].sh_name=n_str; sh[5].sh_type=SHT_STRTAB; sh[5].sh_offset=off_str; sh[5].sh_size=strtab.size; sh[5].sh_addralign=1;
    sh[6].sh_name=n_shs; sh[6].sh_type=SHT_STRTAB; sh[6].sh_offset=off_shs; sh[6].sh_size=shstr.size; sh[6].sh_addralign=1;
    sh[7].sh_name=n_rt; sh[7].sh_type=SHT_REL; sh[7].sh_offset=off_rt; sh[7].sh_size=ctx->rel_text.size; sh[7].sh_link=4; sh[7].sh_info=1; sh[7].sh_entsize=sizeof(Elf32_Rel); sh[7].sh_addralign=4;
    sh[8].sh_name=n_rd; sh[8].sh_type=SHT_REL; sh[8].sh_offset=off_rd; sh[8].sh_size=ctx->rel_data.size; sh[8].sh_link=4; sh[8].sh_info=2; sh[8].sh_entsize=sizeof(Elf32_Rel); sh[8].sh_addralign=4;

    write(fd, sh, sizeof(sh));
    close(fd);

    buf_free(&strtab); buf_free(&symtab); buf_free(&shstr);
}

int main(int argc, char** argv) {
    if(argc < 3) { printf("ASMC v2.2.1\nUsage: asmc in.asm out.o\n"); return 1; }

    int fd = open(argv[1], 0);
    if(fd < 0) { printf("Cannot open input file\n"); return 1; }
    
    char* src = malloc(65536);
    if (!src) { printf("Memory error\n"); return 1; }
    int len = read(fd, src, 65535);
    src[len] = 0; close(fd);

    AssemblerCtx* ctx = malloc(sizeof(AssemblerCtx));
    if (!ctx) { printf("Out of memory for context\n"); free(src); return 1; }
    memset(ctx, 0, sizeof(AssemblerCtx));

    buf_init(&ctx->text, 4096); buf_init(&ctx->data, 4096);
    buf_init(&ctx->bss, 0);     buf_init(&ctx->rel_text, 1024);
    buf_init(&ctx->rel_data, 1024);

    ctx->pass = 1; ctx->line_num = 0;
    char line[256]; int i=0;
    while(src[i]) {
        int j=0; 
        while(src[i] && src[i]!='\n') {
            if (src[i] != '\r') line[j++] = src[i];
            i++;
        }
        line[j]=0; 
        if(src[i]=='\n') i++;
        ctx->line_num++;
        process_line(ctx, line);
    }

    int elf_idx = 1;
    for(int k=0; k<ctx->sym_count; k++) {
        if (ctx->symbols[k].section != SEC_ABS) {
            ctx->symbols[k].elf_idx = elf_idx++;
        } else {
            ctx->symbols[k].elf_idx = 0;
        }
    }

    ctx->pass = 2; ctx->line_num = 0; i=0;
    ctx->text.size = 0; ctx->data.size = 0; ctx->bss.size = 0;
    
    while(src[i]) {
        int j=0; 
        while(src[i] && src[i]!='\n') {
            if (src[i] != '\r') line[j++] = src[i];
            i++;
        }
        line[j]=0; 
        if(src[i]=='\n') i++;
        ctx->line_num++;
        process_line(ctx, line);
    }

    write_elf(ctx, argv[2]);
    printf("Success: %s (%d bytes code, %d bytes data)\n", argv[2], ctx->text.size, ctx->data.size);
    
    free(ctx);
    free(src);
    return 0;
}