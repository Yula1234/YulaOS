#ifndef SCC_CODEGEN_H_INCLUDED
#define SCC_CODEGEN_H_INCLUDED

#include "scc_parser_base.h"
#include "scc_x86.h"

 typedef struct {
     uint32_t start_off;
     uint32_t break_fixups[64];
     int break_count;
 } LoopCtx;

 typedef struct {
     Buffer* text;
     Buffer* rel_text;
     SymTable* syms;
     Parser* p;

     Buffer* data;
     Buffer* rel_data;

     Var* vars;
     uint32_t str_id;

     LoopCtx loops[16];
     int loop_depth;
 } Codegen;

 static void gen_expr(Codegen* cg, AstExpr* e);

 static void emit_reloc_text(Codegen* cg, uint32_t offset, int sym_index, int type) {
     Elf32_Rel r;
     r.r_offset = (Elf32_Addr)offset;
     r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
     buf_write(cg->rel_text, &r, sizeof(r));
 }

 static void emit_reloc_data(Codegen* cg, uint32_t offset, int sym_index, int type) {
     Elf32_Rel r;
     r.r_offset = (Elf32_Addr)offset;
     r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
     buf_write(cg->rel_data, &r, sizeof(r));
 }

 static Var* cg_find_var(Codegen* cg, const char* name) {
     for (Var* v = cg->vars; v; v = v->next) {
         if (strcmp(v->name, name) == 0) return v;
     }
     return 0;
 }

 static void u32_to_dec(char* out, uint32_t v) {
     char tmp[16];
     int n = 0;
     if (v == 0) {
         out[0] = '0';
         out[1] = 0;
         return;
     }
     while (v) {
         tmp[n++] = (char)('0' + (v % 10));
         v /= 10;
     }
     for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
     out[n] = 0;
 }

static Symbol* cg_intern_string(Codegen* cg, const char* bytes, int len) {
    char namebuf[32];
    char dec[16];
    u32_to_dec(dec, cg->str_id++);

    int n = 0;
    namebuf[n++] = '.';
    namebuf[n++] = 'L';
    namebuf[n++] = 's';
    namebuf[n++] = 't';
    namebuf[n++] = 'r';
    for (int i = 0; dec[i]; i++) namebuf[n++] = dec[i];
    namebuf[n] = 0;

    uint32_t off = cg->data->size;
    if (len) buf_write(cg->data, bytes, (uint32_t)len);
    buf_push_u8(cg->data, 0);

    return symtab_add_local_data(cg->syms, cg->p->arena, namebuf, off, (uint32_t)len + 1u);
}

static void cg_eval_const_u32(Codegen* cg, AstExpr* e, uint32_t* out_val, Symbol** out_reloc_sym) {
    *out_val = 0;
    *out_reloc_sym = 0;

    if (!e) return;

    if (e->kind == AST_EXPR_INT_LIT) {
        *out_val = (uint32_t)e->v.int_lit;
        return;
    }

    if (e->kind == AST_EXPR_STR) {
        Symbol* s = cg_intern_string(cg, e->v.str.bytes, e->v.str.len);
        *out_val = 0;
        *out_reloc_sym = s;
        return;
    }

    if (e->kind == AST_EXPR_CAST) {
        uint32_t v = 0;
        Symbol* rs = 0;
        cg_eval_const_u32(cg, e->v.cast.expr, &v, &rs);

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_VOID) {
            *out_val = 0;
            *out_reloc_sym = 0;
            return;
        }

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_CHAR) {
            if (rs) {
                scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Cannot cast relocatable address to char in global initializer");
            }
            v &= 0xFFu;
        }

        *out_val = v;
        *out_reloc_sym = rs;
        return;
    }

    if (e->kind == AST_EXPR_UNARY) {
        uint32_t v = 0;
        Symbol* rs = 0;
        cg_eval_const_u32(cg, e->v.unary.expr, &v, &rs);
        if (rs) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in unary global initializer");
        }

        int32_t sv = (int32_t)v;
        if (e->v.unary.op == AST_UNOP_NEG) sv = -sv;
        else if (e->v.unary.op == AST_UNOP_NOT) sv = (sv == 0) ? 1 : 0;
        *out_val = (uint32_t)sv;
        *out_reloc_sym = 0;
        return;
    }

    if (e->kind == AST_EXPR_BINARY) {
        uint32_t lv = 0;
        uint32_t rv = 0;
        Symbol* ls = 0;
        Symbol* rs = 0;
        cg_eval_const_u32(cg, e->v.binary.left, &lv, &ls);
        cg_eval_const_u32(cg, e->v.binary.right, &rv, &rs);
        if (ls || rs) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in binary global initializer");
        }

        if (e->v.binary.op == AST_BINOP_ADD) { *out_val = lv + rv; return; }
        if (e->v.binary.op == AST_BINOP_SUB) { *out_val = lv - rv; return; }
        if (e->v.binary.op == AST_BINOP_MUL) { *out_val = lv * rv; return; }
        if (e->v.binary.op == AST_BINOP_DIV) {
            if (rv == 0) scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Division by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv / (int32_t)rv);
            return;
        }
        if (e->v.binary.op == AST_BINOP_MOD) {
            if (rv == 0) scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Modulo by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv % (int32_t)rv);
            return;
        }

        scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Only arithmetic operators are supported in global initializers");
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Non-constant global initializer");
}

