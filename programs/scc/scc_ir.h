// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_IR_H_INCLUDED
#define SCC_IR_H_INCLUDED

#include "scc_core.h"
#include "scc_buffer.h"

typedef uint32_t IrValueId;
typedef uint32_t IrInstrId;
typedef uint32_t IrBlockId;
typedef uint32_t IrFuncId;

typedef enum {
    IR_TY_VOID = 1,
    IR_TY_I32,
    IR_TY_U32,
    IR_TY_I16,
    IR_TY_U16,
    IR_TY_I8,
    IR_TY_U8,
    IR_TY_BOOL,
    IR_TY_PTR,
} IrTypeKind;

typedef struct IrType IrType;
struct IrType {
    IrTypeKind kind;
    IrType* base;
};

typedef enum {
    IR_ICMP_EQ = 1,
    IR_ICMP_NE,
    IR_ICMP_SLT,
    IR_ICMP_SLE,
    IR_ICMP_SGT,
    IR_ICMP_SGE,
    IR_ICMP_ULT,
    IR_ICMP_ULE,
    IR_ICMP_UGT,
    IR_ICMP_UGE,
} IrIcmpPred;

typedef enum {
    IR_INSTR_INVALID = 0,

    IR_INSTR_UNDEF,

    IR_INSTR_ICONST,
    IR_INSTR_BCONST,
    IR_INSTR_PTR_NULL,

    IR_INSTR_ZEXT,
    IR_INSTR_SEXT,
    IR_INSTR_TRUNC,
    IR_INSTR_BITCAST,

    IR_INSTR_PTRTOINT,
    IR_INSTR_INTTOPTR,

    IR_INSTR_ADD,
    IR_INSTR_SUB,
    IR_INSTR_MUL,
    IR_INSTR_SDIV,
    IR_INSTR_SREM,
    IR_INSTR_UDIV,
    IR_INSTR_UREM,

    IR_INSTR_AND,
    IR_INSTR_OR,
    IR_INSTR_XOR,

    IR_INSTR_SHL,
    IR_INSTR_SHR,
    IR_INSTR_SAR,

    IR_INSTR_ICMP,

    IR_INSTR_ALLOCA,
    IR_INSTR_LOAD,
    IR_INSTR_STORE,

    IR_INSTR_PTR_ADD,

    IR_INSTR_GLOBAL_ADDR,
    IR_INSTR_CALL,
    IR_INSTR_SYSCALL,
} IrInstrKind;

typedef enum {
    IR_TERM_INVALID = 0,
    IR_TERM_RET,
    IR_TERM_BR,
    IR_TERM_COND_BR,
} IrTermKind;

typedef struct {
    IrBlockId target;
    IrValueId* args;
    uint32_t arg_count;
} IrBranchTarget;

typedef struct {
    IrTermKind kind;
    union {
        struct {
            IrValueId value;
        } ret;
        struct {
            IrBranchTarget dst;
        } br;
        struct {
            IrValueId cond;
            IrBranchTarget tdst;
            IrBranchTarget fdst;
        } condbr;
    } v;
} IrTerminator;

typedef struct {
    IrInstrKind kind;
    IrType* type;
    IrValueId result;

    union {
        struct {
            int32_t imm;
        } iconst;
        struct {
            uint8_t imm;
        } bconst;
        struct {
            IrValueId src;
        } cast;
        struct {
            IrValueId left;
            IrValueId right;
        } bin;
        struct {
            IrIcmpPred pred;
            IrValueId left;
            IrValueId right;
        } icmp;
        struct {
            IrType* alloc_ty;
            uint32_t align;
        } alloca;
        struct {
            IrValueId addr;
        } load;
        struct {
            IrValueId addr;
            IrValueId value;
        } store;
        struct {
            IrValueId base;
            IrValueId offset_bytes;
        } ptr_add;
        struct {
            Symbol* sym;
        } global_addr;
        struct {
            Symbol* callee;
            IrValueId* args;
            uint32_t arg_count;
        } call;
        struct {
            IrValueId n;
            IrValueId a1;
            IrValueId a2;
            IrValueId a3;
        } syscall;
    } v;
} IrInstr;

typedef struct {
    IrValueId id;
    IrType* type;

    IrBlockId def_block;
    IrInstrId def_instr;

    uint32_t is_block_param;
} IrValue;

