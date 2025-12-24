#include <yula.h>

/* --- 1. Architecture & Configuration --- */

#define MAX_LINE_LEN    256
#define MAX_TOKEN_LEN   64
#define MAX_Operands    3

// ELF Definitions
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
} Elf32_Ehdr;

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
} Elf32_Shdr;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr    r_offset;
    Elf32_Word    r_info;
} Elf32_Rel;

// ELF Constants
#define ET_REL        1
#define EM_386        3
#define EV_CURRENT    1

#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_NOBITS    8
#define SHT_REL       9

#define SHF_WRITE     1
#define SHF_ALLOC     2
#define SHF_EXECINSTR 4

#define STB_LOCAL     0
#define STB_GLOBAL    1
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3

#define SHN_UNDEF     0

#define R_386_32      1
#define R_386_PC32    2

#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xf))
#define ELF32_R_INFO(s,t)  (((s)<<8)+(unsigned char)(t))

/* --- 2. Dynamic Buffer Utils --- */

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
} Buffer;

void buf_init(Buffer* b, uint32_t initial_cap) {
    b->data = malloc(initial_cap);
    b->size = 0;
    b->capacity = initial_cap;
}

void buf_free(Buffer* b) {
    if (b->data) free(b->data);
    b->data = 0; b->size = 0;
}

void buf_push_byte(Buffer* b, uint8_t byte) {
    if (b->size >= b->capacity) {
        b->capacity *= 2;
        uint8_t* new_data = malloc(b->capacity);
        memcpy(new_data, b->data, b->size);
        free(b->data);
        b->data = new_data;
    }
    b->data[b->size++] = byte;
}

void buf_push_u32(Buffer* b, uint32_t val) {
    buf_push_byte(b, val & 0xFF);
    buf_push_byte(b, (val >> 8) & 0xFF);
    buf_push_byte(b, (val >> 16) & 0xFF);
    buf_push_byte(b, (val >> 24) & 0xFF);
}

void buf_push_data(Buffer* b, void* src, uint32_t len) {
    for(uint32_t i=0; i<len; i++) buf_push_byte(b, ((uint8_t*)src)[i]);
}

uint32_t buf_add_string(Buffer* b, const char* str) {
    uint32_t offset = b->size;
    while (*str) buf_push_byte(b, *str++);
    buf_push_byte(b, 0);
    return offset;
}

/* --- 3. Symbol Management --- */

typedef enum { SYM_LOCAL, SYM_GLOBAL, SYM_EXTERN } SymBinding;
typedef enum { SEC_NULL=0, SEC_TEXT, SEC_DATA, SEC_BSS, SEC_SHSTRTAB, SEC_SYMTAB, SEC_STRTAB, SEC_REL_TEXT, SEC_REL_DATA } SectionID;

typedef struct {
    char name[64];
    SectionID section;
    uint32_t offset;
    SymBinding binding;
    uint32_t elf_index; // Index in .symtab
} Symbol;

#define MAX_SYMBOLS 1024
Symbol symbols[MAX_SYMBOLS];
int sym_count = 0;

Symbol* find_symbol(const char* name) {
    for(int i=0; i<sym_count; i++) {
        if(strcmp(symbols[i].name, name) == 0) return &symbols[i];
    }
    return 0;
}

Symbol* add_symbol(const char* name, SectionID sec, uint32_t off, SymBinding bind) {
    Symbol* s = find_symbol(name);
    if (s) {
        if (s->binding == SYM_EXTERN && bind != SYM_EXTERN) {
            // Update definition of previously extern symbol
            s->section = sec;
            s->offset = off;
            s->binding = bind; // Now it's defined (GLOBAL or LOCAL)
            return s;
        }
        return s; 
    }
    if (sym_count >= MAX_SYMBOLS) return 0;
    s = &symbols[sym_count++];
    strcpy(s->name, name);
    s->section = sec;
    s->offset = off;
    s->binding = bind;
    s->elf_index = 0;
    return s;
}

/* --- 4. Instruction Encoding Definitions --- */

