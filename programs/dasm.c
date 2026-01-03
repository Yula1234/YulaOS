// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234
// DASM v2.2 - Smart ELF Disassembler

#include <yula.h>

#define MAX_FILE_SIZE   (1024 * 1024) 
#define MAX_SYMBOLS     4096

#define C_RESET         0xD4D4D4
#define C_ADDR          0x569CD6
#define C_BYTES         0x606060
#define C_MNEM          0xC586C0
#define C_REG           0x9CDCFE
#define C_NUM           0xB5CEA8
#define C_SYM           0xCE9178
#define C_SECTION       0x4EC9B0
#define C_BG            0x1E1E1E

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

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_NOBITS   8
#define SHF_WRITE    1
#define SHF_ALLOC    2
#define SHF_EXECINSTR 4

uint8_t* file_buf;
uint32_t file_size;
Elf32_Ehdr* ehdr;
Elf32_Shdr* shdrs;
char* shstrtab;

typedef struct {
    char name[64];
    uint32_t value;
    uint32_t size;
    int type;
} Symbol;

Symbol symbols[MAX_SYMBOLS];
int sym_count = 0;

void panic(const char* msg) {
    set_console_color(0xF44747, C_BG);
    printf("Error: %s\n", msg);
    set_console_color(C_RESET, C_BG);
    exit(1);
}

const char* get_reg_name(int reg, int size) {
    static const char* r8[]  = {"al","cl","dl","bl","ah","ch","dh","bh"};
    static const char* r16[] = {"ax","cx","dx","bx","sp","bp","si","di"};
    static const char* r32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
    if (size == 1) return r8[reg & 7];
    if (size == 2) return r16[reg & 7];
    return r32[reg & 7];
}

void load_symbols() {
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            Elf32_Sym* sym_data = (Elf32_Sym*)(file_buf + shdrs[i].sh_offset);
            int count = shdrs[i].sh_size / sizeof(Elf32_Sym);
            char* strtab = (char*)(file_buf + shdrs[shdrs[i].sh_link].sh_offset);
            
            for (int j = 0; j < count; j++) {
                if (sym_data[j].st_name != 0 && sym_data[j].st_value != 0) {
                    if (sym_count < MAX_SYMBOLS) {
                        Symbol* s = &symbols[sym_count++];
                        strncpy(s->name, strtab + sym_data[j].st_name, 63);
                        s->value = sym_data[j].st_value;
                        s->size = sym_data[j].st_size;
                        s->type = sym_data[j].st_info & 0xF;
                    }
                }
            }
        }
    }
}

const char* find_symbol(uint32_t addr) {
    for (int i = 0; i < sym_count; i++) {
        if (symbols[i].value == addr) return symbols[i].name;
    }
    return 0;
}

typedef struct {
    char mnem[16];
    char op1[64];
    char op2[64];
    int len;
    uint8_t bytes[16];
} Instr;

int decode_modrm(uint8_t* p, int size, char* out, int* reg_out) {
    uint8_t modrm = *p;
    int mod = (modrm >> 6) & 3;
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    int len = 1;
    
    if (reg_out) *reg_out = reg;
    
    if (mod == 3) {
        strcpy(out, get_reg_name(rm, size));
        return len;
    }
    
    int32_t disp = 0;
    int has_disp = 0;
    
    if (rm == 4) {
        uint8_t sib = p[1];
        int scale = (sib >> 6) & 3;
        int index = (sib >> 3) & 7;
        int base  = sib & 7;
        len++;
        
        if (base == 5 && mod == 0) {
            disp = *(int32_t*)(p + len);
            len += 4;
            sprintf(out, "[0x%x", disp);
        } else {
            sprintf(out, "[%s", get_reg_name(base, 4));
        }
        
        if (index != 4) {
            char tmp[16];
            sprintf(tmp, "+%s*%d", get_reg_name(index, 4), 1 << scale);
            strcat(out, tmp);
        }
    } else {
        if (mod == 0 && rm == 5) {
            disp = *(int32_t*)(p + len);
            len += 4;
            sprintf(out, "[0x%x", disp);
            strcat(out, "]");
            return len;
        } else {
            sprintf(out, "[%s", get_reg_name(rm, 4));
        }
    }
    
    if (mod == 1) {
        disp = (int8_t)p[len];
        len += 1;
        has_disp = 1;
    } else if (mod == 2) {
        disp = *(int32_t*)(p + len);
        len += 4;
        has_disp = 1;
    }
    
    if (has_disp) {
        char tmp[16];
        if (disp < 0) sprintf(tmp, "-0x%x", -disp);
        else sprintf(tmp, "+0x%x", disp);
        strcat(out, tmp);
    }
    
    strcat(out, "]");
    return len;
}

