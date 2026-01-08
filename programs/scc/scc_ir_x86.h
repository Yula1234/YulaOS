// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_IR_X86_H_INCLUDED
#define SCC_IR_X86_H_INCLUDED

#include "scc_ir.h"
#include "scc_diag.h"
#include "scc_x86.h"

typedef struct {
    Buffer* text;
    Buffer* data;
    Buffer* rel_text;
    Buffer* rel_data;
    SymTable* syms;
} IrX86Ctx;

typedef struct {
    uint32_t imm_off;
    IrBlockId target;
} IrX86Fixup;

typedef enum {
    IR_X86_LOC_NONE = 0,
    IR_X86_LOC_REG,
    IR_X86_LOC_STACK,
} IrX86LocKind;

typedef struct IrX86Loc {
    IrX86LocKind kind;
    X86Reg reg;
    int32_t disp;
} IrX86Loc;

static void ir_x86_emit_reloc_text(IrX86Ctx* cx, uint32_t offset, int sym_index, int type) {
    if (!cx || !cx->rel_text) {
        scc_fatal_at(0, 0, 0, 0, "Internal error: ir_x86_emit_reloc_text missing context");
    }
    if (type != R_386_32 && type != R_386_PC32) {
        scc_fatal_at(0, 0, 0, 0, "Internal error: ir_x86_emit_reloc_text bad relocation type");
    }
    if (sym_index <= 0) {
        scc_fatal_at(0, 0, 0, 0, "Internal error: ir_x86_emit_reloc_text bad symbol index");
    }
    Elf32_Rel r;
    r.r_offset = (Elf32_Addr)offset;
    r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
    buf_write(cx->rel_text, &r, sizeof(r));
}

static void ir_x86_store_eax_to_slot(Buffer* text, IrType* ty, int32_t disp) {
    if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_U8 || ty->kind == IR_TY_BOOL)) {
        emit_x86_mov_membp_disp_al(text, disp);
        return;
    }
    if (ty && (ty->kind == IR_TY_I16 || ty->kind == IR_TY_U16)) {
        emit_x86_mov_membp_disp_ax(text, disp);
        return;
    }
    emit_x86_mov_membp_disp_eax(text, disp);
}

static void ir_x86_load_slot_to_eax(Buffer* text, IrType* ty, int32_t disp) {
    if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_U8 || ty->kind == IR_TY_BOOL)) {
        emit_x86_movzx_eax_membp_disp(text, disp);
        return;
    }
    if (ty && (ty->kind == IR_TY_I16 || ty->kind == IR_TY_U16)) {
        emit_x86_movzx_eax_membp_disp_u16(text, disp);
        return;
    }
    emit_x86_mov_eax_membp_disp(text, disp);
}

static uint8_t ir_x86_icmp_cc(IrIcmpPred p) {
    if (p == IR_ICMP_EQ) return 0x4;
    if (p == IR_ICMP_NE) return 0x5;
    if (p == IR_ICMP_SLT) return 0xC;
    if (p == IR_ICMP_SLE) return 0xE;
    if (p == IR_ICMP_SGT) return 0xF;
    if (p == IR_ICMP_SGE) return 0xD;
    if (p == IR_ICMP_ULT) return 0x2;
    if (p == IR_ICMP_ULE) return 0x6;
    if (p == IR_ICMP_UGT) return 0x7;
    if (p == IR_ICMP_UGE) return 0x3;
    return 0x4;
}

static void ir_x86_load_value_to_eax(IrFunc* f, Buffer* text, IrValueId v, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!f || !text) return;
    if (v == 0) {
        emit_x86_mov_eax_imm32(text, 0);
        return;
    }

    if (v > f->value_count) {
        scc_fatal_at(0, 0, 0, 0, "Internal error: invalid IR value id in x86 load");
    }

    IrType* ty = f->values[v - 1].type;

    if (value_loc && value_loc[v].kind == IR_X86_LOC_REG) {
        X86Reg r = value_loc[v].reg;
        if (r != X86_REG_EAX) emit_x86_mov_r32_r32(text, X86_REG_EAX, r);

        if (ty && ty->kind == IR_TY_BOOL) emit_x86_and_eax_imm32(text, 1u);
        else if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_U8)) emit_x86_and_eax_imm32(text, 0xFFu);
        else if (ty && (ty->kind == IR_TY_I16 || ty->kind == IR_TY_U16)) emit_x86_and_eax_imm32(text, 0xFFFFu);
        return;
    }
    ir_x86_load_slot_to_eax(text, ty, value_disp[v]);
}

static void ir_x86_store_value_from_eax(IrFunc* f, Buffer* text, IrValueId v, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!f || !text) return;
    if (v == 0) return;

    if (v > f->value_count) {
        scc_fatal_at(0, 0, 0, 0, "Internal error: invalid IR value id in x86 store");
    }

    IrType* ty = f->values[v - 1].type;

    if (value_loc && value_loc[v].kind == IR_X86_LOC_REG) {
        X86Reg r = value_loc[v].reg;
        if (ty && ty->kind == IR_TY_BOOL) emit_x86_and_eax_imm32(text, 1u);
        else if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_U8)) emit_x86_and_eax_imm32(text, 0xFFu);
        else if (ty && (ty->kind == IR_TY_I16 || ty->kind == IR_TY_U16)) emit_x86_and_eax_imm32(text, 0xFFFFu);
        if (r != X86_REG_EAX) emit_x86_mov_r32_r32(text, r, X86_REG_EAX);
        return;
    }
    ir_x86_store_eax_to_slot(text, ty, value_disp[v]);
}

