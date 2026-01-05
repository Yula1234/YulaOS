#ifndef SCC_IR_X86_H_INCLUDED
#define SCC_IR_X86_H_INCLUDED

#include "scc_ir.h"
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

static void ir_x86_emit_reloc_text(IrX86Ctx* cx, uint32_t offset, int sym_index, int type) {
    Elf32_Rel r;
    r.r_offset = (Elf32_Addr)offset;
    r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
    buf_write(cx->rel_text, &r, sizeof(r));
}

static void ir_x86_store_eax_to_slot(Buffer* text, IrType* ty, int32_t disp) {
    if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_BOOL)) {
        emit_x86_mov_membp_disp_al(text, disp);
        return;
    }
    emit_x86_mov_membp_disp_eax(text, disp);
}

static void ir_x86_load_slot_to_eax(Buffer* text, IrType* ty, int32_t disp) {
    if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_BOOL)) {
        emit_x86_movzx_eax_membp_disp(text, disp);
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
    return 0x4;
}

static void ir_x86_load_value_to_eax(IrFunc* f, Buffer* text, IrValueId v, int32_t* value_disp) {
    if (!f || !text) return;
    if (v == 0) {
        emit_x86_mov_eax_imm32(text, 0);
        return;
    }
    IrType* ty = f->values[v - 1].type;
    ir_x86_load_slot_to_eax(text, ty, value_disp[v]);
}

static void ir_x86_store_value_from_eax(IrFunc* f, Buffer* text, IrValueId v, int32_t* value_disp) {
    if (!f || !text) return;
    if (v == 0) return;
    IrType* ty = f->values[v - 1].type;
    ir_x86_store_eax_to_slot(text, ty, value_disp[v]);
}

static void ir_x86_emit_phi_moves(IrFunc* f, Buffer* text, IrBlockId target, IrValueId* args, uint32_t arg_count, int32_t* value_disp) {
    if (!f || !text || !value_disp) return;
    if (target == 0 || target > f->block_count) return;

    IrBlock* b = &f->blocks[target - 1];
    uint32_t n = arg_count;
    if (b->param_count < n) n = b->param_count;

    for (uint32_t i = 0; i < n; i++) {
        ir_x86_load_value_to_eax(f, text, args ? args[i] : 0, value_disp);
        emit_x86_push_eax(text);
    }
    for (uint32_t i = n; i > 0; i--) {
        emit_x86_pop_eax(text);
        ir_x86_store_value_from_eax(f, text, b->params[i - 1], value_disp);
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

static void ir_x86_emit_instr_simple(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp, int32_t* alloca_mem_disp) {
    if (!cx || !f || !ins || !value_disp || !alloca_mem_disp) return;
    Buffer* text = cx->text;

    if (ins->kind == IR_INSTR_UNDEF) {
        if (ins->result == 0) return;
        emit_x86_mov_eax_imm32(text, 0);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_ICONST) {
        emit_x86_mov_eax_imm32(text, (uint32_t)ins->v.iconst.imm);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_BCONST) {
        emit_x86_mov_eax_imm32(text, (uint32_t)ins->v.bconst.imm);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_PTR_NULL) {
        emit_x86_mov_eax_imm32(text, 0);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_ZEXT || ins->kind == IR_INSTR_PTRTOINT || ins->kind == IR_INSTR_INTTOPTR) {
        ir_x86_load_value_to_eax(f, text, ins->v.cast.src, value_disp);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_TRUNC) {
        ir_x86_load_value_to_eax(f, text, ins->v.cast.src, value_disp);
        if (ins->type && ins->type->kind == IR_TY_BOOL) {
            emit_x86_and_eax_imm32(text, 1u);
        } else if (ins->type && ins->type->kind == IR_TY_I8) {
            emit_x86_and_eax_imm32(text, 0xFFu);
        }
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_ALLOCA) {
        if (ins->result == 0) return;
        emit_x86_lea_eax_membp_disp(text, alloca_mem_disp[ins->result]);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_LOAD) {
        ir_x86_load_value_to_eax(f, text, ins->v.load.addr, value_disp);
        if (ins->type && (ins->type->kind == IR_TY_I8 || ins->type->kind == IR_TY_BOOL)) {
            emit_x86_movzx_eax_memeax_u8(text);
        } else {
            emit_x86_mov_eax_memeax_u32(text);
        }
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_STORE) {
        ir_x86_load_value_to_eax(f, text, ins->v.store.addr, value_disp);
        emit_x86_mov_ecx_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.store.value, value_disp);
        IrType* ty = (ins->v.store.value != 0) ? f->values[ins->v.store.value - 1].type : 0;
        if (ty && (ty->kind == IR_TY_I8 || ty->kind == IR_TY_BOOL)) {
            emit_x86_mov_memecx_u8_al(text);
        } else {
            emit_x86_mov_memecx_u32_eax(text);
        }
        return;
    }

    if (ins->kind == IR_INSTR_PTR_ADD) {
        ir_x86_load_value_to_eax(f, text, ins->v.ptr_add.base, value_disp);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.ptr_add.offset_bytes, value_disp);
        emit_x86_pop_ecx(text);
        emit_x86_add_eax_ecx(text);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }
}

