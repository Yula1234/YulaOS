// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define MAX_FILE_SIZE   65536
#define MAX_INSTR_LEN   15

#define C_BG_DEFAULT    0x141414
#define C_ADDR          0x606060
#define C_BYTES         0x808080
#define C_MNEM          0x569CD6
#define C_REG           0xD4D4D4
#define C_NUM           0xB5CEA8
#define C_LABEL         0xC586C0
#define C_JUMP          0xDCDCAA
#define C_DATA_HDR      0x4EC9B0
#define C_DATA_TXT      0xCE9178
#define C_DEFAULT       0xD4D4D4

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) ElfHeader;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) ProgHeader;

typedef enum {
    FMT_NONE, FMT_IMM8, FMT_IMM32, FMT_REG_OP, FMT_MOV_IMM, 
    FMT_MODRM, FMT_REL8, FMT_REL32, FMT_ALU_IMM, FMT_SHIFT, 
    FMT_GRP3, FMT_GRP5
} InstrFormat;

typedef struct {
    uint8_t     opcode;
    uint8_t     mask;
    const char* mnem;
    InstrFormat fmt;
} InstrDef;

typedef struct {
    uint32_t addr;
    int      length;
    uint8_t  bytes[MAX_INSTR_LEN];
    char     mnem[16];
    char     op1[64];
    char     op2[64];
    uint32_t ref_addr;
    int      is_jump;
} DecodedInstr;

const char* regs32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
const char* jcc_names[] = { "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg" };
const char* alu_names[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
const char* grp3_names[] = { "test", "_unk_", "not", "neg", "mul", "imul", "div", "idiv" };
const char* shift_names[] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };
const char* grp5_names[] = { "inc", "dec", "call", "call", "jmp", "jmp", "push", "_unk_" };

static const InstrDef instr_table[] = {
    { 0x90, 0xFF, "nop",   FMT_NONE }, { 0xC3, 0xFF, "ret",   FMT_NONE },
    { 0xF4, 0xFF, "hlt",   FMT_NONE }, { 0xFA, 0xFF, "cli",   FMT_NONE },
    { 0xFB, 0xFF, "sti",   FMT_NONE }, { 0xCC, 0xFF, "int3",  FMT_NONE },
    { 0xCD, 0xFF, "int",   FMT_IMM8 }, { 0x60, 0xFF, "pusha", FMT_NONE },
    { 0x61, 0xFF, "popa",  FMT_NONE }, { 0xFC, 0xFF, "cld",   FMT_NONE },
    { 0xFD, 0xFF, "std",   FMT_NONE }, { 0xA4, 0xFF, "movsb", FMT_NONE },
    { 0xAA, 0xFF, "stosb", FMT_NONE }, { 0xAC, 0xFF, "lodsb", FMT_NONE },
    { 0xF3, 0xFF, "rep",   FMT_NONE }, { 0xE8, 0xFF, "call",  FMT_REL32 },
    { 0xE9, 0xFF, "jmp",   FMT_REL32 }, { 0xEB, 0xFF, "jmp",   FMT_REL8 },
    { 0x70, 0xF0, "jcc",   FMT_REL8 }, { 0x40, 0xF8, "inc",   FMT_REG_OP },
    { 0x48, 0xF8, "dec",   FMT_REG_OP }, { 0x50, 0xF8, "push",  FMT_REG_OP },
    { 0x58, 0xF8, "pop",   FMT_REG_OP }, { 0x68, 0xFF, "push",  FMT_IMM32 },
    { 0x6A, 0xFF, "push",  FMT_IMM8 }, { 0xB8, 0xF8, "mov",   FMT_MOV_IMM },
    { 0x88, 0xFC, "mov",   FMT_MODRM }, { 0x8A, 0xFC, "mov",   FMT_MODRM },
    { 0x87, 0xFF, "xchg",  FMT_MODRM }, { 0x00, 0xF8, "add",   FMT_MODRM },
    { 0x08, 0xF8, "or",    FMT_MODRM }, { 0x20, 0xF8, "and",   FMT_MODRM },
    { 0x28, 0xF8, "sub",   FMT_MODRM }, { 0x30, 0xF8, "xor",   FMT_MODRM },
    { 0x38, 0xF8, "cmp",   FMT_MODRM }, { 0x85, 0xFF, "test",  FMT_MODRM },
    { 0x81, 0xFF, "alu",   FMT_ALU_IMM }, { 0x83, 0xFF, "alu",   FMT_ALU_IMM },
    { 0xF7, 0xFF, "grp3",  FMT_GRP3 }, { 0xFF, 0xFF, "grp5",  FMT_GRP5 },
    { 0xC1, 0xFF, "shift", FMT_SHIFT }, { 0xD1, 0xFF, "shift", FMT_SHIFT },
    { 0xD3, 0xFF, "shift", FMT_SHIFT }, { 0x00, 0x00, NULL,    FMT_NONE }
};