static void ir_x86_emit_phi_moves(IrFunc* f, Buffer* text, IrBlockId target, IrValueId* args, uint32_t arg_count, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!f || !text || !value_disp) return;
    if (target == 0 || target > f->block_count) return;

    IrBlock* b = &f->blocks[target - 1];
    uint32_t n = arg_count;
    if (b->param_count < n) n = b->param_count;

    for (uint32_t i = 0; i < n; i++) {
        ir_x86_load_value_to_eax(f, text, args ? args[i] : 0, value_disp, value_loc);
        emit_x86_push_eax(text);
    }
    for (uint32_t i = n; i > 0; i--) {
        emit_x86_pop_eax(text);
        ir_x86_store_value_from_eax(f, text, b->params[i - 1], value_disp, value_loc);
    }
}

static uint32_t ir_x86_slot_size(IrType* ty) {
    uint32_t sz = ir_type_size(ty);
    if (sz == 0) return 0;
    if (sz < 4) sz = 4;
    return sz;
}

static void ir_x86_assign_frame(IrFunc* f, int32_t* value_disp, int32_t* alloca_mem_disp, uint32_t* out_frame_size) {
    if (!f || !value_disp || !alloca_mem_disp || !out_frame_size) return;

    uint32_t off = 0;
    for (IrValueId v = 1; v <= f->value_count; v++) {
        IrType* ty = f->values[v - 1].type;
        if (!ty || ty->kind == IR_TY_VOID) {
            value_disp[v] = 0;
            continue;
        }
        uint32_t sz = ir_x86_slot_size(ty);
        off = align_up_u32(off, 4);
        off += sz;
        value_disp[v] = -(int32_t)off;
    }

    for (IrInstrId iid = 1; iid <= f->instr_count; iid++) {
        IrInstr* ins = &f->instrs[iid - 1];
        if (ins->kind != IR_INSTR_ALLOCA) continue;
        if (ins->result == 0) continue;
        if (ins->result > f->value_count) {
            scc_fatal_at(0, 0, 0, 0, "Internal error: IR alloca result id out of range in frame assign");
        }

        uint32_t al = ins->v.alloca.align;
        if (al == 0) al = 4;

        uint32_t sz = ir_type_size(ins->v.alloca.alloc_ty);
        sz = align_up_u32(sz, al);
        off = align_up_u32(off, al);
        off += sz;
        alloca_mem_disp[ins->result] = -(int32_t)off;
    }

    *out_frame_size = align_up_u32(off, 4);
}

static void ir_x86_emit_instr_simple(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp, int32_t* alloca_mem_disp, IrX86Loc* value_loc) {
    if (!cx || !f || !ins || !value_disp || !alloca_mem_disp) return;
    Buffer* text = cx->text;

    if (ins->kind == IR_INSTR_UNDEF) {
        if (ins->result == 0) return;
        emit_x86_mov_eax_imm32(text, 0);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_ICONST) {
        emit_x86_mov_eax_imm32(text, (uint32_t)ins->v.iconst.imm);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_BCONST) {
        emit_x86_mov_eax_imm32(text, (uint32_t)ins->v.bconst.imm);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_PTR_NULL) {
        emit_x86_mov_eax_imm32(text, 0);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_ZEXT || ins->kind == IR_INSTR_BITCAST || ins->kind == IR_INSTR_PTRTOINT || ins->kind == IR_INSTR_INTTOPTR) {
        ir_x86_load_value_to_eax(f, text, ins->v.cast.src, value_disp, value_loc);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_SEXT) {
        IrValueId srcv = ins->v.cast.src;
        if (srcv != 0 && srcv > f->value_count) {
            scc_fatal_at(0, 0, 0, 0, "Internal error: invalid IR value id in x86 sext");
        }
        IrType* st = (srcv != 0) ? f->values[srcv - 1].type : 0;
        ir_x86_load_value_to_eax(f, text, srcv, value_disp, value_loc);

        if (st && st->kind == IR_TY_I16) {
            emit_x86_shl_eax_imm8(text, 16);
            emit_x86_sar_eax_imm8(text, 16);
        } else if (st && st->kind == IR_TY_I8) {
            emit_x86_shl_eax_imm8(text, 24);
            emit_x86_sar_eax_imm8(text, 24);
        }

        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_TRUNC) {
        ir_x86_load_value_to_eax(f, text, ins->v.cast.src, value_disp, value_loc);
        if (ins->type && ins->type->kind == IR_TY_BOOL) {
            emit_x86_and_eax_imm32(text, 1u);
        } else if (ins->type && (ins->type->kind == IR_TY_I16 || ins->type->kind == IR_TY_U16)) {
            emit_x86_and_eax_imm32(text, 0xFFFFu);
        } else if (ins->type && (ins->type->kind == IR_TY_I8 || ins->type->kind == IR_TY_U8)) {
            emit_x86_and_eax_imm32(text, 0xFFu);
        }
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_ALLOCA) {
        if (ins->result == 0) return;
        if (ins->result > f->value_count) {
            scc_fatal_at(0, 0, 0, 0, "Internal error: IR alloca result id out of range in x86 emission");
        }
        emit_x86_lea_eax_membp_disp(text, alloca_mem_disp[ins->result]);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_LOAD) {
        ir_x86_load_value_to_eax(f, text, ins->v.load.addr, value_disp, value_loc);
        if (ins->type && (ins->type->kind == IR_TY_I8 || ins->type->kind == IR_TY_U8 || ins->type->kind == IR_TY_BOOL)) {
            emit_x86_movzx_eax_memeax_u8(text);
        } else if (ins->type && (ins->type->kind == IR_TY_I16 || ins->type->kind == IR_TY_U16)) {
            emit_x86_movzx_eax_memeax_u16(text);
        } else {
            emit_x86_mov_eax_memeax_u32(text);
        }
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_STORE) {
        emit_x86_push_r32(text, X86_REG_ECX);
        ir_x86_load_value_to_eax(f, text, ins->v.store.value, value_disp, value_loc);
        emit_x86_push_eax(text);

        ir_x86_load_value_to_eax(f, text, ins->v.store.addr, value_disp, value_loc);
        emit_x86_mov_ecx_eax(text);

        emit_x86_pop_eax(text);
        IrValueId sv = ins->v.store.value;
        if (sv != 0 && sv > f->value_count) {
            scc_fatal_at(0, 0, 0, 0, "Internal error: invalid IR value id in x86 store (value)");
        }
        IrType* ty = (sv != 0) ? f->values[sv - 1].type : 0;
        if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_U8 || ty->kind == IR_TY_BOOL)) {
            emit_x86_mov_memecx_u8_al(text);
        } else if (ty && (ty->kind == IR_TY_I16 || ty->kind == IR_TY_U16)) {
            emit_x86_mov_memecx_u16_ax(text);
        } else {
            emit_x86_mov_memecx_u32_eax(text);
        }
        emit_x86_pop_r32(text, X86_REG_ECX);
        return;
    }

    if (ins->kind == IR_INSTR_PTR_ADD) {
        ir_x86_load_value_to_eax(f, text, ins->v.ptr_add.base, value_disp, value_loc);
        emit_x86_push_r32(text, X86_REG_ECX);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.ptr_add.offset_bytes, value_disp, value_loc);
        emit_x86_pop_ecx(text);
        emit_x86_add_eax_ecx(text);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        emit_x86_pop_r32(text, X86_REG_ECX);
        return;
    }
}