typedef struct {
    IrBlockId id;

    IrValueId* params;
    uint32_t param_count;

    IrInstrId* instrs;
    uint32_t instr_count;

    IrTerminator term;
} IrBlock;

typedef struct {
    IrFuncId id;
    Symbol* sym;

    IrType* ret_type;
    IrType** param_types;
    uint32_t param_count;

    IrBlockId entry;

    IrType* ty_void;
    IrType* ty_i32;
    IrType* ty_u32;
    IrType* ty_i16;
    IrType* ty_u16;
    IrType* ty_i8;
    IrType* ty_u8;
    IrType* ty_bool;

    IrType** ptr_types;
    uint32_t ptr_type_count;
    uint32_t ptr_type_cap;

    IrValue* values;
    uint32_t value_count;
    uint32_t value_cap;

    IrInstr* instrs;
    uint32_t instr_count;
    uint32_t instr_cap;

    IrBlock* blocks;
    uint32_t block_count;
    uint32_t block_cap;

    Arena* arena;
} IrFunc;

typedef struct {
    IrFunc* funcs;
    uint32_t func_count;
    uint32_t func_cap;

    Arena* arena;
} IrModule;

static void ir_module_init(IrModule* m, Arena* arena) {
    memset(m, 0, sizeof(*m));
    m->arena = arena;
}

static void* ir_grow_array(void* old, uint32_t elem_size, uint32_t* cap_inout, uint32_t need) {
    uint32_t cap = *cap_inout;
    if (cap == 0) cap = 16;
    while (cap < need) cap *= 2;

    void* nd = malloc((size_t)cap * (size_t)elem_size);
    if (!nd) exit(1);
    if (old) memcpy(nd, old, (size_t)(*cap_inout) * (size_t)elem_size);
    if (old) free(old);

    *cap_inout = cap;
    return nd;
}

static IrType* ir_type_new(Arena* a, IrTypeKind k, IrType* base) {
    IrType* t = (IrType*)arena_alloc(a, sizeof(IrType), 8);
    t->kind = k;
    t->base = base;
    return t;
}

static IrType* ir_type_ptr(IrFunc* f, IrType* base) {
    for (uint32_t i = 0; i < f->ptr_type_count; i++) {
        IrType* pt = f->ptr_types[i];
        if (pt && pt->kind == IR_TY_PTR && pt->base == base) return pt;
    }

    uint32_t need = f->ptr_type_count + 1;
    if (need > f->ptr_type_cap) {
        f->ptr_types = (IrType**)ir_grow_array(f->ptr_types, sizeof(IrType*), &f->ptr_type_cap, need);
    }

    IrType* pt = ir_type_new(f->arena, IR_TY_PTR, base);
    f->ptr_types[f->ptr_type_count++] = pt;
    return pt;
}

static IrFunc* ir_func_new(IrModule* m, Symbol* sym) {
    if (!m) return 0;

    uint32_t need = m->func_count + 1;
    if (need > m->func_cap) {
        m->funcs = (IrFunc*)ir_grow_array(m->funcs, sizeof(IrFunc), &m->func_cap, need);
    }

    IrFunc* f = &m->funcs[m->func_count++];
    memset(f, 0, sizeof(*f));

    f->id = m->func_count;
    f->sym = sym;
    f->arena = m->arena;

    f->ty_void = ir_type_new(f->arena, IR_TY_VOID, 0);
    f->ty_i32 = ir_type_new(f->arena, IR_TY_I32, 0);
    f->ty_u32 = ir_type_new(f->arena, IR_TY_U32, 0);
    f->ty_i16 = ir_type_new(f->arena, IR_TY_I16, 0);
    f->ty_u16 = ir_type_new(f->arena, IR_TY_U16, 0);
    f->ty_i8 = ir_type_new(f->arena, IR_TY_I8, 0);
    f->ty_u8 = ir_type_new(f->arena, IR_TY_U8, 0);
    f->ty_bool = ir_type_new(f->arena, IR_TY_BOOL, 0);

    return f;
}