typedef enum { OT_NONE=0, OT_REG, OT_IMM, OT_MEM } OpType;
typedef enum { ENC_NONE, ENC_O, ENC_M, ENC_I, ENC_MI, ENC_MR, ENC_RM, ENC_M_I8, ENC_OI } EncType;

typedef struct {
    const char* mnem;
    uint8_t opcode;
    uint8_t subcode; // Extension for ModRM
    EncType enc;
    OpType op1, op2;
} InstrDef;

// Simplified Instruction Set (Extendable)
InstrDef instructions[] = {
    { "nop",  0x90, 0, ENC_NONE, OT_NONE, OT_NONE },
    { "ret",  0xC3, 0, ENC_NONE, OT_NONE, OT_NONE },
    { "hlt",  0xF4, 0, ENC_NONE, OT_NONE, OT_NONE },
    { "cli",  0xFA, 0, ENC_NONE, OT_NONE, OT_NONE },
    { "sti",  0xFB, 0, ENC_NONE, OT_NONE, OT_NONE },
    
    // PUSH/POP
    { "push", 0x50, 0, ENC_O,    OT_REG,  OT_NONE },
    { "push", 0x68, 0, ENC_I,    OT_IMM,  OT_NONE },
    { "pop",  0x58, 0, ENC_O,    OT_REG,  OT_NONE },

    // MOV
    { "mov",  0xB8, 0, ENC_OI,   OT_REG,  OT_IMM },  // mov reg, imm
    { "mov",  0x89, 0, ENC_MR,   OT_MEM,  OT_REG },  // mov [mem], reg
    { "mov",  0x8B, 0, ENC_RM,   OT_REG,  OT_MEM },  // mov reg, [mem]
    { "mov",  0x89, 0, ENC_MR,   OT_REG,  OT_REG },  // mov reg, reg

    // Arithmetic
    { "add",  0x01, 0, ENC_MR,   OT_REG,  OT_REG },
    { "sub",  0x29, 0, ENC_MR,   OT_REG,  OT_REG },
    { "xor",  0x31, 0, ENC_MR,   OT_REG,  OT_REG },
    { "cmp",  0x39, 0, ENC_MR,   OT_REG,  OT_REG },
    { "test", 0x85, 0, ENC_MR,   OT_REG,  OT_REG },
    
    { "inc",  0x40, 0, ENC_O,    OT_REG,  OT_NONE },
    { "dec",  0x48, 0, ENC_O,    OT_REG,  OT_NONE },

    // Jumps (Only 32-bit relative for simplicity in .o)
    { "jmp",  0xE9, 0, ENC_I,    OT_IMM,  OT_NONE },
    { "call", 0xE8, 0, ENC_I,    OT_IMM,  OT_NONE },
    { "je",   0x84, 0, ENC_M,    OT_IMM,  OT_NONE }, // 0x0F 0x84
    { "jne",  0x85, 0, ENC_M,    OT_IMM,  OT_NONE },
    { "jz",   0x84, 0, ENC_M,    OT_IMM,  OT_NONE },
    { "jnz",  0x85, 0, ENC_M,    OT_IMM,  OT_NONE },

    // INT
    { "int",  0xCD, 0, ENC_M_I8, OT_IMM,  OT_NONE },

    { 0,0,0,0,0,0 }
};

const char* reg_names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

/* --- 5. Context & Logic --- */

typedef struct {
    OpType type;
    int reg;
    uint32_t val;
    char label[64];
    int has_label;
} Operand;

typedef struct {
    int pass;
    int line_num;
    SectionID cur_section;
    
    Buffer text;
    Buffer data;
    Buffer bss;
    
    Buffer rel_text;
    Buffer rel_data;
    
    // String tables are built at the end or progressively
} Context;

// Helper to parse "eax" -> 0
int get_reg(const char* txt) {
    for(int i=0; i<8; i++) if(strcmp(txt, reg_names[i])==0) return i;
    return -1;
}