typedef struct {
    uint32_t words;

    uint32_t* use_bits;
    uint32_t* def_bits;
    uint32_t* live_in_bits;
    uint32_t* live_out_bits;

    uint32_t* value_def_pos;
    uint32_t* value_last_use_pos;
} IrX86Liveness;

static uint32_t ir_x86_lv_words(uint32_t bit_count) {
    return (bit_count + 31u) / 32u;
}

static uint32_t ir_x86_lv_test(const uint32_t* bits, uint32_t bit) {
    return (bits[bit >> 5] >> (bit & 31u)) & 1u;
}

static void ir_x86_lv_set(uint32_t* bits, uint32_t bit) {
    bits[bit >> 5] |= (1u << (bit & 31u));
}

static void ir_x86_lv_or(uint32_t* dst, const uint32_t* src, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) dst[i] |= src[i];
}

static void ir_x86_lv_copy(uint32_t* dst, const uint32_t* src, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) dst[i] = src[i];
}

static void ir_x86_lv_andnot(uint32_t* dst, const uint32_t* a, const uint32_t* b, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) dst[i] = a[i] & ~b[i];
}

static uint32_t ir_x86_lv_eq(const uint32_t* a, const uint32_t* b, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void ir_x86_lv_record_use(IrFunc* f, IrX86Liveness* lv, uint32_t* use_bits, const uint32_t* local_defs, IrValueId v, uint32_t pos) {
    if (!f || !lv) return;
    if (v == 0 || v > f->value_count) return;

    if (!ir_x86_lv_test(local_defs, v)) ir_x86_lv_set(use_bits, v);
    if (lv->value_last_use_pos[v] < pos) lv->value_last_use_pos[v] = pos;
}

static void ir_x86_lv_record_def(IrFunc* f, IrX86Liveness* lv, uint32_t* def_bits, uint32_t* local_defs, IrValueId v, uint32_t pos) {
    if (!f || !lv) return;
    if (v == 0 || v > f->value_count) return;

    ir_x86_lv_set(def_bits, v);
    ir_x86_lv_set(local_defs, v);
    lv->value_def_pos[v] = pos;
}

static void ir_x86_compute_liveness(IrFunc* f, IrX86Liveness* out_lv) {
    if (!out_lv) return;
    memset(out_lv, 0, sizeof(*out_lv));
    if (!f) return;

    IrX86Liveness lv;
    memset(&lv, 0, sizeof(lv));

    uint32_t bit_count = f->value_count + 1;
    lv.words = ir_x86_lv_words(bit_count);

    uint32_t bs_words = (f->block_count + 1) * lv.words;
    lv.use_bits = (uint32_t*)arena_alloc(f->arena, bs_words * sizeof(uint32_t), 4);
    lv.def_bits = (uint32_t*)arena_alloc(f->arena, bs_words * sizeof(uint32_t), 4);
    lv.live_in_bits = (uint32_t*)arena_alloc(f->arena, bs_words * sizeof(uint32_t), 4);
    lv.live_out_bits = (uint32_t*)arena_alloc(f->arena, bs_words * sizeof(uint32_t), 4);
    memset(lv.use_bits, 0, bs_words * sizeof(uint32_t));
    memset(lv.def_bits, 0, bs_words * sizeof(uint32_t));
    memset(lv.live_in_bits, 0, bs_words * sizeof(uint32_t));
    memset(lv.live_out_bits, 0, bs_words * sizeof(uint32_t));

    lv.value_def_pos = (uint32_t*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(uint32_t), 4);
    lv.value_last_use_pos = (uint32_t*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(uint32_t), 4);
    memset(lv.value_def_pos, 0, (f->value_count + 1) * sizeof(uint32_t));
    memset(lv.value_last_use_pos, 0, (f->value_count + 1) * sizeof(uint32_t));

    uint32_t* local_defs = (uint32_t*)arena_alloc(f->arena, lv.words * sizeof(uint32_t), 4);
    uint32_t* tmp = (uint32_t*)arena_alloc(f->arena, lv.words * sizeof(uint32_t), 4);

    uint32_t pos = 1;
    for (IrBlockId bid = 1; bid <= f->block_count; bid++) {
        IrBlock* b = &f->blocks[bid - 1];
        uint32_t* use_b = lv.use_bits + bid * lv.words;
        uint32_t* def_b = lv.def_bits + bid * lv.words;

        memset(use_b, 0, lv.words * sizeof(uint32_t));
        memset(def_b, 0, lv.words * sizeof(uint32_t));
        memset(local_defs, 0, lv.words * sizeof(uint32_t));

        for (uint32_t i = 0; i < b->param_count; i++) {
            IrValueId pv = b->params[i];
            ir_x86_lv_record_def(f, &lv, def_b, local_defs, pv, pos);
        }

        for (uint32_t i = 0; i < b->instr_count; i++) {
            IrInstrId iid = b->instrs[i];
            if (iid == 0 || iid > f->instr_count) continue;
            IrInstr* ins = &f->instrs[iid - 1];
            pos++;

            if (ins->kind == IR_INSTR_ZEXT || ins->kind == IR_INSTR_SEXT || ins->kind == IR_INSTR_TRUNC || ins->kind == IR_INSTR_BITCAST || ins->kind == IR_INSTR_PTRTOINT || ins->kind == IR_INSTR_INTTOPTR) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.cast.src, pos);
            } else if (ins->kind == IR_INSTR_ADD || ins->kind == IR_INSTR_SUB || ins->kind == IR_INSTR_MUL || ins->kind == IR_INSTR_SDIV || ins->kind == IR_INSTR_SREM || ins->kind == IR_INSTR_UDIV || ins->kind == IR_INSTR_UREM) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.bin.left, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.bin.right, pos);
            } else if (ins->kind == IR_INSTR_ICMP) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.icmp.left, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.icmp.right, pos);
            } else if (ins->kind == IR_INSTR_LOAD) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.load.addr, pos);
            } else if (ins->kind == IR_INSTR_STORE) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.store.addr, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.store.value, pos);
            } else if (ins->kind == IR_INSTR_PTR_ADD) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.ptr_add.base, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.ptr_add.offset_bytes, pos);
            } else if (ins->kind == IR_INSTR_CALL) {
                if (ins->v.call.arg_count && !ins->v.call.args) {
                    scc_fatal_at(0, 0, 0, 0, "Internal error: missing call args array in liveness");
                }
                for (uint32_t ai = 0; ai < ins->v.call.arg_count; ai++) {
                    IrValueId av = ins->v.call.args ? ins->v.call.args[ai] : 0;
                    if (av != 0 && av > f->value_count) {
                        scc_fatal_at(0, 0, 0, 0, "Internal error: invalid call arg value id in liveness");
                    }
                    ir_x86_lv_record_use(f, &lv, use_b, local_defs, av, pos);
                }
            } else if (ins->kind == IR_INSTR_SYSCALL) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.syscall.n, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.syscall.a1, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.syscall.a2, pos);
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, ins->v.syscall.a3, pos);
            }

            if (ins->result) {
                ir_x86_lv_record_def(f, &lv, def_b, local_defs, ins->result, pos);
            }
        }

        pos++;
        if (b->term.kind == IR_TERM_RET) {
            ir_x86_lv_record_use(f, &lv, use_b, local_defs, b->term.v.ret.value, pos);
        } else if (b->term.kind == IR_TERM_BR) {
            IrBranchTarget* dst = &b->term.v.br.dst;
            for (uint32_t ai = 0; ai < dst->arg_count; ai++) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, dst->args ? dst->args[ai] : 0, pos);
            }
        } else if (b->term.kind == IR_TERM_COND_BR) {
            IrBranchTarget* tdst = &b->term.v.condbr.tdst;
            IrBranchTarget* fdst = &b->term.v.condbr.fdst;
            ir_x86_lv_record_use(f, &lv, use_b, local_defs, b->term.v.condbr.cond, pos);
            for (uint32_t ai = 0; ai < tdst->arg_count; ai++) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, tdst->args ? tdst->args[ai] : 0, pos);
            }
            for (uint32_t ai = 0; ai < fdst->arg_count; ai++) {
                ir_x86_lv_record_use(f, &lv, use_b, local_defs, fdst->args ? fdst->args[ai] : 0, pos);
            }
        }
    }

    uint32_t changed = 1;
    while (changed) {
        changed = 0;

        for (IrBlockId bid = f->block_count; bid > 0; bid--) {
            IrBlock* b = &f->blocks[bid - 1];
            uint32_t* use_b = lv.use_bits + bid * lv.words;
            uint32_t* def_b = lv.def_bits + bid * lv.words;
            uint32_t* in_b = lv.live_in_bits + bid * lv.words;
            uint32_t* out_b = lv.live_out_bits + bid * lv.words;

            memset(tmp, 0, lv.words * sizeof(uint32_t));
            if (b->term.kind == IR_TERM_BR) {
                IrBlockId s = b->term.v.br.dst.target;
                if (s && s <= f->block_count) ir_x86_lv_or(tmp, lv.live_in_bits + s * lv.words, lv.words);
            } else if (b->term.kind == IR_TERM_COND_BR) {
                IrBlockId ts = b->term.v.condbr.tdst.target;
                IrBlockId fs = b->term.v.condbr.fdst.target;
                if (ts && ts <= f->block_count) ir_x86_lv_or(tmp, lv.live_in_bits + ts * lv.words, lv.words);
                if (fs && fs <= f->block_count) ir_x86_lv_or(tmp, lv.live_in_bits + fs * lv.words, lv.words);
            }

            if (!ir_x86_lv_eq(tmp, out_b, lv.words)) {
                ir_x86_lv_copy(out_b, tmp, lv.words);
                changed = 1;
            }

            ir_x86_lv_andnot(tmp, out_b, def_b, lv.words);
            ir_x86_lv_or(tmp, use_b, lv.words);
            if (!ir_x86_lv_eq(tmp, in_b, lv.words)) {
                ir_x86_lv_copy(in_b, tmp, lv.words);
                changed = 1;
            }
        }
    }

    *out_lv = lv;
}

