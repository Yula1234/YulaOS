#ifndef SCC_X86_H_INCLUDED
#define SCC_X86_H_INCLUDED

#include "scc_buffer.h"

static void emit_x86_sub_esp_imm32(Buffer* text, uint32_t imm) {
    if (imm <= 0x7F) {
        buf_push_u8(text, 0x83);
        buf_push_u8(text, 0xEC);
        buf_push_u8(text, (uint8_t)imm);
        return;
    }
    buf_push_u8(text, 0x81);
    buf_push_u8(text, 0xEC);
    buf_push_u32(text, imm);
}

static void emit_x86_pop_eax(Buffer* text) {
    buf_push_u8(text, 0x58);
}

static void emit_x86_pop_ebx(Buffer* text) {
    buf_push_u8(text, 0x5B);
}

static void emit_x86_pop_edx(Buffer* text) {
    buf_push_u8(text, 0x5A);
}

static void emit_x86_int80(Buffer* text) {
    buf_push_u8(text, 0xCD);
    buf_push_u8(text, 0x80);
}

static void emit_x86_mov_eax_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x8B);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x8B);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_membp_disp_eax(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x89);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_movzx_eax_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x0F);
        buf_push_u8(text, 0xB6);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_membp_disp_al(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x88);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_prologue(Buffer* text) {
    buf_push_u8(text, 0x55);
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xE5);
}

static void emit_x86_mov_eax_imm32(Buffer* text, uint32_t imm) {
    buf_push_u8(text, 0xB8);
    buf_push_u32(text, imm);
}

static void emit_x86_mov_eax_memabs_u32(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0xA1);
    buf_push_u32(text, addr);
}

static void emit_x86_mov_memabs_u32_eax(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0xA3);
    buf_push_u32(text, addr);
}

static void emit_x86_movzx_eax_memabs_u8(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x05);
    buf_push_u32(text, addr);
}

static void emit_x86_mov_memabs_u8_al(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x05);
    buf_push_u32(text, addr);
}

static void emit_x86_push_eax(Buffer* text) {
    buf_push_u8(text, 0x50);
}

static void emit_x86_pop_ecx(Buffer* text) {
    buf_push_u8(text, 0x59);
}

static void emit_x86_mov_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xC8);
}

static void emit_x86_mov_ebx_eax(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xC3);
}

static void emit_x86_mov_eax_edx(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xD0);
}

static void emit_x86_add_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x01);
    buf_push_u8(text, 0xC8);
}

static void emit_x86_sub_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x29);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_imul_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xAF);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_cdq(Buffer* text) {
    buf_push_u8(text, 0x99);
}

static void emit_x86_idiv_ebx(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xFB);
}

static void emit_x86_test_eax_eax(Buffer* text) {
    buf_push_u8(text, 0x85);
    buf_push_u8(text, 0xC0);
}

static void emit_x86_cmp_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x39);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_xor_eax_eax(Buffer* text) {
    buf_push_u8(text, 0x31);
    buf_push_u8(text, 0xC0);
}

static void emit_x86_setcc_al(Buffer* text, uint8_t cc) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, (uint8_t)(0x90u + cc));
    buf_push_u8(text, 0xC0);
}

static uint32_t emit_x86_jcc_rel32_fixup(Buffer* text, uint8_t cc) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, (uint8_t)(0x80u + cc));
    uint32_t imm_off = text->size;
    buf_push_u32(text, 0);
    return imm_off;
}

static uint32_t emit_x86_jmp_rel32_fixup(Buffer* text) {
    buf_push_u8(text, 0xE9);
    uint32_t imm_off = text->size;
    buf_push_u32(text, 0);
    return imm_off;
}

static void patch_rel32(Buffer* text, uint32_t imm_off, uint32_t target_off) {
    int32_t rel = (int32_t)target_off - (int32_t)(imm_off + 4u);
    text->data[imm_off + 0] = (uint8_t)(rel & 0xFF);
    text->data[imm_off + 1] = (uint8_t)((rel >> 8) & 0xFF);
    text->data[imm_off + 2] = (uint8_t)((rel >> 16) & 0xFF);
    text->data[imm_off + 3] = (uint8_t)((rel >> 24) & 0xFF);
}

static void emit_x86_neg_eax(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xD8);
}

static void emit_x86_and_eax_imm32(Buffer* text, uint32_t imm) {
    buf_push_u8(text, 0x25);
    buf_push_u32(text, imm);
}

static void emit_x86_call_rel32(Buffer* text, int32_t rel32) {
    buf_push_u8(text, 0xE8);
    buf_push_u32(text, (uint32_t)rel32);
}

static void emit_x86_add_esp_imm32(Buffer* text, uint32_t imm) {
    if (imm <= 0x7F) {
        buf_push_u8(text, 0x83);
        buf_push_u8(text, 0xC4);
        buf_push_u8(text, (uint8_t)imm);
        return;
    }
    buf_push_u8(text, 0x81);
    buf_push_u8(text, 0xC4);
    buf_push_u32(text, imm);
}

static void emit_x86_epilogue(Buffer* text) {
    buf_push_u8(text, 0xC9);
    buf_push_u8(text, 0xC3);
}

#endif