static void ir_x86_emit_instr_arith(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind != IR_INSTR_ADD && ins->kind != IR_INSTR_SUB && ins->kind != IR_INSTR_MUL && ins->kind != IR_INSTR_SDIV && ins->kind != IR_INSTR_SREM) {
        return;
    }

    ir_x86_load_value_to_eax(f, text, ins->v.bin.left, value_disp);
    emit_x86_push_eax(text);
    ir_x86_load_value_to_eax(f, text, ins->v.bin.right, value_disp);
    emit_x86_pop_ecx(text);

    if (ins->kind == IR_INSTR_ADD) {
        emit_x86_add_eax_ecx(text);
    } else if (ins->kind == IR_INSTR_SUB) {
        emit_x86_sub_ecx_eax(text);
        emit_x86_mov_eax_ecx(text);
    } else if (ins->kind == IR_INSTR_MUL) {
        emit_x86_imul_eax_ecx(text);
    } else {
        emit_x86_mov_ebx_eax(text);
        emit_x86_mov_eax_ecx(text);
        emit_x86_cdq(text);
        emit_x86_idiv_ebx(text);
        if (ins->kind == IR_INSTR_SREM) {
            emit_x86_mov_eax_edx(text);
        }
    }

    ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
}

static void ir_x86_emit_instr_icmp(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind != IR_INSTR_ICMP) return;

    ir_x86_load_value_to_eax(f, text, ins->v.icmp.left, value_disp);
    emit_x86_push_eax(text);
    ir_x86_load_value_to_eax(f, text, ins->v.icmp.right, value_disp);
    emit_x86_pop_ecx(text);

    emit_x86_cmp_ecx_eax(text);
    emit_x86_mov_eax_imm32(text, 0);
    emit_x86_setcc_al(text, ir_x86_icmp_cc(ins->v.icmp.pred));
    ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
}