typedef struct {
    IrValueId v;
    uint32_t start;
    uint32_t end;
    uint8_t crosses_call;
    uint8_t crosses_syscall;
} IrX86Interval;

static void ir_x86_sort_intervals_by_start(IrX86Interval* itv, uint32_t n) {
    if (!itv) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < n; j++) {
            if (itv[j].start < itv[best].start) best = j;
        }
        if (best != i) {
            IrX86Interval tmp_itv = itv[i];
            itv[i] = itv[best];
            itv[best] = tmp_itv;
        }
    }
}

static void ir_x86_build_intervals(IrFunc* f, IrX86Liveness* lv, IrX86Interval** out_itv, uint32_t* out_count) {
    if (!out_itv || !out_count) return;
    *out_itv = 0;
    *out_count = 0;
    if (!f || !lv) return;

    uint32_t* block_start = (uint32_t*)arena_alloc(f->arena, (f->block_count + 1) * sizeof(uint32_t), 4);
    uint32_t* block_end = (uint32_t*)arena_alloc(f->arena, (f->block_count + 1) * sizeof(uint32_t), 4);
    memset(block_start, 0, (f->block_count + 1) * sizeof(uint32_t));
    memset(block_end, 0, (f->block_count + 1) * sizeof(uint32_t));

    uint32_t* call_pos = (uint32_t*)arena_alloc(f->arena, (f->instr_count + 1) * sizeof(uint32_t), 4);
    uint32_t* syscall_pos = (uint32_t*)arena_alloc(f->arena, (f->instr_count + 1) * sizeof(uint32_t), 4);
    uint32_t call_n = 0;
    uint32_t syscall_n = 0;

    uint32_t pos = 1;
    for (IrBlockId bid = 1; bid <= f->block_count; bid++) {
        IrBlock* b = &f->blocks[bid - 1];
        block_start[bid] = pos;
        for (uint32_t i = 0; i < b->instr_count; i++) {
            IrInstrId iid = b->instrs[i];
            if (iid == 0 || iid > f->instr_count) continue;
            IrInstr* ins = &f->instrs[iid - 1];
            pos++;
            if (ins->kind == IR_INSTR_CALL) call_pos[call_n++] = pos;
            if (ins->kind == IR_INSTR_SYSCALL) syscall_pos[syscall_n++] = pos;
        }
        pos++;
        block_end[bid] = pos;
    }

    IrX86Interval* itv = (IrX86Interval*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(IrX86Interval), 8);
    uint32_t n = 0;

    for (IrValueId v = 1; v <= f->value_count; v++) {
        IrType* ty = f->values[v - 1].type;
        if (!ty || ty->kind == IR_TY_VOID) continue;

        uint32_t st = lv->value_def_pos[v];
        uint32_t en = lv->value_last_use_pos[v];
        if (st == 0 || en == 0 || en < st) continue;

        for (IrBlockId bid = 1; bid <= f->block_count; bid++) {
            uint32_t* in_b = lv->live_in_bits + bid * lv->words;
            uint32_t* out_b = lv->live_out_bits + bid * lv->words;

            if (ir_x86_lv_test(in_b, v)) {
                uint32_t bs = block_start[bid];
                if (bs && bs < st) st = bs;
            }

            if (ir_x86_lv_test(out_b, v)) {
                uint32_t be = block_end[bid];
                if (be && be > en) en = be;
            }
        }

        IrX86Interval iv;
        memset(&iv, 0, sizeof(iv));
        iv.v = v;
        iv.start = st;
        iv.end = en;

        for (uint32_t i = 0; i < syscall_n; i++) {
            uint32_t p = syscall_pos[i];
            if (p > st && p < en) {
                iv.crosses_syscall = 1;
                break;
            }
        }
        if (!iv.crosses_syscall) {
            for (uint32_t i = 0; i < call_n; i++) {
                uint32_t p = call_pos[i];
                if (p > st && p < en) {
                    iv.crosses_call = 1;
                    break;
                }
            }
        }

        itv[n++] = iv;
    }

    ir_x86_sort_intervals_by_start(itv, n);
    *out_itv = itv;
    *out_count = n;
}

