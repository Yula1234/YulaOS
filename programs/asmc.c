#include "yula.h"


#define MAX_SRC_SIZE    65536
#define MAX_CODE_SIZE   32768
#define MAX_LABELS      512
#define BASE_ADDR       0x08048000
#define ELF_HEADER_SIZE 0x54

typedef struct {
    uint8_t e_ident[16]; uint16_t e_type; uint16_t e_machine; uint32_t e_version;
    uint32_t e_entry; uint32_t e_phoff; uint32_t e_shoff; uint32_t e_flags;
    uint16_t e_ehsize; uint16_t e_phentsize; uint16_t e_phnum; uint16_t e_shentsize;
    uint16_t e_shnum; uint16_t e_shstrndx;
} __attribute__((packed)) ElfHeader;

typedef struct {
    uint32_t p_type; uint32_t p_offset; uint32_t p_vaddr; uint32_t p_paddr;
    uint32_t p_filesz; uint32_t p_memsz; uint32_t p_flags; uint32_t p_align;
} __attribute__((packed)) ProgHeader;

typedef enum {
    OT_NONE  = 0,  
    OT_REG   = 1,  
    OT_IMM   = 2,  
    OT_MEM   = 3,  
} OperandType;

typedef enum {
    ENC_NONE,      
    ENC_PLUS_REG,  
    ENC_IMM32,     
    ENC_IMM8,      
    ENC_M_R,       
    ENC_R_M,       
    ENC_MOV_IMM,   
    ENC_ALU_IMM,   
    ENC_JUMP_REL,  
    ENC_SHIFT_IMM, 
    ENC_M_R_IMM8,  
} EncodingType;

typedef struct {
    const char* mnemonic;
    OperandType op1_type;
    OperandType op2_type;
    uint8_t     opcode;
    uint8_t     subcode;  
    EncodingType encoding;
} InstructionDef;

typedef struct {
    OperandType type;
    int         reg;
    uint32_t    imm;
    char        label[32];
    int         has_label;
} ParsedOperand;

typedef struct {
    uint8_t* text_buf; uint32_t text_idx;
    uint8_t* data_buf; uint32_t data_idx;
    int current_section; 
    int pass;  
    uint32_t text_base_len;

    int line_num;
    int errors;  
} Context;

typedef struct {
    char name[32];
    uint32_t offset;
    int section;
} Label;

Label labels[MAX_LABELS];
int label_count = 0;

const char* reg_names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

void report_error(Context* ctx, const char* msg, const char* token) {
    if (token) {
        printf("Error line %d: %s '%s'\n", ctx->line_num, msg, token);
    } else {
        printf("Error line %d: %s\n", ctx->line_num, msg);
    }
    ctx->errors++;
}