static uint32_t peek32(uint8_t* buf) { return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24); }

void fmt_hex(char* buf, uint32_t val, int pad_zeros) {
    const char* h = "0123456789ABCDEF";
    *buf++ = '0'; *buf++ = 'x';
    char tmp[16]; int i = 0;
    if (val == 0) {
        if (pad_zeros) { for (int k = 0; k < pad_zeros; k++) *buf++ = '0'; } 
        else { *buf++ = '0'; }
        *buf = 0; return;
    }
    while (val > 0 || (pad_zeros > 0 && i < pad_zeros)) {
        tmp[i++] = h[val % 16]; val /= 16;
    }
    while (i > 0) *buf++ = tmp[--i];
    *buf = 0;
}

int is_printable(char c) { return (c >= 32 && c <= 126); }

int decode_modrm(uint8_t* buf, char* out_op, int* out_reg, uint32_t* ref_out) {
    uint8_t modrm = buf[0];
    int mod = (modrm >> 6) & 3;
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    int len = 1;

    if (out_reg) *out_reg = reg;

    if (mod == 3) {
        strcpy(out_op, regs32[rm]);
        return len;
    }

    char disp_s[32] = {0};
    int32_t disp = 0;

    if (rm == 4) len++; 

    if (mod == 1) { 
        disp = (int8_t)buf[len]; len += 1;
    } else if (mod == 2 || (mod == 0 && rm == 5)) {
        disp = (int32_t)peek32(buf + len); len += 4;
        if (mod == 0 && rm == 5) {
            if (ref_out) *ref_out = disp;
            fmt_hex(disp_s, disp, 0);
            strcpy(out_op, "["); strcat(out_op, disp_s); strcat(out_op, "]");
            return len;
        }
    }

    strcpy(out_op, "["); strcat(out_op, regs32[rm]);
    if (disp) {
        if (disp > 0) { strcat(out_op, "+"); fmt_hex(out_op + strlen(out_op), disp, 0); }
        else { strcat(out_op, "-"); fmt_hex(out_op + strlen(out_op), -disp, 0); }
    }
    strcat(out_op, "]");
    return len;
}