static uint32_t ir_x86_reg_mask(X86Reg r) {
    return 1u << (uint32_t)r;
}

static uint32_t ir_x86_is_alloc_reg(X86Reg r) {
    return r != X86_REG_ESP && r != X86_REG_EBP;
}

static uint32_t ir_x86_is_callee_save_reg(X86Reg r) {
    return r == X86_REG_EBX || r == X86_REG_ESI || r == X86_REG_EDI;
}

static X86Reg ir_x86_pick_reg(uint32_t free_mask, uint32_t prefer_callee_save_only) {
    if (prefer_callee_save_only) {
        X86Reg order_cs[] = { X86_REG_EBX, X86_REG_ESI, X86_REG_EDI };
        for (uint32_t i = 0; i < sizeof(order_cs) / sizeof(order_cs[0]); i++) {
            X86Reg r = order_cs[i];
            if (free_mask & ir_x86_reg_mask(r)) return r;
        }
        return X86_REG_EAX;
    }

    X86Reg order[] = { X86_REG_ECX, X86_REG_EDX, X86_REG_EBX, X86_REG_ESI, X86_REG_EDI };
    for (uint32_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        X86Reg r = order[i];
        if (free_mask & ir_x86_reg_mask(r)) return r;
    }
    return X86_REG_EAX;
}

static void ir_x86_active_insert(IrX86Interval** active, uint32_t* inout_n, IrX86Interval* cur) {
    uint32_t n = *inout_n;
    uint32_t i = 0;
    for (; i < n; i++) {
        if (active[i]->end > cur->end) break;
    }
    for (uint32_t j = n; j > i; j--) active[j] = active[j - 1];
    active[i] = cur;
    *inout_n = n + 1;
}

static void ir_x86_active_remove(IrX86Interval** active, uint32_t* inout_n, uint32_t idx) {
    uint32_t n = *inout_n;
    if (idx >= n) return;
    for (uint32_t i = idx; i + 1 < n; i++) active[i] = active[i + 1];
    *inout_n = n - 1;
}

