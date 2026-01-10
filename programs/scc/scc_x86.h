// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_X86_H_INCLUDED
#define SCC_X86_H_INCLUDED

#include "scc_buffer.h"

typedef enum {
    X86_REG_EAX = 0,
    X86_REG_ECX = 1,
    X86_REG_EDX = 2,
    X86_REG_EBX = 3,
    X86_REG_ESP = 4,
    X86_REG_EBP = 5,
    X86_REG_ESI = 6,
    X86_REG_EDI = 7,
} X86Reg;

static void emit_x86_modrm(Buffer* text, uint8_t mod, uint8_t reg, uint8_t rm) {
    buf_push_u8(text, (uint8_t)(((mod & 3u) << 6) | ((reg & 7u) << 3) | (rm & 7u)));
}

static void emit_x86_op_r32_r32(Buffer* text, uint8_t opcode, X86Reg rm_dst, X86Reg reg_src) {
    buf_push_u8(text, opcode);
    emit_x86_modrm(text, 3u, (uint8_t)reg_src, (uint8_t)rm_dst);
}

static void emit_x86_op_r32_membase_disp(Buffer* text, uint8_t opcode, X86Reg reg, X86Reg base, int32_t disp) {
    buf_push_u8(text, opcode);

    if (disp == 0 && base != X86_REG_EBP) {
        emit_x86_modrm(text, 0u, (uint8_t)reg, (uint8_t)base);
        return;
    }
    if (disp >= -128 && disp <= 127) {
        emit_x86_modrm(text, 1u, (uint8_t)reg, (uint8_t)base);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    emit_x86_modrm(text, 2u, (uint8_t)reg, (uint8_t)base);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_r32_imm32(Buffer* text, X86Reg dst, uint32_t imm) {
    buf_push_u8(text, (uint8_t)(0xB8u + (uint8_t)dst));
    buf_push_u32(text, imm);
}

static void emit_x86_push_r32(Buffer* text, X86Reg r) {
    buf_push_u8(text, (uint8_t)(0x50u + (uint8_t)r));
}

static void emit_x86_pop_r32(Buffer* text, X86Reg r) {
    buf_push_u8(text, (uint8_t)(0x58u + (uint8_t)r));
}

static void emit_x86_mov_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x89, dst, src);
}

static void emit_x86_mov_r32_membp_disp(Buffer* text, X86Reg dst, int32_t disp) {
    emit_x86_op_r32_membase_disp(text, 0x8B, dst, X86_REG_EBP, disp);
}

static void emit_x86_mov_membp_disp_r32(Buffer* text, int32_t disp, X86Reg src) {
    emit_x86_op_r32_membase_disp(text, 0x89, src, X86_REG_EBP, disp);
}

static void emit_x86_movzx_r32_membp_disp_u8(Buffer* text, X86Reg dst, int32_t disp) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    if (disp == 0) {
        emit_x86_modrm(text, 1u, (uint8_t)dst, (uint8_t)X86_REG_EBP);
        buf_push_u8(text, 0);
        return;
    }
    if (disp >= -128 && disp <= 127) {
        emit_x86_modrm(text, 1u, (uint8_t)dst, (uint8_t)X86_REG_EBP);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    emit_x86_modrm(text, 2u, (uint8_t)dst, (uint8_t)X86_REG_EBP);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_r32_memr32_u32(Buffer* text, X86Reg dst, X86Reg addr) {
    emit_x86_op_r32_membase_disp(text, 0x8B, dst, addr, 0);
}

static void emit_x86_movzx_r32_memr32_u8(Buffer* text, X86Reg dst, X86Reg addr) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    emit_x86_modrm(text, 0u, (uint8_t)dst, (uint8_t)addr);
}

static void emit_x86_mov_memr32_u32_r32(Buffer* text, X86Reg addr, X86Reg src) {
    buf_push_u8(text, 0x89);
    emit_x86_modrm(text, 0u, (uint8_t)src, (uint8_t)addr);
}

static void emit_x86_add_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x01, dst, src);
}

static void emit_x86_sub_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x29, dst, src);
}

static void emit_x86_imul_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xAF);
    emit_x86_modrm(text, 3u, (uint8_t)dst, (uint8_t)src);
}

static void emit_x86_cmp_r32_r32(Buffer* text, X86Reg left, X86Reg right) {
    emit_x86_op_r32_r32(text, 0x39, left, right);
}

static void emit_x86_test_r32_r32(Buffer* text, X86Reg a, X86Reg b) {
    emit_x86_op_r32_r32(text, 0x85, a, b);
}

static void emit_x86_and_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x21, dst, src);
}

static void emit_x86_or_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x09, dst, src);
}

static void emit_x86_xor_r32_r32(Buffer* text, X86Reg dst, X86Reg src) {
    emit_x86_op_r32_r32(text, 0x31, dst, src);
}

static void emit_x86_and_r32_imm32(Buffer* text, X86Reg r, uint32_t imm) {
    buf_push_u8(text, 0x81);
    emit_x86_modrm(text, 3u, 4u, (uint8_t)r);
    buf_push_u32(text, imm);
}