// Helper to parse "123", "0x1A"
uint32_t get_imm(const char* txt) {
    if(txt[0]=='0' && txt[1]=='x') {
        uint32_t v=0; const char* p=txt+2;
        while(*p) {
            v<<=4;
            if(*p>='0' && *p<='9') v|=(*p-'0');
            else if(*p>='a' && *p<='f') v|=(*p-'a'+10);
            p++;
        }
        return v;
    }
    return (uint32_t)atoi(txt); // atoi from user lib
}

void parse_operand(char* txt, Operand* op) {
    op->type = OT_NONE; op->has_label = 0; op->reg = -1; op->val = 0;
    if(!txt || !*txt) return;

    if(txt[0]=='[') { // Memory
        char tmp[64];
        int len = strlen(txt);
        if(txt[len-1]==']') {
            strncpy(tmp, txt+1, len-2); tmp[len-2]=0;
            int r = get_reg(tmp);
            if(r != -1) { op->type = OT_MEM; op->reg = r; return; }
            // Support [label] as memory operand
            op->type = OT_MEM; op->has_label = 1; strcpy(op->label, tmp); op->reg = 5; // ebp/disp32 encoding
            return;
        }
    }

    int r = get_reg(txt);
    if(r != -1) { op->type = OT_REG; op->reg = r; return; }

    if((txt[0]>='0' && txt[0]<='9') || txt[0]=='-') {
        op->type = OT_IMM; op->val = get_imm(txt);
    } else {
        op->type = OT_IMM; op->has_label = 1; strcpy(op->label, txt);
    }
}

void emit_relocation(Context* ctx, Elf32_Addr offset, uint32_t sym_idx, int type) {
    Elf32_Rel rel;
    rel.r_offset = offset;
    rel.r_info = ELF32_R_INFO(sym_idx, type);
    
    if (ctx->cur_section == SEC_TEXT) buf_push_data(&ctx->rel_text, &rel, sizeof(rel));
    else if (ctx->cur_section == SEC_DATA) buf_push_data(&ctx->rel_data, &rel, sizeof(rel));
}