static void ir_x86_linear_scan_alloc(IrFunc* f, IrX86Interval* itv, uint32_t itv_count, IrX86Loc* value_loc, uint32_t* out_used_callee_mask) {
    if (!f || !value_loc || !out_used_callee_mask) return;
    *out_used_callee_mask = 0;

    for (IrValueId v = 0; v <= f->value_count; v++) {
        value_loc[v].kind = IR_X86_LOC_NONE;
        value_loc[v].reg = X86_REG_EAX;
        value_loc[v].disp = 0;
    }
    for (IrValueId v = 1; v <= f->value_count; v++) {
        IrType* ty = f->values[v - 1].type;
        if (!ty || ty->kind == IR_TY_VOID) continue;
        value_loc[v].kind = IR_X86_LOC_STACK;
    }

    uint32_t alloc_mask = ir_x86_reg_mask(X86_REG_ECX) | ir_x86_reg_mask(X86_REG_EDX) |
                         ir_x86_reg_mask(X86_REG_EBX) | ir_x86_reg_mask(X86_REG_ESI) | ir_x86_reg_mask(X86_REG_EDI);
    uint32_t callee_mask = ir_x86_reg_mask(X86_REG_EBX) | ir_x86_reg_mask(X86_REG_ESI) | ir_x86_reg_mask(X86_REG_EDI);

    uint32_t free_mask = alloc_mask;

    IrX86Interval** active = (IrX86Interval**)arena_alloc(f->arena, itv_count * sizeof(IrX86Interval*), 8);
    uint32_t active_n = 0;

    for (uint32_t i = 0; i < itv_count; i++) {
        IrX86Interval* cur = &itv[i];
        if (!cur || cur->v == 0 || cur->v > f->value_count) continue;
        if (cur->crosses_syscall) {
            value_loc[cur->v].kind = IR_X86_LOC_STACK;
            continue;
        }

        while (active_n > 0 && active[0]->end < cur->start) {
            IrX86Interval* expired = active[0];
            IrX86Loc loc = value_loc[expired->v];
            if (loc.kind == IR_X86_LOC_REG && ir_x86_is_alloc_reg(loc.reg)) {
                free_mask |= ir_x86_reg_mask(loc.reg);
            }
            ir_x86_active_remove(active, &active_n, 0);
        }

        uint32_t allowed_mask = alloc_mask;
        uint32_t need_callee = 0;
        if (cur->crosses_call) {
            allowed_mask = callee_mask;
            need_callee = 1;
        }

        uint32_t avail = free_mask & allowed_mask;
        if (avail) {
            X86Reg r = ir_x86_pick_reg(avail, need_callee);
            value_loc[cur->v].kind = IR_X86_LOC_REG;
            value_loc[cur->v].reg = r;
            free_mask &= ~ir_x86_reg_mask(r);
            if (ir_x86_is_callee_save_reg(r)) *out_used_callee_mask |= ir_x86_reg_mask(r);
            ir_x86_active_insert(active, &active_n, cur);
            continue;
        }

        int32_t spill_idx = -1;
        uint32_t spill_end = 0;
        for (uint32_t a = 0; a < active_n; a++) {
            IrX86Interval* av = active[a];
            IrX86Loc aloc = value_loc[av->v];
            if (aloc.kind != IR_X86_LOC_REG) continue;
            if (!(allowed_mask & ir_x86_reg_mask(aloc.reg))) continue;
            if (av->end >= spill_end) {
                spill_end = av->end;
                spill_idx = (int32_t)a;
            }
        }

        if (spill_idx >= 0) {
            IrX86Interval* victim = active[(uint32_t)spill_idx];
            if (victim->end > cur->end) {
                IrX86Loc vloc = value_loc[victim->v];
                value_loc[victim->v].kind = IR_X86_LOC_STACK;
                value_loc[cur->v] = vloc;
                if (vloc.kind == IR_X86_LOC_REG && ir_x86_is_callee_save_reg(vloc.reg)) {
                    *out_used_callee_mask |= ir_x86_reg_mask(vloc.reg);
                }
                ir_x86_active_remove(active, &active_n, (uint32_t)spill_idx);
                ir_x86_active_insert(active, &active_n, cur);
                continue;
            }
        }

        value_loc[cur->v].kind = IR_X86_LOC_STACK;
    }
}

static void ir_x86_emit_instr_arith(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind != IR_INSTR_ADD && ins->kind != IR_INSTR_SUB && ins->kind != IR_INSTR_MUL && ins->kind != IR_INSTR_SDIV && ins->kind != IR_INSTR_SREM && ins->kind != IR_INSTR_UDIV && ins->kind != IR_INSTR_UREM) {
        return;
    }

    emit_x86_push_r32(text, X86_REG_ECX);
    ir_x86_load_value_to_eax(f, text, ins->v.bin.left, value_disp, value_loc);
    emit_x86_push_eax(text);
    ir_x86_load_value_to_eax(f, text, ins->v.bin.right, value_disp, value_loc);
    emit_x86_pop_ecx(text);

    if (ins->kind == IR_INSTR_ADD) {
        emit_x86_add_eax_ecx(text);
    } else if (ins->kind == IR_INSTR_SUB) {
        emit_x86_sub_ecx_eax(text);
        emit_x86_mov_eax_ecx(text);
    } else if (ins->kind == IR_INSTR_MUL) {
        emit_x86_imul_eax_ecx(text);
    } else if (ins->kind == IR_INSTR_SDIV || ins->kind == IR_INSTR_SREM) {
        emit_x86_push_r32(text, X86_REG_EBX);
        emit_x86_push_r32(text, X86_REG_EDX);
        emit_x86_mov_ebx_eax(text);
        emit_x86_mov_eax_ecx(text);
        emit_x86_cdq(text);
        emit_x86_idiv_ebx(text);
        if (ins->kind == IR_INSTR_SREM) {
            emit_x86_mov_eax_edx(text);
        }
        emit_x86_pop_r32(text, X86_REG_EDX);
        emit_x86_pop_r32(text, X86_REG_EBX);
    } else {
        emit_x86_push_r32(text, X86_REG_EBX);
        emit_x86_push_r32(text, X86_REG_EDX);
        emit_x86_mov_ebx_eax(text);
        emit_x86_mov_eax_ecx(text);
        emit_x86_xor_edx_edx(text);
        emit_x86_div_ebx(text);
        if (ins->kind == IR_INSTR_UREM) {
            emit_x86_mov_eax_edx(text);
        }
        emit_x86_pop_r32(text, X86_REG_EDX);
        emit_x86_pop_r32(text, X86_REG_EBX);
    }

    ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
    emit_x86_pop_r32(text, X86_REG_ECX);
}