int decode_instr(uint8_t* data, uint32_t vaddr, DecodedInstr* out) {
    memset(out, 0, sizeof(DecodedInstr)); out->addr = vaddr;
    uint8_t op = data[0];
    const InstrDef* def = NULL;

    for (int i = 0; instr_table[i].mnem; i++) {
        if ((op & instr_table[i].mask) == instr_table[i].opcode) { def = &instr_table[i]; break; }
    }
    
    if (op == 0x0F) {
        uint8_t sub = data[1];
        if (sub >= 0x80 && sub <= 0x8F) {
            strcpy(out->mnem, jcc_names[sub - 0x80]);
            int32_t rel = (int32_t)peek32(data + 2);
            fmt_hex(out->op1, vaddr + 6 + rel, 8);
            out->length = 6; out->is_jump = 1;
            return 6;
        }
    }

    if (!def) {
        strcpy(out->mnem, "db"); fmt_hex(out->op1, op, 2);
        out->length = 1; return 1;
    }

    strcpy(out->mnem, def->mnem); out->length = 1;

    switch (def->fmt) {
        case FMT_NONE: break;
        case FMT_IMM8: fmt_hex(out->op1, data[1], 2); out->length += 1; break;
        case FMT_IMM32: fmt_hex(out->op1, peek32(data + 1), 8); out->length += 4; out->ref_addr = peek32(data + 1); break;
        case FMT_REG_OP: strcpy(out->op1, regs32[op & 7]); break;
        case FMT_MOV_IMM: strcpy(out->op1, regs32[op & 7]); fmt_hex(out->op2, peek32(data + 1), 8); out->length += 4; out->ref_addr = peek32(data + 1); break;
        case FMT_REL8: { int8_t r = (int8_t)data[1]; if (op >= 0x70) strcpy(out->mnem, jcc_names[op - 0x70]); fmt_hex(out->op1, vaddr + 2 + r, 0); out->length += 1; out->is_jump = 1; break; }
        case FMT_REL32: { int32_t r = (int32_t)peek32(data + 1); fmt_hex(out->op1, vaddr + 5 + r, 8); out->length += 4; out->is_jump = 1; break; }
        case FMT_MODRM: {
            int r; int l = decode_modrm(data + 1, out->op2, &r, &out->ref_addr);
            strcpy(out->op1, regs32[r]);
            if ((op & 2) == 0) { char t[64]; strcpy(t, out->op1); strcpy(out->op1, out->op2); strcpy(out->op2, t); }
            out->length += l; break;
        }
        case FMT_ALU_IMM: {
            int rx; int l = decode_modrm(data + 1, out->op1, &rx, &out->ref_addr);
            strcpy(out->mnem, alu_names[rx]);
            uint32_t imm = (op == 0x81) ? peek32(data + 1 + l) : data[1 + l];
            fmt_hex(out->op2, imm, (op == 0x81) ? 8 : 2);
            out->length += l + ((op == 0x81) ? 4 : 1); break;
        }
        case FMT_GRP5: {
            int rx; int l = decode_modrm(data + 1, out->op1, &rx, &out->ref_addr);
            strcpy(out->mnem, grp5_names[rx]); out->length += l;
            if (rx == 2 || rx == 4) out->is_jump = 1; break;
        }
        case FMT_GRP3: {
            int rx; int l = decode_modrm(data + 1, out->op1, &rx, &out->ref_addr);
            strcpy(out->mnem, grp3_names[rx]);
            if (rx == 0) { fmt_hex(out->op2, peek32(data + 1 + l), 8); out->length += l + 4; }
            else out->length += l; break;
        }
        case FMT_SHIFT: {
            int rx; int l = decode_modrm(data + 1, out->op1, &rx, NULL);
            strcpy(out->mnem, shift_names[rx]);
            if (op == 0xD1) strcpy(out->op2, "1");
            else if (op == 0xD3) strcpy(out->op2, "cl");
            else { fmt_hex(out->op2, data[1 + l], 2); out->length += 1; }
            out->length += l; break;
        }
    }

    for (int k = 0; k < out->length; k++) out->bytes[k] = data[k];
    return out->length;
}