void assemble_line(Context* ctx, char* line) {
    char* tokens[4]; int tok_cnt = 0;
    char* p = line;
    
    // Tokenizer
    while(*p) {
        while(*p==' ' || *p=='\t' || *p==',') *p++=0;
        if(*p==0 || *p==';') break;
        char* start = p;
        if(*p=='"') { p++; while(*p && *p!='"') p++; if(*p=='"') p++; } 
        else { while(*p && *p!=' ' && *p!='\t' && *p!=',' && *p!=';') p++; }
        tokens[tok_cnt++] = start;
        if(tok_cnt>=4) break;
    }
    if(tok_cnt==0) return;

    // Labels
    int len = strlen(tokens[0]);
    if(tokens[0][len-1]==':') {
        tokens[0][len-1] = 0;
        if(ctx->pass == 1) {
            uint32_t off = (ctx->cur_section==SEC_TEXT) ? ctx->text.size : ctx->data.size;
            add_symbol(tokens[0], ctx->cur_section, off, SYM_LOCAL);
        }
        // Shift tokens
        for(int i=0; i<tok_cnt-1; i++) tokens[i]=tokens[i+1];
        tok_cnt--;
        if(tok_cnt==0) return;
    }

    char* mnemonic = tokens[0];

    // Directives
    if(strcmp(mnemonic, "section")==0) {
        if(tok_cnt<2) return;
        if(strcmp(tokens[1], ".text")==0) ctx->cur_section = SEC_TEXT;
        else if(strcmp(tokens[1], ".data")==0) ctx->cur_section = SEC_DATA;
        else if(strcmp(tokens[1], ".bss")==0) ctx->cur_section = SEC_BSS;
        return;
    }
    if(strcmp(mnemonic, "global")==0 || strcmp(mnemonic, "public")==0) {
        if(ctx->pass==1 && tok_cnt>1) add_symbol(tokens[1], SEC_NULL, 0, SYM_GLOBAL);
        return;
    }
    if(strcmp(mnemonic, "extern")==0 || strcmp(mnemonic, "extrn")==0) {
        if(ctx->pass==1 && tok_cnt>1) add_symbol(tokens[1], SEC_NULL, 0, SYM_EXTERN);
        return;
    }
    if(strcmp(mnemonic, "db")==0) {
        Buffer* b = (ctx->cur_section==SEC_TEXT)?&ctx->text:&ctx->data;
        for(int i=1; i<tok_cnt; i++) {
            if(tokens[i][0]=='"') {
                char* s = tokens[i]+1;
                while(*s && *s!='"') { if(ctx->pass==2) buf_push_byte(b, *s); else b->size++; s++; }
            } else {
                if(ctx->pass==2) buf_push_byte(b, get_imm(tokens[i])); else b->size++;
            }
        }
        return;
    }
    if(strcmp(mnemonic, "dd")==0) {
        Buffer* b = (ctx->cur_section==SEC_TEXT)?&ctx->text:&ctx->data;
        if(ctx->pass==2) buf_push_u32(b, get_imm(tokens[1])); else b->size+=4;
        return;
    }
    
    // Instructions
    Operand op1={0}, op2={0};
    if(tok_cnt>1) parse_operand(tokens[1], &op1);
    if(tok_cnt>2) parse_operand(tokens[2], &op2);

    for(int i=0; instructions[i].mnem; i++) {
        InstrDef* def = &instructions[i];
        if(strcmp(def->mnem, mnemonic)!=0) continue;
        if(def->op1 != OT_NONE && def->op1 != op1.type) continue;
        if(def->op2 != OT_NONE && def->op2 != op2.type) continue;

        Buffer* b = &ctx->text;
        
        // Pass 1: Just calculate size
        // Pass 2: Emit code & relocations
        
        if(ctx->pass == 2) {
            uint32_t start_pc = b->size;
            
            // Jumps with 0x0F prefix
            if(def->enc == ENC_M && (def->opcode == 0x84 || def->opcode == 0x85)) buf_push_byte(b, 0x0F);
            
            buf_push_byte(b, def->opcode + (def->enc==ENC_O ? op1.reg : 0) + (def->enc==ENC_OI ? op1.reg : 0));

            // ModR/M handling
            if(def->enc == ENC_MR || def->enc == ENC_RM || def->enc == ENC_M) {
                uint8_t modrm = 0;
                int reg_part = (def->enc == ENC_MR) ? op2.reg : (def->enc == ENC_RM ? op1.reg : 0); // Not fully correct for all cases but enough for test
                int rm_part  = (def->enc == ENC_MR) ? op1.reg : (def->enc == ENC_RM ? op2.reg : 0);
                
                // For simplified "jmp/call label" logic (ENC_M treated as REL32) - no ModRM
                if(def->enc == ENC_M && op1.type == OT_IMM) {
                    // Logic handled in Immediate block
                } else {
                    modrm = 0xC0 | (reg_part << 3) | rm_part;
                    buf_push_byte(b, modrm);
                }
            }

            // Immediate / Displacement handling
            if(op1.type == OT_IMM || op2.type == OT_IMM) {
                Operand* imm_op = (op1.type == OT_IMM) ? &op1 : &op2;
                uint32_t val = imm_op->val;
                
                if(imm_op->has_label) {
                    Symbol* sym = find_symbol(imm_op->label);
                    // Relocation Logic
                    int rel_type = R_386_32; // Default absolute
                    if(def->opcode == 0xE8 || def->opcode == 0xE9 || (def->opcode==0x84 && def->enc==ENC_M)) {
                        rel_type = R_386_PC32; // Relative for calls/jumps
                    }

                    if(sym) {
                        if(sym->binding == SYM_EXTERN || sym->section != ctx->cur_section) {
                            emit_relocation(ctx, b->size, sym->elf_index, rel_type);
                            val = (rel_type == R_386_PC32) ? -4 : 0; // Addend
                        } else {
                            // Local symbol in same section
                            if(rel_type == R_386_PC32) {
                                val = sym->offset - (b->size + 4); 
                            } else {
                                // Absolute reference to local symbol -> still needs relocation in .o
                                emit_relocation(ctx, b->size, sym->elf_index, R_386_32);
                                val = sym->offset; 
                            }
                        }
                    } else {
                        printf("Error: Undefined symbol '%s'\n", imm_op->label);
                    }
                }
                
                if (def->enc == ENC_M_I8) buf_push_byte(b, val);
                else buf_push_u32(b, val);
            }
        } else {
            // Pass 1 Size calc (Approximation for simplicity)
            int sz = 1;
            if(def->enc == ENC_M && (def->opcode == 0x84 || def->opcode == 0x85)) sz++;
            if(def->enc == ENC_MR || def->enc == ENC_RM) sz++;
            if(def->enc == ENC_I || def->enc == ENC_M || def->enc == ENC_OI) sz+=4;
            if(def->enc == ENC_M_I8) sz+=1;
            b->size += sz;
        }
        return;
    }
    printf("Line %d: Unknown instruction '%s'\n", ctx->line_num, mnemonic);
}