static void ir_x86_emit_instr_icmp(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind != IR_INSTR_ICMP) return;

    emit_x86_push_r32(text, X86_REG_ECX);
    ir_x86_load_value_to_eax(f, text, ins->v.icmp.left, value_disp, value_loc);
    emit_x86_push_eax(text);
    ir_x86_load_value_to_eax(f, text, ins->v.icmp.right, value_disp, value_loc);
    emit_x86_pop_ecx(text);

    emit_x86_cmp_ecx_eax(text);
    emit_x86_mov_eax_imm32(text, 0);
    emit_x86_setcc_al(text, ir_x86_icmp_cc(ins->v.icmp.pred));
    ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
    emit_x86_pop_r32(text, X86_REG_ECX);
}

static void ir_x86_emit_instr_misc(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp, IrX86Loc* value_loc) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind == IR_INSTR_GLOBAL_ADDR) {
        Symbol* s = ins->v.global_addr.sym;
        uint32_t off = text->size;
        emit_x86_mov_eax_imm32(text, 0);
        if (s) {
            ir_x86_emit_reloc_text(cx, off + 1u, s->elf_index, R_386_32);
        }
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_CALL) {
        Symbol* s = ins->v.call.callee;

        if (ins->v.call.arg_count && !ins->v.call.args) {
            scc_fatal_at(0, 0, 0, 0, "Internal error: missing call args array in x86 call emission");
        }

        for (int i = (int)ins->v.call.arg_count - 1; i >= 0; i--) {
            IrValueId av = ins->v.call.args ? ins->v.call.args[i] : 0;
            if (av != 0 && av > f->value_count) {
                scc_fatal_at(0, 0, 0, 0, "Internal error: invalid call arg value id in x86 call emission");
            }
            ir_x86_load_value_to_eax(f, text, av, value_disp, value_loc);
            emit_x86_push_eax(text);
        }

        uint32_t call_site = text->size;
        emit_x86_call_rel32(text, -4);
        if (s) ir_x86_emit_reloc_text(cx, call_site + 1u, s->elf_index, R_386_PC32);

        uint32_t stack_bytes = ins->v.call.arg_count * 4u;
        if (stack_bytes) emit_x86_add_esp_imm32(text, stack_bytes);

        if (ins->result) ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }

    if (ins->kind == IR_INSTR_SYSCALL) {
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.n, value_disp, value_loc);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a1, value_disp, value_loc);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a2, value_disp, value_loc);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a3, value_disp, value_loc);
        emit_x86_push_eax(text);

        emit_x86_pop_edx(text);
        emit_x86_pop_ecx(text);
        emit_x86_pop_ebx(text);
        emit_x86_pop_eax(text);
        emit_x86_int80(text);

        ir_x86_store_value_from_eax(f, text, ins->result, value_disp, value_loc);
        return;
    }
}

typedef struct {
    IrX86Fixup* items;
    uint32_t count;
    uint32_t cap;
} IrX86FixupList;

static void ir_x86_fixups_push(IrX86FixupList* l, uint32_t imm_off, IrBlockId target) {
    if (!l) return;
    uint32_t need = l->count + 1;
    if (need > l->cap) {
        l->items = (IrX86Fixup*)ir_grow_array(l->items, sizeof(IrX86Fixup), &l->cap, need);
    }
    l->items[l->count].imm_off = imm_off;
    l->items[l->count].target = target;
    l->count++;
}

static void ir_x86_emit_terminator(IrX86Ctx* cx, IrFunc* f, IrBlock* b, int32_t* value_disp, IrX86FixupList* fixups, uint32_t used_callee_mask, IrX86Loc* value_loc) {
    if (!cx || !f || !b || !value_disp || !fixups) return;
    Buffer* text = cx->text;

    if (b->term.kind == IR_TERM_RET) {
        if (f->ret_type && f->ret_type->kind != IR_TY_VOID) {
            ir_x86_load_value_to_eax(f, text, b->term.v.ret.value, value_disp, value_loc);
        }

        if (used_callee_mask & ir_x86_reg_mask(X86_REG_EDI)) emit_x86_pop_r32(text, X86_REG_EDI);
        if (used_callee_mask & ir_x86_reg_mask(X86_REG_ESI)) emit_x86_pop_r32(text, X86_REG_ESI);
        if (used_callee_mask & ir_x86_reg_mask(X86_REG_EBX)) emit_x86_pop_r32(text, X86_REG_EBX);

        emit_x86_epilogue(text);
        return;
    }

    if (b->term.kind == IR_TERM_BR) {
        IrBranchTarget* dst = &b->term.v.br.dst;
        ir_x86_emit_phi_moves(f, text, dst->target, dst->args, dst->arg_count, value_disp, value_loc);
        uint32_t imm_off = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off, dst->target);
        return;
    }

    if (b->term.kind == IR_TERM_COND_BR) {
        IrBranchTarget* tdst = &b->term.v.condbr.tdst;
        IrBranchTarget* fdst = &b->term.v.condbr.fdst;

        ir_x86_load_value_to_eax(f, text, b->term.v.condbr.cond, value_disp, value_loc);
        emit_x86_test_eax_eax(text);

        uint32_t imm_off_je = emit_x86_jcc_rel32_fixup(text, 0x4);

        ir_x86_emit_phi_moves(f, text, tdst->target, tdst->args, tdst->arg_count, value_disp, value_loc);
        uint32_t imm_off_tjmp = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off_tjmp, tdst->target);

        uint32_t false_stub_off = text->size;
        patch_rel32(text, imm_off_je, false_stub_off);

        ir_x86_emit_phi_moves(f, text, fdst->target, fdst->args, fdst->arg_count, value_disp, value_loc);
        uint32_t imm_off_fjmp = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off_fjmp, fdst->target);
        return;
    }
}