InstructionDef instructions[] = {

    { "ret",    OT_NONE, OT_NONE, 0xC3,   0,   ENC_NONE },
    { "nop",    OT_NONE, OT_NONE, 0x90,   0,   ENC_NONE },
    { "hlt",    OT_NONE, OT_NONE, 0xF4,   0,   ENC_NONE },
    { "cli",    OT_NONE, OT_NONE, 0xFA,   0,   ENC_NONE },
    { "sti",    OT_NONE, OT_NONE, 0xFB,   0,   ENC_NONE },
    { "int",    OT_IMM,  OT_NONE, 0xCD,   0,   ENC_IMM8 },

    { "push",   OT_REG,  OT_NONE, 0x50,   0,   ENC_PLUS_REG }, 
    { "push",   OT_IMM,  OT_NONE, 0x68,   0,   ENC_IMM32 },
    { "pop",    OT_REG,  OT_NONE, 0x58,   0,   ENC_PLUS_REG }, 

    { "jmp",    OT_IMM,  OT_NONE, 0xE9,   0,   ENC_JUMP_REL },
    { "call",   OT_IMM,  OT_NONE, 0xE8,   0,   ENC_JUMP_REL },
    { "je",     OT_IMM,  OT_NONE, 0x84,   0,   ENC_JUMP_REL }, 
    { "jz",     OT_IMM,  OT_NONE, 0x84,   0,   ENC_JUMP_REL },
    { "jne",    OT_IMM,  OT_NONE, 0x85,   0,   ENC_JUMP_REL }, 
    { "jnz",    OT_IMM,  OT_NONE, 0x85,   0,   ENC_JUMP_REL },
    { "jl",     OT_IMM,  OT_NONE, 0x8C,   0,   ENC_JUMP_REL }, 
    { "jg",     OT_IMM,  OT_NONE, 0x8F,   0,   ENC_JUMP_REL }, 

    { "inc",    OT_REG,  OT_NONE, 0x40,   0,   ENC_PLUS_REG },
    { "dec",    OT_REG,  OT_NONE, 0x48,   0,   ENC_PLUS_REG },
    { "mul",    OT_REG,  OT_NONE, 0xF7,   4,   ENC_M_R },     
    { "div",    OT_REG,  OT_NONE, 0xF7,   6,   ENC_M_R },     

    { "mov",    OT_REG,  OT_IMM,  0xB8,   0,   ENC_MOV_IMM }, 
    { "mov",    OT_REG,  OT_REG,  0x89,   0,   ENC_M_R },     
    { "mov",    OT_REG,  OT_MEM,  0x8B,   0,   ENC_R_M },     
    { "mov",    OT_MEM,  OT_REG,  0x89,   0,   ENC_M_R },     

    { "add",    OT_REG,  OT_REG,  0x01,   0,   ENC_M_R },
    { "sub",    OT_REG,  OT_REG,  0x29,   0,   ENC_M_R },
    { "cmp",    OT_REG,  OT_REG,  0x39,   0,   ENC_M_R },
    { "xor",    OT_REG,  OT_REG,  0x31,   0,   ENC_M_R },
    { "or",     OT_REG,  OT_REG,  0x09,   0,   ENC_M_R },
    { "and",    OT_REG,  OT_REG,  0x21,   0,   ENC_M_R },

    { "add",    OT_REG,  OT_IMM,  0x81,   0,   ENC_ALU_IMM },
    { "or",     OT_REG,  OT_IMM,  0x81,   1,   ENC_ALU_IMM },
    { "and",    OT_REG,  OT_IMM,  0x81,   4,   ENC_ALU_IMM },
    { "sub",    OT_REG,  OT_IMM,  0x81,   5,   ENC_ALU_IMM },
    { "xor",    OT_REG,  OT_IMM,  0x81,   6,   ENC_ALU_IMM },
    { "cmp",    OT_REG,  OT_IMM,  0x81,   7,   ENC_ALU_IMM },

    { "not",    OT_REG,  OT_NONE, 0xF7,   2,   ENC_M_R },
    { "neg",    OT_REG,  OT_NONE, 0xF7,   3,   ENC_M_R },

    { "test",   OT_REG,  OT_REG,  0x85,   0,   ENC_M_R },
    { "test",   OT_REG,  OT_IMM,  0xF7,   0,   ENC_M_R_IMM8 }, 

    { "shl",    OT_REG,  OT_IMM,  0,      4,   ENC_SHIFT_IMM },
    { "shr",    OT_REG,  OT_IMM,  0,      5,   ENC_SHIFT_IMM },
    { "sar",    OT_REG,  OT_IMM,  0,      7,   ENC_SHIFT_IMM },

    { "ja",     OT_IMM,  OT_NONE, 0x87,   0,   ENC_JUMP_REL },
    { "jb",     OT_IMM,  OT_NONE, 0x82,   0,   ENC_JUMP_REL },
    { "jae",    OT_IMM,  OT_NONE, 0x83,   0,   ENC_JUMP_REL },
    { "jbe",    OT_IMM,  OT_NONE, 0x86,   0,   ENC_JUMP_REL },

    { "call",   OT_REG,  OT_NONE, 0xFF,   2,   ENC_M_R }, 
    { "jmp",    OT_REG,  OT_NONE, 0xFF,   4,   ENC_M_R },

    { "cld",    OT_NONE, OT_NONE, 0xFC,   0,   ENC_NONE },
    { "std",    OT_NONE, OT_NONE, 0xFD,   0,   ENC_NONE },
    { "rep",    OT_NONE, OT_NONE, 0xF3,   0,   ENC_NONE }, 
    { "movsb",  OT_NONE, OT_NONE, 0xA4,   0,   ENC_NONE }, 
    { "stosb",  OT_NONE, OT_NONE, 0xAA,   0,   ENC_NONE }, 
    { "lodsb",  OT_NONE, OT_NONE, 0xAC,   0,   ENC_NONE }, 

    { "xchg",   OT_REG,  OT_REG,  0x87,   0,   ENC_M_R },

    { 0, 0, 0, 0, 0, 0 } 
};