void disasm_one(uint8_t* data, uint32_t vaddr, Instr* ins) {
    memset(ins, 0, sizeof(Instr));
    int i = 0;
    int size = 4;
    
    if (data[i] == 0x66) { size = 2; i++; }
    if (data[i] == 0xF3) { strcpy(ins->mnem, "rep "); i++; }
    if (data[i] == 0xF2) { strcpy(ins->mnem, "repne "); i++; }

    uint8_t op = data[i++];
    uint32_t val32;
    uint8_t val8;
    int32_t rel32;
    int8_t rel8;

    switch (op) {
        case 0x90: strcpy(ins->mnem, "nop"); break;
        case 0xC3: strcpy(ins->mnem, "ret"); break;
        case 0xC9: strcpy(ins->mnem, "leave"); break;
        case 0xF4: strcpy(ins->mnem, "hlt"); break;
        case 0xFA: strcpy(ins->mnem, "cli"); break;
        case 0xFB: strcpy(ins->mnem, "sti"); break;
        case 0x60: strcpy(ins->mnem, "pusha"); break;
        case 0x61: strcpy(ins->mnem, "popa"); break;
        
        case 0xCD:
            strcpy(ins->mnem, "int");
            val8 = data[i++];
            sprintf(ins->op1, "0x%x", val8);
            break;

        case 0x68: 
            strcpy(ins->mnem, "push"); 
            val32 = *(uint32_t*)(data + i); i+=4;
            sprintf(ins->op1, "0x%x", val32); 
            break;
        case 0x6A: 
            strcpy(ins->mnem, "push"); 
            val8 = data[i++];
            sprintf(ins->op1, "0x%x", val8); 
            break;
        case 0x50 ... 0x57: strcpy(ins->mnem, "push"); strcpy(ins->op1, get_reg_name(op-0x50, 4)); break;
        case 0x58 ... 0x5F: strcpy(ins->mnem, "pop"); strcpy(ins->op1, get_reg_name(op-0x58, 4)); break;

        case 0x40 ... 0x47: strcpy(ins->mnem, "inc"); strcpy(ins->op1, get_reg_name(op-0x40, 4)); break;
        case 0x48 ... 0x4F: strcpy(ins->mnem, "dec"); strcpy(ins->op1, get_reg_name(op-0x48, 4)); break;

        case 0xB0 ... 0xB7: 
            strcpy(ins->mnem, "mov"); 
            strcpy(ins->op1, get_reg_name(op-0xB0, 1)); 
            val8 = data[i++];
            sprintf(ins->op2, "0x%x", val8); 
            break;
        case 0xB8 ... 0xBF: 
            strcpy(ins->mnem, "mov"); 
            strcpy(ins->op1, get_reg_name(op-0xB8, 4)); 
            val32 = *(uint32_t*)(data + i); i+=4;
            sprintf(ins->op2, "0x%x", val32); 
            break;
        case 0x88: strcpy(ins->mnem, "mov"); i += decode_modrm(data+i, 1, ins->op1, NULL); break; 
        case 0x89: strcpy(ins->mnem, "mov"); i += decode_modrm(data+i, 4, ins->op1, NULL); break; 
        case 0x8A: strcpy(ins->mnem, "mov"); { int r; int l=decode_modrm(data+i, 1, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 1)); i+=l; } break;
        case 0x8B: strcpy(ins->mnem, "mov"); { int r; int l=decode_modrm(data+i, 4, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 4)); i+=l; } break;
        case 0xC6: 
            strcpy(ins->mnem, "mov"); 
            i += decode_modrm(data+i, 1, ins->op1, NULL); 
            val8 = data[i++];
            sprintf(ins->op2, "0x%x", val8); 
            break;
        case 0xC7: 
            strcpy(ins->mnem, "mov"); 
            i += decode_modrm(data+i, 4, ins->op1, NULL); 
            val32 = *(uint32_t*)(data + i); i+=4;
            sprintf(ins->op2, "0x%x", val32); 
            break;
        
        case 0x00: strcpy(ins->mnem, "add"); i += decode_modrm(data+i, 1, ins->op1, NULL); break;
        case 0x01: strcpy(ins->mnem, "add"); i += decode_modrm(data+i, 4, ins->op1, NULL); break;
        case 0x02: strcpy(ins->mnem, "add"); { int r; int l=decode_modrm(data+i, 1, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 1)); i+=l; } break;
        case 0x03: strcpy(ins->mnem, "add"); { int r; int l=decode_modrm(data+i, 4, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 4)); i+=l; } break;

        case 0x29: strcpy(ins->mnem, "sub"); i += decode_modrm(data+i, 4, ins->op1, NULL); break;
        case 0x31: strcpy(ins->mnem, "xor"); i += decode_modrm(data+i, 4, ins->op1, NULL); break;
        
        case 0x38: strcpy(ins->mnem, "cmp"); i += decode_modrm(data+i, 1, ins->op1, NULL); break;
        case 0x39: strcpy(ins->mnem, "cmp"); i += decode_modrm(data+i, 4, ins->op1, NULL); break;
        case 0x3A: strcpy(ins->mnem, "cmp"); { int r; int l=decode_modrm(data+i, 1, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 1)); i+=l; } break;
        case 0x3B: strcpy(ins->mnem, "cmp"); { int r; int l=decode_modrm(data+i, 4, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 4)); i+=l; } break;

        case 0x84: strcpy(ins->mnem, "test"); i += decode_modrm(data+i, 1, ins->op1, NULL); break;
        case 0x85: strcpy(ins->mnem, "test"); i += decode_modrm(data+i, 4, ins->op1, NULL); break;

        case 0x83: 
        case 0x81: {
            int r; 
            int l = decode_modrm(data+i, 4, ins->op1, &r);
            i += l;
            static const char* grp1[] = {"add","or","adc","sbb","and","sub","xor","cmp"};
            strcpy(ins->mnem, grp1[r]);
            uint32_t imm;
            if (op == 0x83) { imm = (int8_t)data[i++]; } 
            else { imm = *(uint32_t*)(data + i); i+=4; }
            sprintf(ins->op2, "0x%x", imm);
            break;
        }

        case 0xE9: {
            strcpy(ins->mnem, "jmp"); 
            rel32 = *(int32_t*)(data + i); i+=4;
            uint32_t target = vaddr + i + rel32;
            const char* sym = find_symbol(target);
            if (sym) sprintf(ins->op1, "<%s>", sym);
            else sprintf(ins->op1, "0x%x", target);
            break;
        }
        case 0xEB: {
            strcpy(ins->mnem, "jmp");
            rel8 = (int8_t)data[i++];
            sprintf(ins->op1, "0x%x", vaddr + i + rel8);
            break;
        }
        case 0xE8: {
            strcpy(ins->mnem, "call");
            rel32 = *(int32_t*)(data + i); i+=4;
            uint32_t target = vaddr + i + rel32;
            const char* sym = find_symbol(target);
            if (sym) sprintf(ins->op1, "<%s>", sym);
            else sprintf(ins->op1, "0x%x", target);
            break;
        }
        
        case 0x0F: {
            uint8_t sub = data[i++];
            if (sub >= 0x80 && sub <= 0x8F) { 
                static const char* jcc[] = {"jo","jno","jb","jae","je","jne","jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg"};
                strcpy(ins->mnem, jcc[sub - 0x80]);
                rel32 = *(int32_t*)(data + i); i+=4;
                sprintf(ins->op1, "0x%x", vaddr + i + rel32);
            } 
            else if (sub == 0xB6 || sub == 0xB7) {
                strcpy(ins->mnem, "movzx");
                int r;
                int l = decode_modrm(data+i, (sub==0xB6)?1:2, ins->op2, &r);
                strcpy(ins->op1, get_reg_name(r, 4));
                i += l;
            }
            else if (sub == 0xBE || sub == 0xBF) {
                strcpy(ins->mnem, "movsx");
                int r;
                int l = decode_modrm(data+i, (sub==0xBE)?1:2, ins->op2, &r);
                strcpy(ins->op1, get_reg_name(r, 4));
                i += l;
            }
            else {
                sprintf(ins->mnem, "db 0x0F, 0x%02X", sub);
            }
            break;
        }
        
        case 0x8D:
            strcpy(ins->mnem, "lea");
            { int r; int l=decode_modrm(data+i, 4, ins->op2, &r); strcpy(ins->op1, get_reg_name(r, 4)); i+=l; }
            break;

        default:
            sprintf(ins->mnem, "db 0x%02X", op);
            break;
    }
    
    ins->len = i;
    memcpy(ins->bytes, data, i);
}