static void gen_expr_binary_arith(Codegen* cg, AstExpr* e) {
    gen_expr(cg, e->v.binary.left);
    emit_x86_push_eax(cg->text);
    gen_expr(cg, e->v.binary.right);
    emit_x86_pop_ecx(cg->text);

    if (e->v.binary.op == AST_BINOP_ADD) {
        emit_x86_add_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_SUB) {
        emit_x86_sub_ecx_eax(cg->text);
        emit_x86_mov_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_MUL) {
        emit_x86_imul_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
        emit_x86_mov_ebx_eax(cg->text);
        emit_x86_mov_eax_ecx(cg->text);
        emit_x86_cdq(cg->text);
        emit_x86_idiv_ebx(cg->text);
        if (e->v.binary.op == AST_BINOP_MOD) {
            emit_x86_mov_eax_edx(cg->text);
        }
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Binary operator not supported in codegen yet");
}

static void gen_expr_binary_cmp(Codegen* cg, AstExpr* e) {
    gen_expr(cg, e->v.binary.left);
    emit_x86_push_eax(cg->text);
    gen_expr(cg, e->v.binary.right);
    emit_x86_pop_ecx(cg->text);

    emit_x86_cmp_ecx_eax(cg->text);
    emit_x86_mov_eax_imm32(cg->text, 0);

    if (e->v.binary.op == AST_BINOP_EQ) {
        emit_x86_setcc_al(cg->text, 0x4);
        return;
    }
    if (e->v.binary.op == AST_BINOP_NE) {
        emit_x86_setcc_al(cg->text, 0x5);
        return;
    }
    if (e->v.binary.op == AST_BINOP_LT) {
        emit_x86_setcc_al(cg->text, 0xC);
        return;
    }
    if (e->v.binary.op == AST_BINOP_LE) {
        emit_x86_setcc_al(cg->text, 0xE);
        return;
    }
    if (e->v.binary.op == AST_BINOP_GT) {
        emit_x86_setcc_al(cg->text, 0xF);
        return;
    }
    if (e->v.binary.op == AST_BINOP_GE) {
        emit_x86_setcc_al(cg->text, 0xD);
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Comparison operator not supported in codegen yet");
}

static void gen_expr_binary_logical(Codegen* cg, AstExpr* e) {
    if (e->v.binary.op == AST_BINOP_ANDAND) {
        gen_expr(cg, e->v.binary.left);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jz_false = emit_x86_jcc_rel32_fixup(cg->text, 0x4);

        gen_expr(cg, e->v.binary.right);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jz_false2 = emit_x86_jcc_rel32_fixup(cg->text, 0x4);

        emit_x86_mov_eax_imm32(cg->text, 1);
        uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);

        uint32_t false_off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 0);
        uint32_t end_off = cg->text->size;

        patch_rel32(cg->text, jz_false, false_off);
        patch_rel32(cg->text, jz_false2, false_off);
        patch_rel32(cg->text, jmp_end, end_off);
        return;
    }

    if (e->v.binary.op == AST_BINOP_OROR) {
        gen_expr(cg, e->v.binary.left);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jnz_true = emit_x86_jcc_rel32_fixup(cg->text, 0x5);

        gen_expr(cg, e->v.binary.right);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jnz_true2 = emit_x86_jcc_rel32_fixup(cg->text, 0x5);

        emit_x86_mov_eax_imm32(cg->text, 0);
        uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);

        uint32_t true_off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 1);
        uint32_t end_off = cg->text->size;

        patch_rel32(cg->text, jnz_true, true_off);
        patch_rel32(cg->text, jnz_true2, true_off);
        patch_rel32(cg->text, jmp_end, end_off);
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Logical operator not supported in codegen yet");
}