int get_reg_id(const char* s) {
    for(int i=0; i<8; i++) if(strcmp(s, reg_names[i]) == 0) return i;
    return -1;
}

uint32_t parse_number(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t val = 0; s += 2;
        while (*s) {
            val <<= 4;
            if (*s >= '0' && *s <= '9') val |= (*s - '0');
            else if ((*s|32) >= 'a' && (*s|32) <= 'f') val |= ((*s|32) - 'a' + 10);
            s++;
        }
        return val;
    }
    int res = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { res = res * 10 + (*s - '0'); s++; }
    return res * sign;
}

void register_label(Context* ctx, const char* name) {
    if (ctx->pass != 1) return;
    for (int i = 0; i < label_count; i++) 
        if (strcmp(labels[i].name, name) == 0) return;
    
    strcpy(labels[label_count].name, name);
    labels[label_count].section = ctx->current_section;
    labels[label_count].offset = (ctx->current_section == 0) ? ctx->text_idx : ctx->data_idx;
    label_count++;
}

uint32_t resolve_label(const char* name, int pass1_text_size) {
    for (int i = 0; i < label_count; i++) {
        if (strcmp(labels[i].name, name) == 0) {
            uint32_t base = BASE_ADDR + ELF_HEADER_SIZE;
            if (labels[i].section == 0) return base + labels[i].offset;
            return base + pass1_text_size + labels[i].offset;
        }
    }
    return 0;
}

void parse_token_to_operand(const char* token, ParsedOperand* op) {
    op->type = OT_NONE;
    op->has_label = 0;
    
    if (!token || !*token) return;

    int reg = get_reg_id(token);
    if (reg != -1) { op->type = OT_REG; op->reg = reg; return; }

    if (token[0] == '[') {
        char tmp[32];
        int len = strlen(token);
        if (token[len-1] == ']') {
            strncpy(tmp, token+1, len-2);
            tmp[len-2] = 0;
            int r = get_reg_id(tmp);
            if (r != -1) { op->type = OT_MEM; op->reg = r; return; }
        }
    }

    if ((token[0] >= '0' && token[0] <= '9') || token[0] == '-') {
        op->type = OT_IMM;
        op->imm = parse_number(token);
    } else {
        op->type = OT_IMM;
        op->imm = 0;
        op->has_label = 1;
        strcpy(op->label, token);
    }
}

void emit8(Context* ctx, uint8_t b) {
    if (ctx->current_section == 0) ctx->text_buf[ctx->text_idx++] = b;
    else ctx->data_buf[ctx->data_idx++] = b;
}

void emit32(Context* ctx, uint32_t v) {
    emit8(ctx, v & 0xFF);
    emit8(ctx, (v >> 8) & 0xFF);
    emit8(ctx, (v >> 16) & 0xFF);
    emit8(ctx, (v >> 24) & 0xFF);
}