static IrValueId ir_value_new(IrFunc* f, IrType* ty) {
    uint32_t need = f->value_count + 1;
    if (need > f->value_cap) {
        f->values = (IrValue*)ir_grow_array(f->values, sizeof(IrValue), &f->value_cap, need);
    }

    IrValueId id = f->value_count + 1;

    IrValue* v = &f->values[f->value_count++];
    memset(v, 0, sizeof(*v));
    v->id = id;
    v->type = ty;
    v->def_block = 0;
    v->def_instr = 0;
    v->is_block_param = 0;
    return id;
}

static IrInstrId ir_instr_new(IrFunc* f, IrInstrKind k, IrType* ty) {
    uint32_t need = f->instr_count + 1;
    if (need > f->instr_cap) {
        f->instrs = (IrInstr*)ir_grow_array(f->instrs, sizeof(IrInstr), &f->instr_cap, need);
    }

    IrInstrId id = f->instr_count + 1;

    IrInstr* ins = &f->instrs[f->instr_count++];
    memset(ins, 0, sizeof(*ins));
    ins->kind = k;
    ins->type = ty;
    ins->result = 0;
    return id;
}

static IrBlockId ir_block_new(IrFunc* f) {
    uint32_t need = f->block_count + 1;
    if (need > f->block_cap) {
        f->blocks = (IrBlock*)ir_grow_array(f->blocks, sizeof(IrBlock), &f->block_cap, need);
    }

    IrBlockId id = f->block_count + 1;

    IrBlock* b = &f->blocks[f->block_count++];
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->term.kind = IR_TERM_INVALID;
    return id;
}

static IrValueId ir_block_add_param(IrFunc* f, IrBlockId bid, IrType* ty) {
    if (bid == 0 || bid > f->block_count) return 0;
    IrBlock* b = &f->blocks[bid - 1];

    uint32_t need = b->param_count + 1;
    IrValueId* np = (IrValueId*)arena_alloc(f->arena, need * sizeof(IrValueId), 8);
    if (b->params) memcpy(np, b->params, b->param_count * sizeof(IrValueId));
    b->params = np;

    IrValueId v = ir_value_new(f, ty);
    b->params[b->param_count++] = v;

    f->values[v - 1].def_block = bid;
    f->values[v - 1].is_block_param = 1;
    return v;
}

static void ir_block_append_instr(IrFunc* f, IrBlockId bid, IrInstrId iid) {
    if (bid == 0 || bid > f->block_count) return;
    IrBlock* b = &f->blocks[bid - 1];

    uint32_t need = b->instr_count + 1;
    IrInstrId* ni = (IrInstrId*)arena_alloc(f->arena, need * sizeof(IrInstrId), 8);
    if (b->instrs) memcpy(ni, b->instrs, b->instr_count * sizeof(IrInstrId));
    b->instrs = ni;
    b->instrs[b->instr_count++] = iid;
}