static void emit_x86_neg_r32(Buffer* text, X86Reg r) {
    buf_push_u8(text, 0xF7);
    emit_x86_modrm(text, 3u, 3u, (uint8_t)r);
}

static void emit_x86_shl_r32_imm8(Buffer* text, X86Reg r, uint8_t imm) {
    buf_push_u8(text, 0xC1);
    emit_x86_modrm(text, 3u, 4u, (uint8_t)r);
    buf_push_u8(text, imm);
}

static void emit_x86_shl_r32_cl(Buffer* text, X86Reg r) {
    buf_push_u8(text, 0xD3);
    emit_x86_modrm(text, 3u, 4u, (uint8_t)r);
}

static void emit_x86_shr_r32_cl(Buffer* text, X86Reg r) {
    buf_push_u8(text, 0xD3);
    emit_x86_modrm(text, 3u, 5u, (uint8_t)r);
}

static void emit_x86_sar_r32_imm8(Buffer* text, X86Reg r, uint8_t imm) {
    buf_push_u8(text, 0xC1);
    emit_x86_modrm(text, 3u, 7u, (uint8_t)r);
    buf_push_u8(text, imm);
}

static void emit_x86_sar_r32_cl(Buffer* text, X86Reg r) {
    buf_push_u8(text, 0xD3);
    emit_x86_modrm(text, 3u, 7u, (uint8_t)r);
}

static void emit_x86_idiv_r32(Buffer* text, X86Reg r) {
    buf_push_u8(text, 0xF7);
    emit_x86_modrm(text, 3u, 7u, (uint8_t)r);
}

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

static void emit_x86_lea_eax_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x8D);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x8D);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_lea_ecx_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x8D);
        buf_push_u8(text, 0x4D);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x8D);
    buf_push_u8(text, 0x8D);
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

static void emit_x86_movzx_eax_membp_disp_u16(Buffer* text, int32_t disp) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB7);
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
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

static void emit_x86_mov_membp_disp_ax(Buffer* text, int32_t disp) {
    buf_push_u8(text, 0x66);
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

static void emit_x86_mov_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xC1);
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

static void emit_x86_xor_edx_edx(Buffer* text) {
    buf_push_u8(text, 0x31);
    buf_push_u8(text, 0xD2);
}

static void emit_x86_idiv_ebx(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xFB);
}

static void emit_x86_div_ebx(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xF3);
}

static void emit_x86_test_eax_eax(Buffer* text) {
    buf_push_u8(text, 0x85);
    buf_push_u8(text, 0xC0);
}

static void emit_x86_cmp_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x39);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_mov_eax_memecx_u32(Buffer* text) {
    buf_push_u8(text, 0x8B);
    buf_push_u8(text, 0x01);
}

static void emit_x86_movzx_eax_memecx_u8(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x01);
}

static void emit_x86_movzx_eax_memecx_u16(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB7);
    buf_push_u8(text, 0x01);
}

static void emit_x86_mov_memecx_u32_eax(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x01);
}

static void emit_x86_mov_memecx_u8_al(Buffer* text) {
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x01);
}

static void emit_x86_mov_memecx_u16_ax(Buffer* text) {
    buf_push_u8(text, 0x66);
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x01);
}

static void emit_x86_mov_eax_memeax_u32(Buffer* text) {
    buf_push_u8(text, 0x8B);
    buf_push_u8(text, 0x00);
}

static void emit_x86_movzx_eax_memeax_u8(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x00);
}

static void emit_x86_movzx_eax_memeax_u16(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB7);
    buf_push_u8(text, 0x00);
}

static void emit_x86_mov_memeax_u32_eax(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x00);
}

static void emit_x86_mov_memeax_u8_al(Buffer* text) {
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x00);
}

static void emit_x86_mov_memeax_u16_ax(Buffer* text) {
    buf_push_u8(text, 0x66);
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x00);
}

static void emit_x86_shl_eax_imm8(Buffer* text, uint8_t imm) {
    buf_push_u8(text, 0xC1);
    buf_push_u8(text, 0xE0);
    buf_push_u8(text, imm);
}

static void emit_x86_shl_ecx_imm8(Buffer* text, uint8_t imm) {
    buf_push_u8(text, 0xC1);
    buf_push_u8(text, 0xE1);
    buf_push_u8(text, imm);
}

static void emit_x86_sar_eax_imm8(Buffer* text, uint8_t imm) {
    buf_push_u8(text, 0xC1);
    buf_push_u8(text, 0xF8);
    buf_push_u8(text, imm);
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
    if (!text || !text->data) {
        printf("Internal error: patch_rel32 null text buffer\n");
        exit(1);
    }
    if (imm_off > text->size || text->size - imm_off < 4u) {
        printf("Internal error: patch_rel32 out of range (imm_off=%u, text_size=%u)\n", (unsigned int)imm_off, (unsigned int)text->size);
        exit(1);
    }
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