static void gen_expr(Codegen* cg, AstExpr* e) {
    if (!e) {
        emit_x86_mov_eax_imm32(cg->text, 0);
        return;
    }

    if (e->kind == AST_EXPR_INT_LIT) {
        emit_x86_mov_eax_imm32(cg->text, (uint32_t)e->v.int_lit);
        return;
    }

    if (e->kind == AST_EXPR_NAME) {
        Var* v = e->v.name.var;
        if (v) {
            if (v->ty && v->ty->kind == TYPE_CHAR) {
                emit_x86_movzx_eax_membp_disp(cg->text, v->ebp_offset);
            } else {
                emit_x86_mov_eax_membp_disp(cg->text, v->ebp_offset);
            }
            return;
        }

        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(cg->syms, e->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown identifier");
        }

        uint32_t off = cg->text->size;
        if (s->ty && s->ty->kind == TYPE_CHAR) {
            emit_x86_movzx_eax_memabs_u8(cg->text, 0);
            emit_reloc_text(cg, off + 3, s->elf_index, R_386_32);
        } else {
            emit_x86_mov_eax_memabs_u32(cg->text, 0);
            emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        }
        return;
    }

    if (e->kind == AST_EXPR_STR) {
        Symbol* s = cg_intern_string(cg, e->v.str.bytes, e->v.str.len);
        uint32_t off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 0);
        emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        return;
    }

    if (e->kind == AST_EXPR_CAST) {
        gen_expr(cg, e->v.cast.expr);
        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_CHAR) {
            emit_x86_and_eax_imm32(cg->text, 0xFF);
        } else if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_VOID) {
            emit_x86_mov_eax_imm32(cg->text, 0);
        }
        return;
    }

    if (e->kind == AST_EXPR_CALL) {
        if (strcmp(e->v.call.callee, "__syscall") == 0) {
            if (e->v.call.arg_count != 4) {
                scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "__syscall requires exactly 4 arguments");
            }

            for (int i = 0; i < 4; i++) {
                gen_expr(cg, e->v.call.args[i]);
                emit_x86_push_eax(cg->text);
            }

            emit_x86_pop_edx(cg->text);
            emit_x86_pop_ecx(cg->text);
            emit_x86_pop_ebx(cg->text);
            emit_x86_pop_eax(cg->text);
            emit_x86_int80(cg->text);
            return;
        }

        Symbol* s = symtab_find(cg->syms, e->v.call.callee);
        if (!s || s->kind != SYM_FUNC) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Call to undeclared function");
        }

        if (s->ftype.param_count != e->v.call.arg_count) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Argument count mismatch in call");
        }

        for (int i = e->v.call.arg_count - 1; i >= 0; i--) {
            gen_expr(cg, e->v.call.args[i]);
            emit_x86_push_eax(cg->text);
        }

        uint32_t call_site = cg->text->size;
        emit_x86_call_rel32(cg->text, -4);
        emit_reloc_text(cg, call_site + 1, s->elf_index, R_386_PC32);

        uint32_t stack_bytes = (uint32_t)e->v.call.arg_count * 4u;
        if (stack_bytes) emit_x86_add_esp_imm32(cg->text, stack_bytes);
        return;
    }

    if (e->kind == AST_EXPR_UNARY) {
        gen_expr(cg, e->v.unary.expr);
        if (e->v.unary.op == AST_UNOP_NEG) {
            emit_x86_neg_eax(cg->text);
        } else if (e->v.unary.op == AST_UNOP_NOT) {
            emit_x86_test_eax_eax(cg->text);
            emit_x86_mov_eax_imm32(cg->text, 0);
            emit_x86_setcc_al(cg->text, 0x4);
        }
        return;
    }

    if (e->kind == AST_EXPR_BINARY) {
        if (e->v.binary.op == AST_BINOP_ADD || e->v.binary.op == AST_BINOP_SUB || e->v.binary.op == AST_BINOP_MUL || e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
            gen_expr_binary_arith(cg, e);
        } else if (e->v.binary.op == AST_BINOP_EQ || e->v.binary.op == AST_BINOP_NE || e->v.binary.op == AST_BINOP_LT || e->v.binary.op == AST_BINOP_LE || e->v.binary.op == AST_BINOP_GT || e->v.binary.op == AST_BINOP_GE) {
            gen_expr_binary_cmp(cg, e);
        } else if (e->v.binary.op == AST_BINOP_ANDAND || e->v.binary.op == AST_BINOP_OROR) {
            gen_expr_binary_logical(cg, e);
        } else {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Binary operator not supported in codegen yet");
        }
        return;
    }

    if (e->kind == AST_EXPR_ASSIGN) {
        if (e->v.assign.left->kind != AST_EXPR_NAME) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Invalid assignment target");
        }

        gen_expr(cg, e->v.assign.right);

        Var* v = e->v.assign.left->v.name.var;
        if (v) {
            if (v->ty && v->ty->kind == TYPE_CHAR) {
                emit_x86_mov_membp_disp_al(cg->text, v->ebp_offset);
            } else {
                emit_x86_mov_membp_disp_eax(cg->text, v->ebp_offset);
            }
            return;
        }

        Symbol* s = e->v.assign.left->v.name.sym;
        if (!s) s = symtab_find(cg->syms, e->v.assign.left->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown identifier in assignment");
        }

        uint32_t off = cg->text->size;
        if (s->ty && s->ty->kind == TYPE_CHAR) {
            emit_x86_mov_memabs_u8_al(cg->text, 0);
            emit_reloc_text(cg, off + 2, s->elf_index, R_386_32);
        } else {
            emit_x86_mov_memabs_u32_eax(cg->text, 0);
            emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        }
        return;
     }
 
     scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown expression kind");
 }

 static int gen_stmt(Codegen* cg, AstStmt* s);

 static int gen_stmt_list(Codegen* cg, AstStmt* s) {
     while (s) {
         int did = gen_stmt(cg, s);
         if (did) return 1;
         s = s->next;
     }
     return 0;
 }

 static int gen_stmt(Codegen* cg, AstStmt* s) {
     if (!s) return 0;

     if (s->kind == AST_STMT_DECL) {
         if (s->v.decl.init) {
             gen_expr(cg, s->v.decl.init);
             Var* v = s->v.decl.decl_var;
             if (!v) scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Internal error: decl var not found");
             if (v->ty && v->ty->kind == TYPE_CHAR) emit_x86_mov_membp_disp_al(cg->text, v->ebp_offset);
             else emit_x86_mov_membp_disp_eax(cg->text, v->ebp_offset);
         }
         return 0;
     }

     if (s->kind == AST_STMT_EXPR) {
         if (s->v.expr.expr) gen_expr(cg, s->v.expr.expr);
         return 0;
     }

     if (s->kind == AST_STMT_RETURN) {
         gen_expr(cg, s->v.expr.expr);
         emit_x86_epilogue(cg->text);
         return 1;
     }

     if (s->kind == AST_STMT_BLOCK) {
         return gen_stmt_list(cg, s->v.block.first);
     }

     if (s->kind == AST_STMT_IF) {
         gen_expr(cg, s->v.if_stmt.cond);
         emit_x86_test_eax_eax(cg->text);

         if (s->v.if_stmt.else_stmt) {
             uint32_t jz_else = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
             int then_ret = gen_stmt(cg, s->v.if_stmt.then_stmt);
             uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);
             uint32_t else_off = cg->text->size;
             patch_rel32(cg->text, jz_else, else_off);
             int else_ret = gen_stmt(cg, s->v.if_stmt.else_stmt);
             uint32_t end_off = cg->text->size;
             patch_rel32(cg->text, jmp_end, end_off);
             return (then_ret && else_ret) ? 1 : 0;
         }

         uint32_t jz_end = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
         (void)gen_stmt(cg, s->v.if_stmt.then_stmt);
         uint32_t end_off = cg->text->size;
         patch_rel32(cg->text, jz_end, end_off);
         return 0;
     }

     if (s->kind == AST_STMT_WHILE) {
         uint32_t start_off = cg->text->size;

         if (cg->loop_depth >= (int)(sizeof(cg->loops) / sizeof(cg->loops[0]))) {
             scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Loop nesting too deep");
         }
         LoopCtx* lc = &cg->loops[cg->loop_depth++];
         memset(lc, 0, sizeof(*lc));
         lc->start_off = start_off;

         gen_expr(cg, s->v.while_stmt.cond);
         emit_x86_test_eax_eax(cg->text);
         uint32_t jz_end = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
         (void)gen_stmt(cg, s->v.while_stmt.body);
         uint32_t jmp_back = emit_x86_jmp_rel32_fixup(cg->text);
         patch_rel32(cg->text, jmp_back, start_off);
         uint32_t end_off = cg->text->size;
         patch_rel32(cg->text, jz_end, end_off);

         for (int i = 0; i < lc->break_count; i++) {
             patch_rel32(cg->text, lc->break_fixups[i], end_off);
         }

         cg->loop_depth--;
         return 0;
     }

     if (s->kind == AST_STMT_BREAK) {
         if (cg->loop_depth <= 0) {
             scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "break not within loop");
         }
         LoopCtx* lc = &cg->loops[cg->loop_depth - 1];
         if (lc->break_count >= (int)(sizeof(lc->break_fixups) / sizeof(lc->break_fixups[0]))) {
             scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Too many breaks in loop");
         }
         uint32_t jmp = emit_x86_jmp_rel32_fixup(cg->text);
         lc->break_fixups[lc->break_count++] = jmp;
         return 0;
     }

     if (s->kind == AST_STMT_CONTINUE) {
         if (cg->loop_depth <= 0) {
             scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "continue not within loop");
         }
         LoopCtx* lc = &cg->loops[cg->loop_depth - 1];
         uint32_t jmp = emit_x86_jmp_rel32_fixup(cg->text);
         patch_rel32(cg->text, jmp, lc->start_off);
         return 0;
     }

     scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Unknown statement kind");
     return 0;
 }

#endif