void assemble_line(Context* ctx, char* line) {
    char* tokens[8]; int count = 0;
    char* ptr = line;
    while (*ptr) {
        while (*ptr == ' ' || *ptr == '\t' || *ptr == ',') *ptr++ = 0;
        if (*ptr == 0 || *ptr == ';') break;
        
        char* start = ptr;
        if (*ptr == '"') {
            ptr++; while (*ptr && *ptr != '"') ptr++;
            if (*ptr == '"') ptr++;
        } else {
            while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != ',' && *ptr != ';') ptr++;
        }
        tokens[count++] = start;
        if (count >= 8) break;
    }
    if (count == 0) return;

    int len = strlen(tokens[0]);
    if (tokens[0][len-1] == ':') {
        tokens[0][len-1] = 0; 
        if (ctx->pass == 1) register_label(ctx, tokens[0]);
        for(int i=0; i<count-1; i++) tokens[i] = tokens[i+1];
        count--;
        if (count == 0) return;
    }

    if (strcmp(tokens[0], "section") == 0) {
        if (count < 2) { report_error(ctx, "Section name missing", NULL); return; }
        if (strcmp(tokens[1], ".text") == 0) ctx->current_section = 0;
        else if (strcmp(tokens[1], ".data") == 0) ctx->current_section = 1;
        else report_error(ctx, "Unknown section", tokens[1]);
        return;
    }
    
    if (strcmp(tokens[0], "db") == 0) {
        if (count < 2) { report_error(ctx, "Data missing for db", NULL); return; }
        for(int i=1; i<count; i++) {
            if (tokens[i][0] == '"') {
                char* s = tokens[i] + 1;
                while (*s && *s != '"') emit8(ctx, *s++);
            } else emit8(ctx, parse_number(tokens[i]));
        }
        return;
    }

    ParsedOperand op1 = {0}, op2 = {0};
    if (count > 1) parse_token_to_operand(tokens[1], &op1);
    if (count > 2) parse_token_to_operand(tokens[2], &op2);
    
    if (count > 3) {
        report_error(ctx, "Too many operands", tokens[3]);
        return;
    }

    int mnemonic_found = 0;

    for (int i = 0; instructions[i].mnemonic != 0; i++) {
        InstructionDef* def = &instructions[i];
        
        if (strcmp(tokens[0], def->mnemonic) != 0) continue;
        
        mnemonic_found = 1;
        
        if (def->op1_type != OT_NONE && def->op1_type != op1.type) continue;
        if (def->op1_type == OT_NONE && count > 1) continue;

        if (def->op2_type != OT_NONE && def->op2_type != op2.type) continue;
        if (def->op2_type == OT_NONE && count > 2) continue;
        
        uint32_t val1 = op1.imm;
        uint32_t val2 = op2.imm;
        if (ctx->pass == 2) {
            if (op1.has_label) {
                val1 = resolve_label(op1.label, ctx->text_base_len);
                if (val1 == 0) report_error(ctx, "Undefined label", op1.label);
            }
            if (op2.has_label) {
                val2 = resolve_label(op2.label, ctx->text_base_len);
                if (val2 == 0) report_error(ctx, "Undefined label", op2.label);
            }
        }

        switch (def->encoding) {
            case ENC_NONE: emit8(ctx, def->opcode); break;
            case ENC_PLUS_REG: emit8(ctx, def->opcode + op1.reg); break;
            case ENC_IMM8: emit8(ctx, def->opcode); emit8(ctx, (uint8_t)val1); break;
            case ENC_IMM32: emit8(ctx, def->opcode); emit32(ctx, val1); break;
            case ENC_MOV_IMM: emit8(ctx, def->opcode + op1.reg); emit32(ctx, val2); break;
            case ENC_M_R:
                emit8(ctx, def->opcode);
                if (def->op2_type == OT_NONE) emit8(ctx, 0xC0 | (def->subcode << 3) | op1.reg);
                else emit8(ctx, 0xC0 | (op2.reg << 3) | op1.reg);
                break;
            case ENC_R_M:
                emit8(ctx, def->opcode);
                emit8(ctx, 0x00 | (op1.reg << 3) | op2.reg);
                break;
            case ENC_ALU_IMM:
                emit8(ctx, def->opcode);
                emit8(ctx, 0xC0 | (def->subcode << 3) | op1.reg);
                emit32(ctx, val2);
                break;
            case ENC_JUMP_REL:
                if (def->opcode == 0x84 || def->opcode == 0x85 || def->opcode == 0x8C || def->opcode == 0x8F) emit8(ctx, 0x0F);
                emit8(ctx, def->opcode);
                if (ctx->pass == 2) {
                    uint32_t ip = BASE_ADDR + ELF_HEADER_SIZE + ctx->text_idx + 4;
                    emit32(ctx, val1 - ip);
                } else emit32(ctx, 0);
                break;
            case ENC_SHIFT_IMM:
                emit8(ctx, 0xC1);
                emit8(ctx, 0xC0 | (def->subcode << 3) | op1.reg);
                emit8(ctx, (uint8_t)val2);
                break;
            case ENC_M_R_IMM8:
                emit8(ctx, def->opcode);
                emit8(ctx, 0xC0 | (def->subcode << 3) | op1.reg);
                emit32(ctx, val2);
                break;
        }
        
        return;
    }

    if (mnemonic_found) {
        report_error(ctx, "Invalid operands or combination", tokens[0]);
    } else {
        report_error(ctx, "Unknown instruction", tokens[0]);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print("Usage: asmc <input.asm> <output.exe>\n");
        return 1;
    }

    int fd = open(argv[1], 0);
    if (fd < 0) { print("Error reading file\n"); return 1; }
    char* src = malloc(MAX_SRC_SIZE);
    int len = read(fd, src, MAX_SRC_SIZE-1);
    src[len] = 0;
    close(fd);

    Context ctx;
    ctx.text_buf = malloc(MAX_CODE_SIZE);
    ctx.data_buf = malloc(MAX_CODE_SIZE);

    ctx.errors = 0;
    
    ctx.pass = 1;
    ctx.text_idx = 0; ctx.data_idx = 0; ctx.current_section = 0;

    ctx.line_num = 0; 
    
    char line[128];
    int i = 0;
    while (i < len) {
        ctx.line_num++;
        int j = 0;
        while (src[i] && src[i] != '\n' && j < 127) line[j++] = src[i++];
        line[j] = 0; if (src[i] == '\n') i++;
        
        assemble_line(&ctx, line);
        
        if (ctx.errors > 10) break;
    }

    if (ctx.errors > 0) {
        printf("Build failed with %d errors.\n", ctx.errors);
        free(src); free(ctx.text_buf); free(ctx.data_buf);
        return 1;
    }
    
    printf("Pass 1: %d bytes (.text: %d, .data: %d)\n", 
           ctx.text_idx + ctx.data_idx, ctx.text_idx, ctx.data_idx);
    
    ctx.text_base_len = ctx.text_idx;
    uint32_t final_data_len = ctx.data_idx;

    ctx.pass = 2;
    ctx.text_idx = 0; ctx.data_idx = 0; ctx.current_section = 0;
    ctx.line_num = 0;
    ctx.errors = 0;  


    i = 0;
    while (i < len) {
        ctx.line_num++;
        int j = 0;
        while (src[i] && src[i] != '\n' && j < 127) line[j++] = src[i++];
        line[j] = 0; if (src[i] == '\n') i++;
        assemble_line(&ctx, line);
    }

    if (ctx.errors > 0) {
        printf("Build failed with %d errors (Pass 2).\n", ctx.errors);
        free(src); free(ctx.text_buf); free(ctx.data_buf);
        return 1;
    }

    printf("Pass 2: %d bytes (.text: %d, .data: %d)\n", 
           ctx.text_idx + ctx.data_idx, ctx.text_idx, ctx.data_idx);

    ElfHeader eh; memset(&eh, 0, sizeof(eh));
    eh.e_ident[0]=0x7F; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
    eh.e_ident[4]=1; eh.e_ident[5]=1; eh.e_ident[6]=1;
    eh.e_type=2; eh.e_machine=3; eh.e_version=1;
    eh.e_entry = resolve_label("_start", ctx.text_base_len);
    if (!eh.e_entry) eh.e_entry = BASE_ADDR + ELF_HEADER_SIZE;
    eh.e_phoff = sizeof(ElfHeader); eh.e_ehsize = sizeof(ElfHeader);
    eh.e_phentsize = sizeof(ProgHeader); eh.e_phnum = 1;

    ProgHeader ph; memset(&ph, 0, sizeof(ph));
    ph.p_type=1; ph.p_offset=0; ph.p_vaddr=BASE_ADDR; ph.p_paddr=BASE_ADDR;
    ph.p_filesz = ELF_HEADER_SIZE + ctx.text_idx + final_data_len;
    ph.p_memsz = ph.p_filesz; ph.p_flags=7; ph.p_align=4096;

    int out = open(argv[2], 1);
    write(out, &eh, sizeof(eh));
    write(out, &ph, sizeof(ph));
    write(out, ctx.text_buf, ctx.text_idx);
    write(out, ctx.data_buf, final_data_len);
    close(out);

    printf("Success, Output: %s\n", argv[2]);
    
    free(src); free(ctx.text_buf); free(ctx.data_buf);
    return 0;
}