void print_hexdump(uint8_t* data, uint32_t size, uint32_t base_addr) {
    for (uint32_t i = 0; i < size; i += 16) {
        set_console_color(C_ADDR, C_BG);
        printf("%08x: ", base_addr + i);
        
        set_console_color(C_BYTES, C_BG);
        for (int j = 0; j < 16; j++) {
            if (i + j < size) printf("%02x ", data[i+j]);
            else print("   ");
        }
        
        set_console_color(C_NUM, C_BG);
        print("|");
        for (int j = 0; j < 16; j++) {
            if (i + j < size) {
                char c = data[i+j];
                if (c < 32 || c > 126) c = '.';
                char s[2] = {c, 0};
                print(s);
            }
        }
        print("|\n");
    }
}

void print_section(Elf32_Shdr* sh) {
    uint8_t* sec_data = file_buf + sh->sh_offset;
    const char* name = shstrtab + sh->sh_name;
    
    set_console_color(C_SECTION, C_BG);
    printf("\nSection %s (Addr: %08x, Size: %d)\n", name, sh->sh_addr, sh->sh_size);
    
    if (sh->sh_flags & SHF_EXECINSTR) {
        uint32_t offset = 0;
        Instr ins;
        while (offset < sh->sh_size) {
            uint32_t vaddr = sh->sh_addr + offset;
            
            const char* sym = find_symbol(vaddr);
            if (sym) {
                set_console_color(C_SYM, C_BG);
                printf("\n<%s>:\n", sym);
            }
            
            disasm_one(sec_data + offset, vaddr, &ins);
            
            set_console_color(C_ADDR, C_BG);
            printf("  %08x: ", vaddr);
            
            set_console_color(C_BYTES, C_BG);
            for (int k=0; k<6; k++) {
                if (k < ins.len) printf("%02x ", ins.bytes[k]);
                else print("   ");
            }
            
            set_console_color(C_MNEM, C_BG);
            printf(" %-6s ", ins.mnem);
            
            set_console_color(C_REG, C_BG);
            if (ins.op1[0]) printf("%s", ins.op1);
            if (ins.op2[0]) printf(", %s", ins.op2);
            
            print("\n");
            offset += ins.len;
        }
    } else if (sh->sh_type == SHT_PROGBITS && (sh->sh_flags & SHF_ALLOC)) {
        print_hexdump(sec_data, sh->sh_size, sh->sh_addr);
    } else {
        set_console_color(C_BYTES, C_BG);
        print("  [No data to display]\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: dasm <file.o/exe>\n");
        return 1;
    }

    int fd = open(argv[1], 0);
    if (fd < 0) panic("Cannot open file");
    
    file_buf = malloc(MAX_FILE_SIZE);
    if (!file_buf) panic("OOM");
    
    int n = read(fd, file_buf, MAX_FILE_SIZE);
    close(fd);

    if (n < 0) panic("Read error");
    
    if (n < (int)sizeof(Elf32_Ehdr)) panic("File too small");
    
    ehdr = (Elf32_Ehdr*)file_buf;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        char msg[128];
        sprintf(msg, "Not an ELF file (magic: %02x %02x %02x %02x, read=%d)",
                (unsigned)file_buf[0], (unsigned)file_buf[1], (unsigned)file_buf[2], (unsigned)file_buf[3], n);
        panic(msg);
    }
    
    shdrs = (Elf32_Shdr*)(file_buf + ehdr->e_shoff);
    shstrtab = (char*)(file_buf + shdrs[ehdr->e_shstrndx].sh_offset);
    
    load_symbols();
    
    printf("DASM v2.2 - Disassembling %s\n", argv[1]);
    printf("Entry point: 0x%x\n", ehdr->e_entry);
    
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_flags & SHF_ALLOC) {
            print_section(&shdrs[i]);
        }
    }
    
    free(file_buf);
    set_console_color(C_RESET, C_BG);
    return 0;
}