void print_colored(DecodedInstr* in) {
    set_console_color(C_ADDR, C_BG_DEFAULT);
    printf("0x%x:  ", in->addr);
    
    set_console_color(C_BYTES, C_BG_DEFAULT);
    for (int i = 0; i < 6; i++) {
        if (i < in->length) {
            if (in->bytes[i] < 16) print("0");
            printf("%x ", in->bytes[i]);
        } else print("   ");
    }
    
    uint32_t mc = C_MNEM;
    if (in->is_jump) mc = C_JUMP;
    if (strcmp(in->mnem, "int") == 0) mc = C_NUM;
    if (strcmp(in->mnem, "ret") == 0) mc = C_JUMP;
    
    set_console_color(mc, C_BG_DEFAULT);
    printf("  %s", in->mnem);
    
    int p = strlen(in->mnem); while (p++ < 7) print(" ");

    if (in->op1[0]) {
        if (in->op1[0] == '0' && in->op1[1] == 'x') set_console_color(C_NUM, C_BG_DEFAULT);
        else if (in->op1[0] == '[') set_console_color(C_LABEL, C_BG_DEFAULT);
        else set_console_color(C_REG, C_BG_DEFAULT);
        printf("%s", in->op1);
    }
    
    if (in->op2[0]) {
        set_console_color(C_BYTES, C_BG_DEFAULT); print(", ");
        if (in->op2[0] == '0' && in->op2[1] == 'x') set_console_color(C_NUM, C_BG_DEFAULT);
        else if (in->op2[0] == '[') set_console_color(C_LABEL, C_BG_DEFAULT);
        else set_console_color(C_REG, C_BG_DEFAULT);
        printf("%s", in->op2);
    }
    
    print("\n");
    
    if (in->length > 6) {
        set_console_color(C_BYTES, C_BG_DEFAULT);
        print("           ");
        for (int i = 6; i < in->length; i++) {
            if (in->bytes[i] < 16) print("0"); printf("%x ", in->bytes[i]);
        }
        print("\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { print("Usage: dasm <file>\n"); return 1; }

    int fd = open(argv[1], 0);
    if (fd < 0) { print("Error: Could not open file.\n"); return 1; }

    uint8_t* buf = malloc(MAX_FILE_SIZE);
    int size = read(fd, buf, MAX_FILE_SIZE);
    close(fd);

    ElfHeader* eh = (ElfHeader*)buf;
    if (size < sizeof(ElfHeader) || eh->e_ident[0] != 0x7F) {
        print("Error: Not a valid ELF file.\n"); free(buf); return 1;
    }

    ProgHeader* ph = (ProgHeader*)(buf + eh->e_phoff);
    uint32_t code_off = ph->p_offset + (eh->e_entry - ph->p_vaddr);
    uint32_t vaddr = eh->e_entry;
    
    uint32_t data_start_vaddr = vaddr + size;

    int cur = code_off;
    uint32_t sim_vaddr = vaddr;
    DecodedInstr tmp;
    
    print("Analyzing binary structure...\n");
    while (cur < size) {
        if (cur > code_off + 32000) break; 
        int len = decode_instr(buf + cur, sim_vaddr, &tmp);
        if (tmp.ref_addr >= vaddr && tmp.ref_addr < vaddr + size) {
            if (tmp.ref_addr > sim_vaddr) {
                if (tmp.ref_addr < data_start_vaddr) data_start_vaddr = tmp.ref_addr;
            }
        }
        cur += len; sim_vaddr += len;
        if (sim_vaddr >= data_start_vaddr) break;
    }
    
    uint32_t data_offset = code_off + (data_start_vaddr - vaddr);
    
    set_console_color(C_DEFAULT, C_BG_DEFAULT);
    char cls = 0x0C; write(1, &cls, 1); 
    
    printf("Disassembly: %s\n", argv[1]);
    printf("Code Range:  0x%x - 0x%x\n", vaddr, data_start_vaddr);
    printf("Data Start:  0x%x\n", data_start_vaddr);
    printf("--------------------------------------------------\n");

    cur = code_off;
    sim_vaddr = vaddr;
    
    while (cur < data_offset && cur < size) {
        int len = decode_instr(buf + cur, sim_vaddr, &tmp);
        print_colored(&tmp);
        cur += len; sim_vaddr += len;
    }
    
    if (cur < size) {
        set_console_color(C_DATA_HDR, C_BG_DEFAULT);
        printf("\n--- Data Section (0x%x) ---\n", sim_vaddr);
        
        while (cur < size) {
            set_console_color(C_ADDR, C_BG_DEFAULT);
            printf("0x%x:  ", sim_vaddr);
            
            set_console_color(C_BYTES, C_BG_DEFAULT);
            int chunk = (size - cur) > 16 ? 16 : (size - cur);
            
            for (int i = 0; i < 16; i++) {
                if (i < chunk) { 
                    if (buf[cur + i] < 16) print("0"); 
                    printf("%x ", buf[cur + i]); 
                } else print("   ");
            }
            
            set_console_color(C_DATA_TXT, C_BG_DEFAULT);
            print(" |");
            for (int i = 0; i < chunk; i++) {
                char c = buf[cur + i];
                char s[2] = { is_printable(c) ? c : '.', 0 };
                print(s);
            }
            print("|\n");
            
            cur += 16; sim_vaddr += 16;
        }
    }

    set_console_color(C_DEFAULT, C_BG_DEFAULT); 
    free(buf);
    return 0;
}