static IrValueId ir_emit_iconst(IrFunc* f, IrBlockId b, int32_t imm) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_ICONST, f->ty_i32);
    IrValueId res = ir_value_new(f, f->ty_i32);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.iconst.imm = imm;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_uconst(IrFunc* f, IrBlockId b, uint32_t imm) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_ICONST, f->ty_u32);
    IrValueId res = ir_value_new(f, f->ty_u32);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.iconst.imm = (int32_t)imm;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_bitcast(IrFunc* f, IrBlockId b, IrType* dst_ty, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_BITCAST, dst_ty);
    IrValueId res = ir_value_new(f, dst_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_sext(IrFunc* f, IrBlockId b, IrType* dst_ty, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_SEXT, dst_ty);
    IrValueId res = ir_value_new(f, dst_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_ptrtoint(IrFunc* f, IrBlockId b, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_PTRTOINT, f->ty_i32);
    IrValueId res = ir_value_new(f, f->ty_i32);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_inttoptr(IrFunc* f, IrBlockId b, IrType* ptr_ty, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_INTTOPTR, ptr_ty);
    IrValueId res = ir_value_new(f, ptr_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_undef(IrFunc* f, IrBlockId b, IrType* ty) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_UNDEF, ty);
    IrValueId res = ir_value_new(f, ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_bconst(IrFunc* f, IrBlockId b, uint8_t imm01) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_BCONST, f->ty_bool);
    IrValueId res = ir_value_new(f, f->ty_bool);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.bconst.imm = (imm01 != 0) ? 1u : 0u;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_zext(IrFunc* f, IrBlockId b, IrType* dst_ty, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_ZEXT, dst_ty);
    IrValueId res = ir_value_new(f, dst_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_trunc(IrFunc* f, IrBlockId b, IrType* dst_ty, IrValueId src) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_TRUNC, dst_ty);
    IrValueId res = ir_value_new(f, dst_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.cast.src = src;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_bin(IrFunc* f, IrBlockId b, IrInstrKind k, IrType* ty, IrValueId left, IrValueId right) {
    IrInstrId ins = ir_instr_new(f, k, ty);
    IrValueId res = ir_value_new(f, ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.bin.left = left;
    i->v.bin.right = right;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_icmp(IrFunc* f, IrBlockId b, IrIcmpPred pred, IrValueId left, IrValueId right) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_ICMP, f->ty_bool);
    IrValueId res = ir_value_new(f, f->ty_bool);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.icmp.pred = pred;
    i->v.icmp.left = left;
    i->v.icmp.right = right;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_ptr_null(IrFunc* f, IrBlockId b, IrType* ptr_ty) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_PTR_NULL, ptr_ty);
    IrValueId res = ir_value_new(f, ptr_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_alloca(IrFunc* f, IrBlockId b, IrType* alloc_ty, uint32_t align) {
    IrType* res_ty = ir_type_ptr(f, alloc_ty);
    IrInstrId ins = ir_instr_new(f, IR_INSTR_ALLOCA, res_ty);
    IrValueId res = ir_value_new(f, res_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.alloca.alloc_ty = alloc_ty;
    i->v.alloca.align = align;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_load(IrFunc* f, IrBlockId b, IrType* load_ty, IrValueId addr) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_LOAD, load_ty);
    IrValueId res = ir_value_new(f, load_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.load.addr = addr;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static void ir_emit_store(IrFunc* f, IrBlockId b, IrValueId addr, IrValueId value) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_STORE, f->ty_void);
    IrInstr* i = &f->instrs[ins - 1];
    i->result = 0;
    i->v.store.addr = addr;
    i->v.store.value = value;
    ir_block_append_instr(f, b, ins);
}

static IrValueId ir_emit_ptr_add(IrFunc* f, IrBlockId b, IrType* ptr_ty, IrValueId base, IrValueId offset_bytes) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_PTR_ADD, ptr_ty);
    IrValueId res = ir_value_new(f, ptr_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.ptr_add.base = base;
    i->v.ptr_add.offset_bytes = offset_bytes;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_global_addr(IrFunc* f, IrBlockId b, IrType* ptr_ty, Symbol* sym) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_GLOBAL_ADDR, ptr_ty);
    IrValueId res = ir_value_new(f, ptr_ty);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.global_addr.sym = sym;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_call(IrFunc* f, IrBlockId b, IrType* ret_ty, Symbol* callee, IrValueId* args, uint32_t arg_count) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_CALL, ret_ty);
    IrInstr* i = &f->instrs[ins - 1];

    i->v.call.callee = callee;
    i->v.call.arg_count = arg_count;
    if (arg_count) {
        IrValueId* a = (IrValueId*)arena_alloc(f->arena, arg_count * sizeof(IrValueId), 8);
        memcpy(a, args, arg_count * sizeof(IrValueId));
        i->v.call.args = a;
    } else {
        i->v.call.args = 0;
    }

    if (ret_ty && ret_ty->kind == IR_TY_VOID) {
        i->result = 0;
        ir_block_append_instr(f, b, ins);
        return 0;
    }

    IrValueId res = ir_value_new(f, ret_ty);
    i->result = res;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static IrValueId ir_emit_syscall(IrFunc* f, IrBlockId b, IrValueId n, IrValueId a1, IrValueId a2, IrValueId a3) {
    IrInstrId ins = ir_instr_new(f, IR_INSTR_SYSCALL, f->ty_i32);
    IrValueId res = ir_value_new(f, f->ty_i32);

    IrInstr* i = &f->instrs[ins - 1];
    i->result = res;
    i->v.syscall.n = n;
    i->v.syscall.a1 = a1;
    i->v.syscall.a2 = a2;
    i->v.syscall.a3 = a3;

    f->values[res - 1].def_block = b;
    f->values[res - 1].def_instr = ins;

    ir_block_append_instr(f, b, ins);
    return res;
}

static void ir_set_term_ret(IrFunc* f, IrBlockId b, IrValueId v) {
    if (b == 0 || b > f->block_count) return;
    IrBlock* blk = &f->blocks[b - 1];
    blk->term.kind = IR_TERM_RET;
    blk->term.v.ret.value = v;
}

static void ir_set_term_br(IrFunc* f, IrBlockId b, IrBlockId dst, IrValueId* args, uint32_t arg_count) {
    if (b == 0 || b > f->block_count) return;
    IrBlock* blk = &f->blocks[b - 1];

    blk->term.kind = IR_TERM_BR;
    blk->term.v.br.dst.target = dst;
    blk->term.v.br.dst.arg_count = arg_count;

    if (arg_count) {
        IrValueId* a = (IrValueId*)arena_alloc(f->arena, arg_count * sizeof(IrValueId), 8);
        memcpy(a, args, arg_count * sizeof(IrValueId));
        blk->term.v.br.dst.args = a;
    } else {
        blk->term.v.br.dst.args = 0;
    }
}

static void ir_set_term_condbr(IrFunc* f, IrBlockId b, IrValueId cond, IrBlockId tdst, IrValueId* targs, uint32_t targc, IrBlockId fdst, IrValueId* fargs, uint32_t fargc) {
    if (b == 0 || b > f->block_count) return;
    IrBlock* blk = &f->blocks[b - 1];

    blk->term.kind = IR_TERM_COND_BR;
    blk->term.v.condbr.cond = cond;

    blk->term.v.condbr.tdst.target = tdst;
    blk->term.v.condbr.tdst.arg_count = targc;
    if (targc) {
        IrValueId* a = (IrValueId*)arena_alloc(f->arena, targc * sizeof(IrValueId), 8);
        memcpy(a, targs, targc * sizeof(IrValueId));
        blk->term.v.condbr.tdst.args = a;
    } else {
        blk->term.v.condbr.tdst.args = 0;
    }

    blk->term.v.condbr.fdst.target = fdst;
    blk->term.v.condbr.fdst.arg_count = fargc;
    if (fargc) {
        IrValueId* a = (IrValueId*)arena_alloc(f->arena, fargc * sizeof(IrValueId), 8);
        memcpy(a, fargs, fargc * sizeof(IrValueId));
        blk->term.v.condbr.fdst.args = a;
    } else {
        blk->term.v.condbr.fdst.args = 0;
    }
}

static uint32_t ir_type_size(IrType* t) {
    if (!t) return 4;
    if (t->kind == IR_TY_BOOL) return 1;
    if (t->kind == IR_TY_I8) return 1;
    if (t->kind == IR_TY_U8) return 1;
    if (t->kind == IR_TY_I16) return 2;
    if (t->kind == IR_TY_U16) return 2;
    if (t->kind == IR_TY_I32) return 4;
    if (t->kind == IR_TY_U32) return 4;
    if (t->kind == IR_TY_PTR) return 4;
    return 0;
}

static void ir_print_type(Buffer* out, IrType* t) {
    if (!t) {
        buf_add_cstr(out, "<null>");
        return;
    }

    if (t->kind == IR_TY_VOID) { buf_add_cstr(out, "void"); return; }
    if (t->kind == IR_TY_I32) { buf_add_cstr(out, "i32"); return; }
    if (t->kind == IR_TY_U32) { buf_add_cstr(out, "u32"); return; }
    if (t->kind == IR_TY_I16) { buf_add_cstr(out, "i16"); return; }
    if (t->kind == IR_TY_U16) { buf_add_cstr(out, "u16"); return; }
    if (t->kind == IR_TY_I8) { buf_add_cstr(out, "i8"); return; }
    if (t->kind == IR_TY_U8) { buf_add_cstr(out, "u8"); return; }
    if (t->kind == IR_TY_BOOL) { buf_add_cstr(out, "bool"); return; }
    if (t->kind == IR_TY_PTR) {
        buf_add_cstr(out, "ptr(");
        ir_print_type(out, t->base);
        buf_add_cstr(out, ")");
        return;
    }

    buf_add_cstr(out, "<ty>");
}

#endif