static uint32_t ir_x86_codegen_func(IrX86Ctx* cx, IrFunc* f) {
    if (!cx || !f || !cx->text) return 0;
    Buffer* text = cx->text;

    uint32_t func_start = text->size;

    IrX86Liveness lv;
    memset(&lv, 0, sizeof(lv));
    ir_x86_compute_liveness(f, &lv);

    IrX86Interval* itv = 0;
    uint32_t itv_count = 0;
    ir_x86_build_intervals(f, &lv, &itv, &itv_count);

    IrX86Loc* value_loc = (IrX86Loc*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(IrX86Loc), 8);
    uint32_t used_callee_mask = 0;
    ir_x86_linear_scan_alloc(f, itv, itv_count, value_loc, &used_callee_mask);

    int32_t* value_disp = (int32_t*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(int32_t), 8);
    memset(value_disp, 0, (f->value_count + 1) * sizeof(int32_t));

    int32_t* alloca_mem_disp = (int32_t*)arena_alloc(f->arena, (f->value_count + 1) * sizeof(int32_t), 8);
    memset(alloca_mem_disp, 0, (f->value_count + 1) * sizeof(int32_t));

    uint32_t* block_off = (uint32_t*)arena_alloc(f->arena, (f->block_count + 1) * sizeof(uint32_t), 4);
    memset(block_off, 0, (f->block_count + 1) * sizeof(uint32_t));

    uint32_t frame_size = 0;
    ir_x86_assign_frame(f, value_disp, alloca_mem_disp, &frame_size);

    emit_x86_prologue(text);
    if (frame_size) emit_x86_sub_esp_imm32(text, frame_size);

    if (used_callee_mask & ir_x86_reg_mask(X86_REG_EBX)) emit_x86_push_r32(text, X86_REG_EBX);
    if (used_callee_mask & ir_x86_reg_mask(X86_REG_ESI)) emit_x86_push_r32(text, X86_REG_ESI);
    if (used_callee_mask & ir_x86_reg_mask(X86_REG_EDI)) emit_x86_push_r32(text, X86_REG_EDI);

    if (f->entry && f->entry <= f->block_count) {
        IrBlock* entry = &f->blocks[f->entry - 1];
        for (uint32_t i = 0; i < entry->param_count; i++) {
            int32_t disp = (int32_t)(8 + i * 4);
            IrValueId pv = entry->params[i];
            if (pv == 0 || pv > f->value_count) {
                scc_fatal_at(0, 0, 0, 0, "Internal error: invalid IR value id in x86 prologue param load");
            }
            IrType* pty = f->values[pv - 1].type;
            if (pty && (pty->kind == IR_TY_I8 || pty->kind == IR_TY_U8 || pty->kind == IR_TY_BOOL)) emit_x86_movzx_eax_membp_disp(text, disp);
            else if (pty && (pty->kind == IR_TY_I16 || pty->kind == IR_TY_U16)) emit_x86_movzx_eax_membp_disp_u16(text, disp);
            else emit_x86_mov_eax_membp_disp(text, disp);
            ir_x86_store_value_from_eax(f, text, pv, value_disp, value_loc);
        }
    }

    IrX86FixupList fixups;
    memset(&fixups, 0, sizeof(fixups));

    for (IrBlockId bid = 1; bid <= f->block_count; bid++) {
        IrBlock* b = &f->blocks[bid - 1];
        block_off[bid] = text->size;

        for (uint32_t i = 0; i < b->instr_count; i++) {
            IrInstrId iid = b->instrs[i];
            if (iid == 0 || iid > f->instr_count) continue;
            IrInstr* ins = &f->instrs[iid - 1];

            ir_x86_emit_instr_simple(cx, f, ins, value_disp, alloca_mem_disp, value_loc);
            ir_x86_emit_instr_arith(cx, f, ins, value_disp, value_loc);
            ir_x86_emit_instr_icmp(cx, f, ins, value_disp, value_loc);
            ir_x86_emit_instr_misc(cx, f, ins, value_disp, value_loc);
        }

        ir_x86_emit_terminator(cx, f, b, value_disp, &fixups, used_callee_mask, value_loc);
    }

    for (uint32_t i = 0; i < fixups.count; i++) {
        IrX86Fixup* fx = &fixups.items[i];
        if (fx->target == 0 || fx->target > f->block_count) continue;
        patch_rel32(text, fx->imm_off, block_off[fx->target]);
    }

    if (fixups.items) free(fixups.items);
    return func_start;
}

static void ir_x86_codegen_module_stub(IrX86Ctx* cx, IrModule* m) {
    if (!cx || !m || !cx->text) return;

    for (uint32_t i = 0; i < m->func_count; i++) {
        IrFunc* f = &m->funcs[i];
        if (!f->sym) continue;
        if (f->sym->shndx == SHN_UNDEF) continue;

        while ((cx->text->size & 3u) != 0) buf_push_u8(cx->text, 0);

        uint32_t func_start = cx->text->size;
        f->sym->value = func_start;

        (void)ir_x86_codegen_func(cx, f);

        uint32_t func_size = cx->text->size - func_start;
        f->sym->size = func_size;
    }
}

#endif