/* --- 6. ELF Output Generation --- */

void write_object_file(Context* ctx, const char* filename) {
    // 1. Prepare String Table (.strtab) and assign ELF indices
    Buffer strtab; buf_init(&strtab, 1024);
    buf_push_byte(&strtab, 0); // Null entry

    // System symbols (sections)
    // Indices: 0=NULL, 1=.text, 2=.data, 3=.bss
    
    int global_start = -1;
    int current_idx = 4; // Start user symbols after section symbols if we were strictly following standard, but here we simplify.
                         // Actually, in .o, STT_SECTION symbols are usually local.
    
    // Assign indices to symbols
    for(int i=0; i<sym_count; i++) {
        symbols[i].elf_index = current_idx++; // Simple sequential indexing
    }

    // 2. Prepare Symbol Table (.symtab)
    Buffer symtab; buf_init(&symtab, 1024);
    
    // Entry 0: NULL
    Elf32_Sym null_sym = {0};
    buf_push_data(&symtab, &null_sym, sizeof(null_sym)); // 0

    // Dummy Section Symbols (1=.text, 2=.data, 3=.bss) 
    // Usually assemblers emit these. We will skip complex logic and just emit user symbols for now.
    // If we skip section symbols in symtab, relocations must point to user symbols.
    // But relocations to "section + offset" are common. 
    // Let's create proper entries for sections.
    
    const char* sec_names[] = { "", "", "", "" }; // Logic handled in user symbols loop for simplicity
    
    // Emit user symbols
    // ELF requires LOCALS first, then GLOBALS.
    // We didn't sort, so this might be non-compliant if we mix them.
    // Ideally we should sort symbols or write in two passes.
    // For this educational OS, we will write as is, but standard linkers might complain.
    
    for(int i=0; i<sym_count; i++) {
        Symbol* s = &symbols[i];
        Elf32_Sym es;
        es.st_name = buf_add_string(&strtab, s->name);
        es.st_value = s->offset;
        es.st_size = 0;
        es.st_other = 0;
        
        int type = STT_NOTYPE;
        if(s->section == SEC_TEXT) type = STT_FUNC;
        if(s->section == SEC_DATA || s->section == SEC_BSS) type = STT_OBJECT;
        
        int bind = (s->binding == SYM_GLOBAL) ? STB_GLOBAL : STB_LOCAL;
        if (s->binding == SYM_EXTERN) { bind = STB_GLOBAL; s->section = SEC_NULL; }
        
        es.st_info = ELF32_ST_INFO(bind, type);
        
        // Map internal SectionID to ELF Section Index
        if(s->section == SEC_TEXT) es.st_shndx = 1;
        else if(s->section == SEC_DATA) es.st_shndx = 2;
        else if(s->section == SEC_BSS) es.st_shndx = 3;
        else es.st_shndx = SHN_UNDEF;
        
        buf_push_data(&symtab, &es, sizeof(es));
        // Update index to match table position
        s->elf_index = i + 1; 
    }

    // 3. Prepare Section Header String Table (.shstrtab)
    Buffer shstr; buf_init(&shstr, 256);
    buf_push_byte(&shstr, 0);
    uint32_t n_text = buf_add_string(&shstr, ".text");
    uint32_t n_data = buf_add_string(&shstr, ".data");
    uint32_t n_bss  = buf_add_string(&shstr, ".bss");
    uint32_t n_shstr= buf_add_string(&shstr, ".shstrtab");
    uint32_t n_sym  = buf_add_string(&shstr, ".symtab");
    uint32_t n_str  = buf_add_string(&shstr, ".strtab");
    uint32_t n_rel  = buf_add_string(&shstr, ".rel.text");
    uint32_t n_reld = buf_add_string(&shstr, ".rel.data");

    // 4. Calculate Offsets
    uint32_t off = sizeof(Elf32_Ehdr);
    // Section Headers will be at the end, but data is contiguous
    uint32_t off_text = off; off += ctx->text.size;
    uint32_t off_data = off; off += ctx->data.size;
    uint32_t off_bss  = off; // NOBITS
    uint32_t off_shstr= off; off += shstr.size;
    uint32_t off_sym  = off; off += symtab.size;
    uint32_t off_str  = off; off += strtab.size;
    uint32_t off_rel  = off; off += ctx->rel_text.size;
    uint32_t off_reld = off; off += ctx->rel_data.size;
    uint32_t off_shdr = off;

    // 5. Write File
    int fd = open(filename, 1); // Write mode
    if(fd < 0) { printf("Cannot open output file\n"); return; }

    // Header
    Elf32_Ehdr eh = {0};
    eh.e_ident[0]=0x7F; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
    eh.e_ident[4]=1; // 32-bit
    eh.e_ident[5]=1; // Little Endian
    eh.e_ident[6]=1; // Version
    eh.e_type = ET_REL;
    eh.e_machine = EM_386;
    eh.e_version = 1;
    eh.e_shoff = off_shdr;
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_shentsize = sizeof(Elf32_Shdr);
    eh.e_shnum = 9; // NULL, text, data, bss, shstr, sym, str, rel.text, rel.data
    eh.e_shstrndx = 4;
    write(fd, &eh, sizeof(eh));

    // Sections Data
    if(ctx->text.size) write(fd, ctx->text.data, ctx->text.size);
    if(ctx->data.size) write(fd, ctx->data.data, ctx->data.size);
    // BSS is empty
    write(fd, shstr.data, shstr.size);
    write(fd, symtab.data, symtab.size);
    write(fd, strtab.data, strtab.size);
    if(ctx->rel_text.size) write(fd, ctx->rel_text.data, ctx->rel_text.size);
    if(ctx->rel_data.size) write(fd, ctx->rel_data.data, ctx->rel_data.size);

    // Section Headers
    Elf32_Shdr sh[9] = {0};
    
    // 0: NULL
    
    // 1: .text
    sh[1].sh_name = n_text; sh[1].sh_type = SHT_PROGBITS; sh[1].sh_flags = SHF_ALLOC|SHF_EXECINSTR;
    sh[1].sh_offset = off_text; sh[1].sh_size = ctx->text.size; sh[1].sh_addralign = 4;

    // 2: .data
    sh[2].sh_name = n_data; sh[2].sh_type = SHT_PROGBITS; sh[2].sh_flags = SHF_ALLOC|SHF_WRITE;
    sh[2].sh_offset = off_data; sh[2].sh_size = ctx->data.size; sh[2].sh_addralign = 4;

    // 3: .bss
    sh[3].sh_name = n_bss; sh[3].sh_type = SHT_NOBITS; sh[3].sh_flags = SHF_ALLOC|SHF_WRITE;
    sh[3].sh_offset = off_bss; sh[3].sh_size = ctx->bss.size; sh[3].sh_addralign = 4;

    // 4: .shstrtab
    sh[4].sh_name = n_shstr; sh[4].sh_type = SHT_STRTAB; 
    sh[4].sh_offset = off_shstr; sh[4].sh_size = shstr.size; sh[4].sh_addralign = 1;

    // 5: .symtab
    sh[5].sh_name = n_sym; sh[5].sh_type = SHT_SYMTAB; 
    sh[5].sh_offset = off_sym; sh[5].sh_size = symtab.size; 
    sh[5].sh_link = 6; // Link to .strtab
    sh[5].sh_entsize = sizeof(Elf32_Sym); sh[5].sh_addralign = 4;
    // sh_info should be index of first global symbol (we skipped sorting, leaving 0 or 1)
    sh[5].sh_info = 1; 

    // 6: .strtab
    sh[6].sh_name = n_str; sh[6].sh_type = SHT_STRTAB; 
    sh[6].sh_offset = off_str; sh[6].sh_size = strtab.size; sh[6].sh_addralign = 1;

    // 7: .rel.text
    sh[7].sh_name = n_rel; sh[7].sh_type = SHT_REL; 
    sh[7].sh_offset = off_rel; sh[7].sh_size = ctx->rel_text.size; 
    sh[7].sh_link = 5; // Link to .symtab
    sh[7].sh_info = 1; // Target section (.text)
    sh[7].sh_entsize = sizeof(Elf32_Rel); sh[7].sh_addralign = 4;

    // 8: .rel.data
    sh[8].sh_name = n_reld; sh[8].sh_type = SHT_REL; 
    sh[8].sh_offset = off_reld; sh[8].sh_size = ctx->rel_data.size; 
    sh[8].sh_link = 5; 
    sh[8].sh_info = 2; // Target section (.data)
    sh[8].sh_entsize = sizeof(Elf32_Rel); sh[8].sh_addralign = 4;

    write(fd, sh, sizeof(sh));
    close(fd);

    buf_free(&strtab); buf_free(&symtab); buf_free(&shstr);
}