static void ir_x86_emit_instr_misc(IrX86Ctx* cx, IrFunc* f, IrInstr* ins, int32_t* value_disp) {
    if (!cx || !f || !ins || !value_disp) return;
    Buffer* text = cx->text;

    if (ins->kind == IR_INSTR_GLOBAL_ADDR) {
        Symbol* s = ins->v.global_addr.sym;
        uint32_t off = text->size;
        emit_x86_mov_eax_imm32(text, 0);
        if (s) ir_x86_emit_reloc_text(cx, off + 1u, s->elf_index, R_386_32);
        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_CALL) {
        Symbol* s = ins->v.call.callee;

        for (int i = (int)ins->v.call.arg_count - 1; i >= 0; i--) {
            ir_x86_load_value_to_eax(f, text, ins->v.call.args ? ins->v.call.args[i] : 0, value_disp);
            emit_x86_push_eax(text);
        }

        uint32_t call_site = text->size;
        emit_x86_call_rel32(text, -4);
        if (s) ir_x86_emit_reloc_text(cx, call_site + 1u, s->elf_index, R_386_PC32);

        uint32_t stack_bytes = ins->v.call.arg_count * 4u;
        if (stack_bytes) emit_x86_add_esp_imm32(text, stack_bytes);

        if (ins->result) ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
        return;
    }

    if (ins->kind == IR_INSTR_SYSCALL) {
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.n, value_disp);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a1, value_disp);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a2, value_disp);
        emit_x86_push_eax(text);
        ir_x86_load_value_to_eax(f, text, ins->v.syscall.a3, value_disp);
        emit_x86_push_eax(text);

        emit_x86_pop_edx(text);
        emit_x86_pop_ecx(text);
        emit_x86_pop_ebx(text);
        emit_x86_pop_eax(text);
        emit_x86_int80(text);

        ir_x86_store_value_from_eax(f, text, ins->result, value_disp);
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

static void ir_x86_emit_terminator(IrX86Ctx* cx, IrFunc* f, IrBlock* b, int32_t* value_disp, IrX86FixupList* fixups) {
    if (!cx || !f || !b || !value_disp || !fixups) return;
    Buffer* text = cx->text;

    if (b->term.kind == IR_TERM_RET) {
        if (f->ret_type && f->ret_type->kind != IR_TY_VOID) {
            ir_x86_load_value_to_eax(f, text, b->term.v.ret.value, value_disp);
        }
        emit_x86_epilogue(text);
        return;
    }

    if (b->term.kind == IR_TERM_BR) {
        IrBranchTarget* dst = &b->term.v.br.dst;
        ir_x86_emit_phi_moves(f, text, dst->target, dst->args, dst->arg_count, value_disp);
        uint32_t imm_off = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off, dst->target);
        return;
    }

    if (b->term.kind == IR_TERM_COND_BR) {
        IrBranchTarget* tdst = &b->term.v.condbr.tdst;
        IrBranchTarget* fdst = &b->term.v.condbr.fdst;

        ir_x86_load_value_to_eax(f, text, b->term.v.condbr.cond, value_disp);
        emit_x86_test_eax_eax(text);

        uint32_t imm_off_je = emit_x86_jcc_rel32_fixup(text, 0x4);

        ir_x86_emit_phi_moves(f, text, tdst->target, tdst->args, tdst->arg_count, value_disp);
        uint32_t imm_off_tjmp = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off_tjmp, tdst->target);

        uint32_t false_stub_off = text->size;
        patch_rel32(text, imm_off_je, false_stub_off);

        ir_x86_emit_phi_moves(f, text, fdst->target, fdst->args, fdst->arg_count, value_disp);
        uint32_t imm_off_fjmp = emit_x86_jmp_rel32_fixup(text);
        ir_x86_fixups_push(fixups, imm_off_fjmp, fdst->target);
        return;
    }
}

static uint32_t ir_x86_codegen_func(IrX86Ctx* cx, IrFunc* f) {
    if (!cx || !f || !cx->text) return 0;
    Buffer* text = cx->text;

    uint32_t func_start = text->size;

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

    if (f->entry && f->entry <= f->block_count) {
        IrBlock* entry = &f->blocks[f->entry - 1];
        for (uint32_t i = 0; i < entry->param_count; i++) {
            int32_t disp = (int32_t)(8 + i * 4);
            emit_x86_mov_eax_membp_disp(text, disp);
            ir_x86_store_value_from_eax(f, text, entry->params[i], value_disp);
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

            ir_x86_emit_instr_simple(cx, f, ins, value_disp, alloca_mem_disp);
            ir_x86_emit_instr_arith(cx, f, ins, value_disp);
            ir_x86_emit_instr_icmp(cx, f, ins, value_disp);
            ir_x86_emit_instr_misc(cx, f, ins, value_disp);
        }

        ir_x86_emit_terminator(cx, f, b, value_disp, &fixups);
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