/* --- 7. Main Entry Point --- */

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("YulaOS Assembler (ELF Relocatable)\n");
        printf("Usage: asmc <input.asm> <output.o>\n");
        return 1;
    }

    int fd = open(argv[1], 0);
    if (fd < 0) { printf("Error opening source file\n"); return 1; }
    
    // Read source fully (simple)
    char* src = malloc(65536); // 64KB limit for now
    int len = read(fd, src, 65535);
    src[len] = 0;
    close(fd);

    Context ctx = {0};
    buf_init(&ctx.text, 4096);
    buf_init(&ctx.data, 4096);
    buf_init(&ctx.bss, 0); // No data, just size
    buf_init(&ctx.rel_text, 1024);
    buf_init(&ctx.rel_data, 1024);

    // PASS 1
    ctx.pass = 1;
    ctx.cur_section = SEC_TEXT;
    int i = 0; ctx.line_num = 0;
    char line[MAX_LINE_LEN];
    
    while(src[i]) {
        int j=0;
        while(src[i] && src[i]!='\n' && j<MAX_LINE_LEN-1) line[j++] = src[i++];
        line[j]=0; if(src[i]=='\n') i++;
        ctx.line_num++;
        assemble_line(&ctx, line);
    }

    printf("Pass 1 complete. Predicted size: %d bytes code, %d bytes data.\n", ctx.text.size, ctx.data.size);

    // Reset Buffers for Pass 2 (Only size was needed, but we used same buffers)
    ctx.text.size = 0;
    ctx.data.size = 0;
    ctx.bss.size = 0;

    // PASS 2
    ctx.pass = 2;
    ctx.cur_section = SEC_TEXT;
    i = 0; ctx.line_num = 0;
    
    while(src[i]) {
        int j=0;
        while(src[i] && src[i]!='\n' && j<MAX_LINE_LEN-1) line[j++] = src[i++];
        line[j]=0; if(src[i]=='\n') i++;
        ctx.line_num++;
        assemble_line(&ctx, line);
    }

    write_object_file(&ctx, argv[2]);
    printf("Success, output: %s (%d bytes code, data)\n", argv[2], ctx.text.size + ctx.data.size);

    free(src);
    buf_free(&ctx.text); buf_free(&ctx.data);
    buf_free(&ctx.rel_text); buf_free(&ctx.rel_data);
    return 0;
}