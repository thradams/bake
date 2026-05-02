#include "bake.h"

/* ------------------------------------------------------------------ */
/* Code generation                                                      */
/* ------------------------------------------------------------------ */

static int   label_count = 0;

/* escape a string for use inside AT&T .string directive */
static void emit_string_literal(const char *s) {
    fprintf(out, "  .string \"");
    for (; *s; s++) {
        switch ((unsigned char)*s) {
            case '\n': fprintf(out, "\\n");  break;
            case '\t': fprintf(out, "\\t");  break;
            case '\r': fprintf(out, "\\r");  break;
            case '\\': fprintf(out, "\\\\"); break;
            case '"':  fprintf(out, "\\\""); break;
            case '\0': fprintf(out, "\\0");  break;
            default:   fputc(*s, out);       break;
        }
    }
    fprintf(out, "\"\n");
}
static char      *current_func_end;        /* label for return */
static char      *break_label;
static char      *continue_label;
static struct Type *current_func_ret_type; /* return type of current function */
static int        current_func_sret_offset;/* rbp offset of hidden sret pointer (large structs) */

/* System V AMD64: integer args in rdi, rsi, rdx, rcx, r8, r9 */
static const char *arg_regs[] = {
    "rdi", "rsi", "rdx", "rcx", "r8",  "r9"
};
static const char *arg_regs32[] = {
    "edi", "esi", "edx", "ecx", "r8d", "r9d"
};
static const char *arg_regs16[] = {
    "di",  "si",  "dx",  "cx",  "r8w", "r9w"
};
static const char *arg_regs8[] = {
    "dil", "sil", "dl",  "cl",  "r8b", "r9b"
};

static int new_label(void) { return label_count++; }

/* Returns true if this type should use XMM (float/double) registers */
static bool is_float_type(struct Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE);
}

/* Returns true if this node produces a float/double value */
static bool node_is_float(struct Node *n) {
    return n && is_float_type(n->type);
}

void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Struct return classification (System V AMD64 ABI)                   */
/* Structs <= 16 bytes are returned in rax[:rdx] (integer eightbytes). */
/* Larger structs use a hidden pointer in rdi (sret convention).        */
/* ------------------------------------------------------------------ */

/* Inline memcpy: copy `bytes` bytes from [src_reg] to [dst_reg]. Clobbers rcx. */
static void emit_memcpy(const char *dst_reg, const char *src_reg, int bytes) {
    int off = 0;
    while (bytes - off >= 8) {
        emit("  movq  %d(%%%s), %%rcx", off, src_reg);
        emit("  movq  %%rcx, %d(%%%s)", off, dst_reg);
        off += 8;
    }
    if (bytes - off >= 4) {
        emit("  movl  %d(%%%s), %%ecx", off, src_reg);
        emit("  movl  %%ecx, %d(%%%s)", off, dst_reg);
        off += 4;
    }
    if (bytes - off >= 2) {
        emit("  movw  %d(%%%s), %%cx", off, src_reg);
        emit("  movw  %%cx, %d(%%%s)", off, dst_reg);
        off += 2;
    }
    if (bytes - off >= 1) {
        emit("  movb  %d(%%%s), %%cl", off, src_reg);
        emit("  movb  %%cl, %d(%%%s)", off, dst_reg);
    }
}

/* size suffix for AT&T asm */
static const char *size_suffix(int bytes) __attribute__((unused));
static const char *size_suffix(int bytes) {
    switch (bytes) {
        case 1: return "b";
        case 2: return "w";
        case 4: return "l";
        case 8: return "q";
        default: return "q";
    }
}

/* load an lvalue address into rax */
static void gen_addr(struct Node *n);
/* evaluate expression, result in rax (or xmm0 for float) */
static void gen_expr(struct Node *n);
static void gen_stmt(struct Node *n);

/* find struct member */

static void gen_addr(struct Node *n) {
    switch (n->kind) {
        case ND_IDENT:
            if (n->is_local)
                emit("  leaq %d(%%rbp), %%rax", n->offset);
            else
                emit("  leaq %s(%%rip), %%rax", n->name);
            break;
        case ND_DEREF:
            gen_expr(n->lhs);
            break;
        case ND_INDEX: {
            /* addr = base + index * elem_size */
            /* if lhs is a pointer (not array), load its value; if array, take its address */
            struct Type *lhs_type = n->lhs->type;
            if (lhs_type && lhs_type->kind == TY_PTR) {
                gen_expr(n->lhs);  /* load pointer value into rax */
            } else {
                gen_addr(n->lhs);  /* address of array base */
            }
            emit("  pushq %%rax");
            gen_expr(n->rhs);
            /* element size */
            int esz = 8;
            if (lhs_type) {
                struct Type *et = (lhs_type->kind == TY_ARRAY || lhs_type->kind == TY_PTR)
                                  ? lhs_type->base : lhs_type;
                if (et) esz = type_size(et);
            }
            if (esz != 1)
                emit("  imulq $%d, %%rax", esz);
            emit("  popq %%rcx");
            emit("  addq %%rcx, %%rax");
            break;
        }
        case ND_MEMBER: {
            gen_addr(n->lhs);
            /* type of lhs must be struct */
            /* we rely on the type being set during resolution */
            /* find member offset from the struct type */
            struct Type *st = n->lhs->type;
            if (!st) die("unresolved struct type for member access");
            struct Member *m = find_member(st, n->name);
            emit("  addq $%d, %%rax", m->offset);
            break;
        }
        case ND_ARROW: {
            gen_expr(n->lhs);  /* pointer value in rax */
            struct Type *st = n->lhs->type;
            if (st && st->kind == TY_PTR) st = st->base;
            if (!st) die("unresolved struct type for arrow access");
            struct Member *m = find_member(st, n->name);
            emit("  addq $%d, %%rax", m->offset);
            break;
        }
        default:
            /* If the expression is a struct-returning call, gen_expr leaves
               rax pointing to the result buffer — that is already an address. */
            if (n->kind == ND_CALL && is_struct_type(n->type)) {
                gen_expr(n);
                return;
            }
            die("cannot take address of expression");
    }
}

static void gen_expr(struct Node *n) {
    switch (n->kind) {
        case ND_INT:
            emit("  movq $%lld, %%rax", n->ival);
            return;
        case ND_FLOAT: {
            /* store constant in .rodata and load into xmm0 */
            int lbl = new_label();
            emit("  .section .rodata");
            emit(".LC%d:", lbl);
            if (n->is_float) {
                /* 4-byte IEEE single */
                union { float f; uint32_t u; } u;
                u.f = (float)n->fval;
                emit("  .long %u", u.u);
                emit("  .text");
                emit("  movss .LC%d(%%rip), %%xmm0", lbl);
            } else {
                /* 8-byte IEEE double */
                union { double d; uint64_t u; } u;
                u.d = n->fval;
                emit("  .quad %llu", (unsigned long long)u.u);
                emit("  .text");
                emit("  movsd .LC%d(%%rip), %%xmm0", lbl);
            }
            return;
        }
        case ND_STR: {
            int lbl = new_label();
            emit("  .section .rodata");
            emit(".LC%d:", lbl);
            emit_string_literal(n->sval);
            emit("  .text");
            emit("  leaq .LC%d(%%rip), %%rax", lbl);
            return;
        }
        case ND_IDENT:
            gen_addr(n);
            /* arrays and functions decay to their address — no load needed */
            if (n->type && n->type->kind == TY_ARRAY) {
                return;
            }
            /* structs: leave rax = address (caller uses gen_addr for struct access) */
            if (is_struct_type(n->type)) {
                return;
            }
            /* function name used as value (e.g. fp = add): rax already holds address */
            if (!n->is_local && find_proto(n->name)) {
                /* it's a function name — its address is the value */
                return;
            }
            if (is_float_type(n->type)) {
                if (n->type->kind == TY_FLOAT)
                    emit("  movss (%%rax), %%xmm0");
                else
                    emit("  movsd (%%rax), %%xmm0");
                return;
            }
            {
                int sz = n->type ? type_size(n->type) : 8;
                if (sz == 8)
                    emit("  movq (%%rax), %%rax");
                else if (sz == 4)
                    emit("  movslq (%%rax), %%rax");
                else if (sz == 2)
                    emit("  movswq (%%rax), %%rax");
                else
                    emit("  movsbq (%%rax), %%rax");
            }
            return;
        case ND_INDEX:
            gen_addr(n);
            if (is_float_type(n->type)) {
                if (n->type->kind == TY_FLOAT)
                    emit("  movss (%%rax), %%xmm0");
                else
                    emit("  movsd (%%rax), %%xmm0");
                return;
            }
            {
                int sz = n->type ? type_size(n->type) : 8;
                if (sz == 8)       emit("  movq (%%rax), %%rax");
                else if (sz == 4)  emit("  movslq (%%rax), %%rax");
                else if (sz == 2)  emit("  movswq (%%rax), %%rax");
                else               emit("  movsbq (%%rax), %%rax");
            }
            return;
        case ND_ADDR:
            gen_addr(n->lhs);
            return;
        case ND_DEREF:
            gen_expr(n->lhs);
            if (is_float_type(n->type)) {
                if (n->type->kind == TY_FLOAT)
                    emit("  movss (%%rax), %%xmm0");
                else
                    emit("  movsd (%%rax), %%xmm0");
                return;
            }
            {
                int sz = n->type ? type_size(n->type) : 8;
                if (sz == 8)      emit("  movq (%%rax), %%rax");
                else if (sz == 4) emit("  movslq (%%rax), %%rax");
                else if (sz == 2) emit("  movswq (%%rax), %%rax");
                else              emit("  movsbq (%%rax), %%rax");
            }
            return;
        case ND_UNARY:
            gen_expr(n->lhs);
            if (node_is_float(n->lhs)) {
                /* float/double negate: XOR sign bit */
                bool is_f = (n->lhs->type->kind == TY_FLOAT);
                if (n->op == TK_MINUS) {
                    int lbl = new_label();
                    emit("  .section .rodata");
                    emit(".LC%d:", lbl);
                    if (is_f) {
                        emit("  .long 0x80000000");   /* float sign mask */
                        emit("  .text");
                        emit("  movss .LC%d(%%rip), %%xmm1", lbl);
                        emit("  xorps %%xmm1, %%xmm0");
                    } else {
                        emit("  .quad 0x8000000000000000"); /* double sign mask */
                        emit("  .text");
                        emit("  movsd .LC%d(%%rip), %%xmm1", lbl);
                        emit("  xorpd %%xmm1, %%xmm0");
                    }
                } else if (n->op == TK_BANG) {
                    /* !f: compare to 0.0, result is int in rax */
                    if (is_f) {
                        emit("  xorps %%xmm1, %%xmm1");
                        emit("  ucomiss %%xmm1, %%xmm0");
                    } else {
                        emit("  xorpd %%xmm1, %%xmm1");
                        emit("  ucomisd %%xmm1, %%xmm0");
                    }
                    emit("  sete %%al");
                    emit("  movzbq %%al, %%rax");
                }
                return;
            }
            switch (n->op) {
                case TK_MINUS: emit("  negq %%rax"); break;
                case TK_BANG:
                    emit("  cmpq $0, %%rax");
                    emit("  sete %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_TILDE: emit("  notq %%rax"); break;
                default: break;
            }
            return;
        case ND_BINARY: {
            /* short-circuit && and || -- always produce int result */
            if (n->op == TK_AND) {
                int lbl = new_label();
                gen_expr(n->lhs);
                /* test lhs: for float, compare to 0 */
                if (node_is_float(n->lhs)) {
                    bool is_f = (n->lhs->type->kind == TY_FLOAT);
                    if (is_f) { emit("  xorps %%xmm1, %%xmm1"); emit("  ucomiss %%xmm1, %%xmm0"); }
                    else      { emit("  xorpd %%xmm1, %%xmm1"); emit("  ucomisd %%xmm1, %%xmm0"); }
                    emit("  sete %%al"); emit("  movzbq %%al, %%rax");
                    emit("  cmpq $0, %%rax");
                } else {
                    emit("  cmpq $0, %%rax");
                }
                emit("  je .Lfalse%d", lbl);
                gen_expr(n->rhs);
                if (node_is_float(n->rhs)) {
                    bool is_f = (n->rhs->type->kind == TY_FLOAT);
                    if (is_f) { emit("  xorps %%xmm1, %%xmm1"); emit("  ucomiss %%xmm1, %%xmm0"); }
                    else      { emit("  xorpd %%xmm1, %%xmm1"); emit("  ucomisd %%xmm1, %%xmm0"); }
                    emit("  sete %%al"); emit("  movzbq %%al, %%rax");
                    emit("  cmpq $0, %%rax");
                } else {
                    emit("  cmpq $0, %%rax");
                }
                emit("  je .Lfalse%d", lbl);
                emit("  movq $1, %%rax");
                emit("  jmp .Lend%d", lbl);
                emit(".Lfalse%d:", lbl);
                emit("  xorq %%rax, %%rax");
                emit(".Lend%d:", lbl);
                return;
            }
            if (n->op == TK_OR) {
                int lbl = new_label();
                gen_expr(n->lhs);
                if (node_is_float(n->lhs)) {
                    bool is_f = (n->lhs->type->kind == TY_FLOAT);
                    if (is_f) { emit("  xorps %%xmm1, %%xmm1"); emit("  ucomiss %%xmm1, %%xmm0"); }
                    else      { emit("  xorpd %%xmm1, %%xmm1"); emit("  ucomisd %%xmm1, %%xmm0"); }
                    emit("  setne %%al"); emit("  movzbq %%al, %%rax");
                    emit("  cmpq $0, %%rax");
                } else {
                    emit("  cmpq $0, %%rax");
                }
                emit("  jne .Ltrue%d", lbl);
                gen_expr(n->rhs);
                if (node_is_float(n->rhs)) {
                    bool is_f = (n->rhs->type->kind == TY_FLOAT);
                    if (is_f) { emit("  xorps %%xmm1, %%xmm1"); emit("  ucomiss %%xmm1, %%xmm0"); }
                    else      { emit("  xorpd %%xmm1, %%xmm1"); emit("  ucomisd %%xmm1, %%xmm0"); }
                    emit("  setne %%al"); emit("  movzbq %%al, %%rax");
                    emit("  cmpq $0, %%rax");
                } else {
                    emit("  cmpq $0, %%rax");
                }
                emit("  jne .Ltrue%d", lbl);
                emit("  xorq %%rax, %%rax");
                emit("  jmp .Lend%d", lbl);
                emit(".Ltrue%d:", lbl);
                emit("  movq $1, %%rax");
                emit(".Lend%d:", lbl);
                return;
            }

            /* float/double arithmetic */
            bool lhs_float = node_is_float(n->lhs);
            bool rhs_float = node_is_float(n->rhs);
            if (lhs_float || rhs_float) {
                bool use_double = (n->lhs->type && n->lhs->type->kind == TY_DOUBLE) ||
                                  (n->rhs->type && n->rhs->type->kind == TY_DOUBLE);
                /* evaluate rhs -> xmm0, push to stack (8 bytes), then lhs -> xmm0 */
                gen_expr(n->rhs);
                /* if rhs is int, convert to float/double */
                if (!rhs_float) {
                    if (use_double) emit("  cvtsi2sdq %%rax, %%xmm0");
                    else            emit("  cvtsi2ssq %%rax, %%xmm0");
                } else if (!use_double && n->rhs->type->kind == TY_FLOAT && use_double) {
                    emit("  cvtss2sd %%xmm0, %%xmm0");
                }
                /* spill xmm0 to stack */
                emit("  subq $8, %%rsp");
                if (use_double) emit("  movsd %%xmm0, (%%rsp)");
                else            emit("  movss %%xmm0, (%%rsp)");

                gen_expr(n->lhs);
                if (!lhs_float) {
                    if (use_double) emit("  cvtsi2sdq %%rax, %%xmm0");
                    else            emit("  cvtsi2ssq %%rax, %%xmm0");
                } else if (!use_double && n->lhs->type->kind == TY_FLOAT && use_double) {
                    emit("  cvtss2sd %%xmm0, %%xmm0");
                }
                /* reload rhs into xmm1 */
                if (use_double) emit("  movsd (%%rsp), %%xmm1");
                else            emit("  movss (%%rsp), %%xmm1");
                emit("  addq $8, %%rsp");

                /* now: xmm0=lhs, xmm1=rhs */
                if (use_double) {
                    switch (n->op) {
                        case TK_PLUS:  emit("  addsd %%xmm1, %%xmm0"); break;
                        case TK_MINUS: emit("  subsd %%xmm1, %%xmm0"); break;
                        case TK_STAR:  emit("  mulsd %%xmm1, %%xmm0"); break;
                        case TK_SLASH: emit("  divsd %%xmm1, %%xmm0"); break;
                        case TK_EQ:
                            emit("  ucomisd %%xmm1, %%xmm0");
                            emit("  sete %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_NEQ:
                            emit("  ucomisd %%xmm1, %%xmm0");
                            emit("  setne %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_LT:
                            emit("  ucomisd %%xmm1, %%xmm0");
                            emit("  setb %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_GT:
                            emit("  ucomisd %%xmm0, %%xmm1");
                            emit("  setb %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_LEQ:
                            emit("  ucomisd %%xmm1, %%xmm0");
                            emit("  setbe %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_GEQ:
                            emit("  ucomisd %%xmm0, %%xmm1");
                            emit("  setbe %%al"); emit("  movzbq %%al, %%rax"); break;
                        default: die("unsupported operator on double"); break;
                    }
                } else {
                    switch (n->op) {
                        case TK_PLUS:  emit("  addss %%xmm1, %%xmm0"); break;
                        case TK_MINUS: emit("  subss %%xmm1, %%xmm0"); break;
                        case TK_STAR:  emit("  mulss %%xmm1, %%xmm0"); break;
                        case TK_SLASH: emit("  divss %%xmm1, %%xmm0"); break;
                        case TK_EQ:
                            emit("  ucomiss %%xmm1, %%xmm0");
                            emit("  sete %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_NEQ:
                            emit("  ucomiss %%xmm1, %%xmm0");
                            emit("  setne %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_LT:
                            emit("  ucomiss %%xmm1, %%xmm0");
                            emit("  setb %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_GT:
                            emit("  ucomiss %%xmm0, %%xmm1");
                            emit("  setb %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_LEQ:
                            emit("  ucomiss %%xmm1, %%xmm0");
                            emit("  setbe %%al"); emit("  movzbq %%al, %%rax"); break;
                        case TK_GEQ:
                            emit("  ucomiss %%xmm0, %%xmm1");
                            emit("  setbe %%al"); emit("  movzbq %%al, %%rax"); break;
                        default: die("unsupported operator on float"); break;
                    }
                }
                return;
            }

            /* integer arithmetic (original path) */
            /* detect pointer arithmetic */
            bool lhs_is_ptr = n->lhs->type &&
                              (n->lhs->type->kind == TY_PTR ||
                               n->lhs->type->kind == TY_ARRAY);
            bool rhs_is_ptr = n->rhs->type &&
                              (n->rhs->type->kind == TY_PTR ||
                               n->rhs->type->kind == TY_ARRAY);

            /* ptr - ptr: subtract bytes then divide by element size */
            if (n->op == TK_MINUS && lhs_is_ptr && rhs_is_ptr) {
                int esz = 1;
                struct Type *base = n->lhs->type->base;
                if (base) esz = type_size(base);
                gen_expr(n->rhs);
                emit("  pushq %%rax");
                gen_expr(n->lhs);
                emit("  popq %%rcx");
                emit("  subq %%rcx, %%rax");   /* byte difference in rax */
                if (esz > 1) {
                    emit("  movq $%d, %%rcx", esz);
                    emit("  cqto");
                    emit("  idivq %%rcx");       /* rax = byte_diff / esz */
                }
                return;
            }

            /* ptr +/- int: scale the integer by element size */
            int ptr_scale = 1;
            if ((n->op == TK_PLUS || n->op == TK_MINUS) && lhs_is_ptr && !rhs_is_ptr) {
                struct Type *base = n->lhs->type->base;
                if (base) ptr_scale = type_size(base);
            }
            /* int + ptr: commute so pointer is on lhs */
            if (n->op == TK_PLUS && !lhs_is_ptr && rhs_is_ptr) {
                struct Type *base = n->rhs->type->base;
                if (base) ptr_scale = type_size(base);
                /* evaluate lhs (int), scale it, then add rhs (ptr) */
                gen_expr(n->lhs);
                if (ptr_scale > 1) emit("  imulq $%d, %%rax", ptr_scale);
                emit("  pushq %%rax");
                gen_expr(n->rhs);
                emit("  popq %%rcx");
                emit("  addq %%rcx, %%rax");
                return;
            }

            gen_expr(n->rhs);
            if (ptr_scale > 1) emit("  imulq $%d, %%rax", ptr_scale);
            emit("  pushq %%rax");
            gen_expr(n->lhs);
            emit("  popq %%rcx");
            switch (n->op) {
                case TK_PLUS:   emit("  addq %%rcx, %%rax"); break;
                case TK_MINUS:  emit("  subq %%rcx, %%rax"); break;
                case TK_STAR:   emit("  imulq %%rcx, %%rax"); break;
                case TK_SLASH:
                    emit("  cqto");
                    emit("  idivq %%rcx");
                    break;
                case TK_PERCENT:
                    emit("  cqto");
                    emit("  idivq %%rcx");
                    emit("  movq %%rdx, %%rax");
                    break;
                case TK_AMP:    emit("  andq %%rcx, %%rax"); break;
                case TK_PIPE:   emit("  orq  %%rcx, %%rax"); break;
                case TK_CARET:  emit("  xorq %%rcx, %%rax"); break;
                case TK_LSHIFT: emit("  salq %%cl,  %%rax"); break;
                case TK_RSHIFT: emit("  sarq %%cl,  %%rax"); break;
                case TK_EQ:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  sete %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_NEQ:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  setne %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_LT:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  setl %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_GT:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  setg %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_LEQ:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  setle %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                case TK_GEQ:
                    emit("  cmpq %%rcx, %%rax");
                    emit("  setge %%al");
                    emit("  movzbq %%al, %%rax");
                    break;
                default: break;
            }
            return;
        }
        case ND_ASSIGN: {
            /* Struct assignment: s1 = s2  (only plain TK_ASSIGN makes sense) */
            if (is_struct_type(n->lhs->type)) {
                int sz = type_size(n->lhs->type);
                bool rhs_is_call = (n->rhs->kind == ND_CALL);
                if (rhs_is_call) {
                    gen_expr(n->rhs);          /* rax = pointer to temp result */
                    emit("  movq %%rax, %%rsi");
                } else {
                    gen_addr(n->rhs);
                    emit("  movq %%rax, %%rsi");
                }
                gen_addr(n->lhs);              /* rax = destination address */
                emit("  movq %%rax, %%rdi");
                emit_memcpy("rdi", "rsi", sz);
                emit("  movq %%rdi, %%rax");   /* return lhs address as value */
                if (rhs_is_call && n->rhs->ival > 0) {
                    emit("  addq $%lld, %%rsp", n->rhs->ival);
                }
                return;
            }
            bool lhs_float = is_float_type(n->lhs->type);
            if (lhs_float) {
                bool is_d = (n->lhs->type->kind == TY_DOUBLE);
                /* evaluate rhs into xmm0 */
                gen_expr(n->rhs);
                /* convert if rhs is integer */
                if (!node_is_float(n->rhs)) {
                    if (is_d) emit("  cvtsi2sdq %%rax, %%xmm0");
                    else      emit("  cvtsi2ssq %%rax, %%xmm0");
                } else if (is_d && n->rhs->type && n->rhs->type->kind == TY_FLOAT) {
                    emit("  cvtss2sd %%xmm0, %%xmm0");
                } else if (!is_d && n->rhs->type && n->rhs->type->kind == TY_DOUBLE) {
                    emit("  cvtsd2ss %%xmm0, %%xmm0");
                }
                /* spill xmm0, get address, restore */
                emit("  subq $8, %%rsp");
                if (is_d) emit("  movsd %%xmm0, (%%rsp)");
                else      emit("  movss %%xmm0, (%%rsp)");
                gen_addr(n->lhs);
                if (is_d) {
                    emit("  movsd (%%rsp), %%xmm0");
                    emit("  addq $8, %%rsp");
                    if (n->op == TK_ASSIGN) {
                        emit("  movsd %%xmm0, (%%rax)");
                    } else {
                        /* compound float assign */
                        emit("  movsd (%%rax), %%xmm1");
                        switch (n->op) {
                            case TK_PLUS_ASSIGN:  emit("  addsd %%xmm0, %%xmm1"); break;
                            case TK_MINUS_ASSIGN: emit("  subsd %%xmm0, %%xmm1"); break;
                            case TK_STAR_ASSIGN:  emit("  mulsd %%xmm0, %%xmm1"); break;
                            case TK_SLASH_ASSIGN: emit("  divsd %%xmm0, %%xmm1"); break;
                            default: die("unsupported compound assign on double"); break;
                        }
                        emit("  movsd %%xmm1, (%%rax)");
                        emit("  movsd %%xmm1, %%xmm0");
                    }
                } else {
                    emit("  movss (%%rsp), %%xmm0");
                    emit("  addq $8, %%rsp");
                    if (n->op == TK_ASSIGN) {
                        emit("  movss %%xmm0, (%%rax)");
                    } else {
                        emit("  movss (%%rax), %%xmm1");
                        switch (n->op) {
                            case TK_PLUS_ASSIGN:  emit("  addss %%xmm0, %%xmm1"); break;
                            case TK_MINUS_ASSIGN: emit("  subss %%xmm0, %%xmm1"); break;
                            case TK_STAR_ASSIGN:  emit("  mulss %%xmm0, %%xmm1"); break;
                            case TK_SLASH_ASSIGN: emit("  divss %%xmm0, %%xmm1"); break;
                            default: die("unsupported compound assign on float"); break;
                        }
                        emit("  movss %%xmm1, (%%rax)");
                        emit("  movss %%xmm1, %%xmm0");
                    }
                }
                return;
            }
            /* integer assign (original path) */
            gen_expr(n->rhs);
            /* if rhs is float and lhs is int, convert */
            if (node_is_float(n->rhs)) {
                if (n->rhs->type->kind == TY_FLOAT)
                    emit("  cvttss2siq %%xmm0, %%rax");
                else
                    emit("  cvttsd2siq %%xmm0, %%rax");
            }
            emit("  pushq %%rax");
            gen_addr(n->lhs);
            emit("  popq %%rcx");
            if (n->op != TK_ASSIGN) {
                int sz = n->lhs->type ? type_size(n->lhs->type) : 8;
                emit("  pushq %%rax");
                if      (sz == 8) emit("  movq (%%rax), %%rax");
                else if (sz == 4) emit("  movslq (%%rax), %%rax");
                else if (sz == 2) emit("  movswq (%%rax), %%rax");
                else              emit("  movsbq (%%rax), %%rax");
                emit("  pushq %%rax");
                emit("  movq %%rcx, %%rax");
                emit("  popq %%rcx");
                emit("  xchgq %%rax, %%rcx");
                switch (n->op) {
                    case TK_PLUS_ASSIGN:   emit("  addq %%rcx, %%rax"); break;
                    case TK_MINUS_ASSIGN:  emit("  subq %%rcx, %%rax"); break;
                    case TK_STAR_ASSIGN:   emit("  imulq %%rcx, %%rax"); break;
                    case TK_SLASH_ASSIGN:  emit("  cqto"); emit("  idivq %%rcx"); break;
                    case TK_PERCENT_ASSIGN: emit("  cqto"); emit("  idivq %%rcx"); emit("  movq %%rdx, %%rax"); break;
                    case TK_AMP_ASSIGN:    emit("  andq %%rcx, %%rax"); break;
                    case TK_PIPE_ASSIGN:   emit("  orq  %%rcx, %%rax"); break;
                    case TK_CARET_ASSIGN:  emit("  xorq %%rcx, %%rax"); break;
                    case TK_LSHIFT_ASSIGN: emit("  salq %%cl,  %%rax"); break;
                    case TK_RSHIFT_ASSIGN: emit("  sarq %%cl,  %%rax"); break;
                    default: break;
                }
                emit("  movq %%rax, %%rcx");
                emit("  popq %%rax");
            }
            {
                int sz = n->lhs->type ? type_size(n->lhs->type) : 8;
                if (sz == 8)      emit("  movq %%rcx, (%%rax)");
                else if (sz == 4) emit("  movl %%ecx, (%%rax)");
                else if (sz == 2) emit("  movw %%cx,  (%%rax)");
                else              emit("  movb %%cl,  (%%rax)");
            }
            emit("  movq %%rcx, %%rax");
            return;
        }
        case ND_TERNARY: {
            int lbl = new_label();
            gen_expr(n->cond);
            /* test cond (may be float) */
            if (node_is_float(n->cond)) {
                bool is_f = (n->cond->type->kind == TY_FLOAT);
                if (is_f) { emit("  xorps %%xmm1, %%xmm1"); emit("  ucomiss %%xmm1, %%xmm0"); }
                else      { emit("  xorpd %%xmm1, %%xmm1"); emit("  ucomisd %%xmm1, %%xmm0"); }
                emit("  sete %%al"); emit("  movzbq %%al, %%rax");
            }
            emit("  cmpq $0, %%rax");
            emit("  je .Lelse%d", lbl);
            gen_expr(n->then);
            emit("  jmp .Lend%d", lbl);
            emit(".Lelse%d:", lbl);
            gen_expr(n->els);
            emit(".Lend%d:", lbl);
            return;
        }
        case ND_CALL: {
            /* collect args */
            int argc = 0;
            struct Node *args[64];
            for (struct Node *a = n->args; a; a = a->next) {
                if (argc < 64) args[argc] = a;
                argc++;
            }

            /* System V AMD64 ABI:
               Stack layout (low addr = top of stack):
                 [large-struct arg copies]  <- allocated FIRST (safe from callee frame)
                 [sret return buffer]       <- r10 points here (if call_sret)
                 [stack args + pad]
                 [return address]           <- callq pushes this
            */
            int int_arg_idx = 0;
            int xmm_arg_idx = 0;

            struct Type *call_ret = n->type;
            bool call_sret = struct_needs_sret(call_ret);
            bool call_sreg = struct_fits_regs(call_ret);

            /* --- classify args --- */
            bool is_stack[64] = {false};
            int  stack_argc   = 0;
            {
                int ii = call_sret ? 1 : 0;
                int xi = 0;
                for (int i = 0; i < argc; i++) {
                    struct Type *at = args[i]->type;
                    if (is_float_type(at)) {
                        if (xi < 8) xi++;
                        else { is_stack[i] = true; stack_argc++; }
                    } else if (struct_fits_regs(at)) {
                        int nregs = (type_size(at) + 7) / 8;
                        if (ii + nregs <= 6) ii += nregs;
                        else { is_stack[i] = true; stack_argc += nregs; }
                    } else if (struct_needs_sret(at)) {
                        if (ii < 6) ii++;
                        else { is_stack[i] = true; stack_argc++; }
                    } else {
                        if (ii < 6) ii++;
                        else { is_stack[i] = true; stack_argc++; }
                    }
                }
            }

            /* --- Step 1: allocate large-struct arg copies ABOVE sret buffer ---
               Pre-compute total, allocate once, fill each copy.
               These sit in caller stack space; the callee cannot overwrite them. */
            int lsarg_offsets[64];
            memset(lsarg_offsets, 0, sizeof(lsarg_offsets));
            int lsarg_total = 0;
            for (int i = 0; i < argc; i++) {
                if (!is_stack[i] && struct_needs_sret(args[i]->type)) {
                    lsarg_offsets[i] = lsarg_total;
                    lsarg_total += (type_size(args[i]->type) + 15) & ~15;
                }
            }
            if (lsarg_total) {
                emit("  subq $%d, %%rsp", lsarg_total);
                emit("  movq %%rsp, %%r10"); /* temp base; overwritten by sret below */
                for (int i = 0; i < argc; i++) {
                    if (is_stack[i] || !struct_needs_sret(args[i]->type)) continue;
                    int sz = type_size(args[i]->type);
                    gen_addr(args[i]);
                    emit("  movq  %%rax, %%rsi");
                    emit("  leaq  %d(%%r10), %%rdi", lsarg_offsets[i]);
                    emit_memcpy("rdi", "rsi", sz);
                }
            }

            /* --- Step 2: allocate sret return buffer below copies --- */
            int sret_sz_aligned = call_sret ? ((type_size(call_ret) + 15) & ~15) : 0;
            if (call_sret) {
                emit("  subq $%d, %%rsp", sret_sz_aligned);
                emit("  movq %%rsp, %%r10");
            }

            /* --- Step 3: alignment pad + stack args (right-to-left) --- */
            int total_pushes = stack_argc + (n->callee ? 1 : 0);
            int pad = (total_pushes % 2 != 0) ? 1 : 0;
            if (pad) emit("  subq $8, %%rsp");

            for (int i = argc - 1; i >= 0; i--) {
                if (!is_stack[i]) continue;
                struct Type *at = args[i]->type;
                if (is_float_type(at)) {
                    gen_expr(args[i]);
                    emit("  subq $8, %%rsp");
                    if (at->kind == TY_FLOAT) {
                        emit("  cvtss2sd %%xmm0, %%xmm0");
                        emit("  movsd %%xmm0, (%%rsp)");
                    } else {
                        emit("  movsd %%xmm0, (%%rsp)");
                    }
                } else if (struct_fits_regs(at)) {
                    int sz = type_size(at);
                    gen_addr(args[i]);
                    emit("  movq  %%rax, %%rcx");
                    if (sz > 8) { emit("  movq  8(%%rcx), %%rax"); emit("  pushq %%rax"); }
                    emit("  movq  (%%rcx), %%rax");
                    emit("  pushq %%rax");
                } else {
                    gen_expr(args[i]);
                    emit("  pushq %%rax");
                }
            }

            /* --- Step 4: indirect callee ptr --- */
            if (n->callee) {
                gen_expr(n->callee);
                emit("  pushq %%rax");
            }

            /* --- Step 5: register args (right-to-left spill, forward reload) --- */
            {
                int_arg_idx = call_sret ? 1 : 0;
                xmm_arg_idx = 0;
                /* Track bytes pushed so far in this loop so that the leaq offset
                   for large-struct arg pointers stays correct as rsp moves. */
                int spilled_so_far = 0;
                for (int i = argc - 1; i >= 0; i--) {
                    if (is_stack[i]) continue;
                    struct Type *at = args[i]->type;
                    if (is_float_type(at)) {
                        gen_expr(args[i]);
                        emit("  subq $8, %%rsp");
                        if (at->kind == TY_FLOAT) emit("  movss %%xmm0, (%%rsp)");
                        else                      emit("  movsd %%xmm0, (%%rsp)");
                        spilled_so_far += 8;
                    } else if (struct_fits_regs(at)) {
                        int sz = type_size(at);
                        gen_addr(args[i]);
                        emit("  movq  %%rax, %%rcx");
                        emit("  movq  (%%rcx), %%rax"); emit("  pushq %%rax");
                        spilled_so_far += 8;
                        if (sz > 8) {
                            emit("  movq  8(%%rcx), %%rax"); emit("  pushq %%rax");
                            spilled_so_far += 8;
                        }
                    } else if (struct_needs_sret(at)) {
                        /* Address of pre-filled copy relative to *current* rsp.
                           block base = rsp + spilled_so_far + (stack_argc+pad)*8
                                            + sret_sz_aligned
                           copy i starts at block_base + lsarg_offsets[i] */
                        int block_base_from_rsp = spilled_so_far
                                                  + (stack_argc + pad) * 8
                                                  + sret_sz_aligned;
                        emit("  leaq  %d(%%rsp), %%rax",
                             block_base_from_rsp + lsarg_offsets[i]);
                        emit("  pushq %%rax");
                        spilled_so_far += 8;
                    } else {
                        gen_expr(args[i]);
                        emit("  pushq %%rax");
                        spilled_so_far += 8;
                    }
                }
                for (int i = 0; i < argc; i++) {
                    if (is_stack[i]) continue;
                    struct Type *at = args[i]->type;
                    if (is_float_type(at)) {
                        if (at->kind == TY_FLOAT) emit("  movss (%%rsp), %%xmm%d", xmm_arg_idx);
                        else                      emit("  movsd (%%rsp), %%xmm%d", xmm_arg_idx);
                        emit("  addq $8, %%rsp");
                        xmm_arg_idx++;
                    } else if (struct_fits_regs(at)) {
                        int sz = type_size(at);
                        emit("  popq %%%s", arg_regs[int_arg_idx++]);
                        if (sz > 8) emit("  popq %%%s", arg_regs[int_arg_idx++]);
                    } else {
                        emit("  popq %%%s", arg_regs[int_arg_idx++]);
                    }
                }
            }

            /* --- Step 6: set sret ptr --- */
            if (call_sret) emit("  movq %%r10, %%rdi");

            /* --- Step 7: call --- */
            struct ProtoEntry *pe = n->name ? find_proto(n->name) : NULL;
            bool is_var = pe && pe->is_variadic;
            if (!is_var && n->callee) {
                struct Type *ct = n->callee->type;
                if (ct && ct->kind == TY_PTR && ct->base && ct->base->kind == TY_FUNC)
                    is_var = ct->base->func_variadic;
            }
            if (n->callee) {
                emit("  popq %%r11");
                if (is_var) emit("  movb $%d, %%al", xmm_arg_idx);
                else        emit("  xorq %%rax, %%rax");
                emit("  callq *%%r11");
            } else {
                if (is_var) emit("  movb $%d, %%al", xmm_arg_idx);
                else        emit("  xorq %%rax, %%rax");
                emit("  callq %s", n->name);
            }

            /* --- Step 8: cleanup stack args + pad ---
               Large-struct copies and sret buffer are consumed by ND_DECL/ND_ASSIGN. */
            {
                int cleanup = (stack_argc + pad) * 8;
                if (cleanup) emit("  addq $%d, %%rsp", cleanup);
            }

            /* --- Step 9: struct result ---
               sret : rax = r10 (sret buffer on stack, above lsarg copies).
                      ND_DECL frees sret_sz_aligned + lsarg_total bytes.
               sreg : unpack rax:rdx into a 16-byte stack slot; rax = ptr.
                      ND_DECL frees 16 bytes.
               Store total bytes to reclaim in n->ival for consumers (ND_DECL/ND_ASSIGN). */
            if (call_sret) {
                /* nothing: rax already points to sret buffer */
                n->ival = sret_sz_aligned + lsarg_total;
            } else if (call_sreg) {
                int sz = type_size(call_ret);
                emit("  subq $16, %%rsp");
                emit("  movq %%rax, (%%rsp)");
                if (sz > 8) emit("  movq %%rdx, 8(%%rsp)");
                emit("  movq %%rsp, %%rax");
                n->ival = 16;
            } else {
                n->ival = 0;
            }

            return;
        }
        case ND_MEMBER:
        case ND_ARROW:
            gen_addr(n);
            if (is_float_type(n->type)) {
                if (n->type->kind == TY_FLOAT)
                    emit("  movss (%%rax), %%xmm0");
                else
                    emit("  movsd (%%rax), %%xmm0");
                return;
            }
            {
                int sz = n->type ? type_size(n->type) : 8;
                if (sz == 8)      emit("  movq (%%rax), %%rax");
                else if (sz == 4) emit("  movslq (%%rax), %%rax");
                else if (sz == 2) emit("  movswq (%%rax), %%rax");
                else              emit("  movsbq (%%rax), %%rax");
            }
            return;
        case ND_CAST: {
            gen_expr(n->lhs);
            bool src_float = node_is_float(n->lhs);
            bool dst_float = is_float_type(n->type);
            if (src_float && dst_float) {
                /* float <-> double */
                bool src_d = (n->lhs->type->kind == TY_DOUBLE);
                bool dst_d = (n->type->kind == TY_DOUBLE);
                if (src_d && !dst_d) emit("  cvtsd2ss %%xmm0, %%xmm0");
                else if (!src_d && dst_d) emit("  cvtss2sd %%xmm0, %%xmm0");
                /* same kind: no-op */
            } else if (!src_float && dst_float) {
                /* int -> float/double */
                if (n->type->kind == TY_FLOAT) emit("  cvtsi2ssq %%rax, %%xmm0");
                else                           emit("  cvtsi2sdq %%rax, %%xmm0");
            } else if (src_float && !dst_float) {
                /* float/double -> int */
                if (n->lhs->type->kind == TY_FLOAT) emit("  cvttss2siq %%xmm0, %%rax");
                else                                emit("  cvttsd2siq %%xmm0, %%rax");
                /* truncate to target width */
                int sz = n->type ? type_size(n->type) : 8;
                if (sz == 1)      emit("  movsbq %%al, %%rax");
                else if (sz == 2) emit("  movswq %%ax, %%rax");
                else if (sz == 4) emit("  movslq %%eax, %%rax");
            }
            /* int->int cast: rax already has the value; width truncation on use */
            return;
        }
        case ND_PREINC: case ND_PREDEC:
        case ND_POSTINC: case ND_POSTDEC: {
            gen_addr(n->lhs);
            bool fp = is_float_type(n->type);
            if (fp) {
                bool is_d = (n->type->kind == TY_DOUBLE);
                /* load current value */
                if (is_d) emit("  movsd (%%rax), %%xmm0");
                else      emit("  movss (%%rax), %%xmm0");
                /* spill address */
                emit("  pushq %%rax");
                /* build constant 1.0 in xmm1 */
                int lbl = new_label();
                emit("  .section .rodata");
                emit(".LC%d:", lbl);
                if (is_d) {
                    union { double d; uint64_t u; } u; u.d = 1.0;
                    emit("  .quad %llu", (unsigned long long)u.u);
                    emit("  .text");
                    emit("  movsd .LC%d(%%rip), %%xmm1", lbl);
                } else {
                    union { float f; uint32_t u; } u; u.f = 1.0f;
                    emit("  .long %u", u.u);
                    emit("  .text");
                    emit("  movss .LC%d(%%rip), %%xmm1", lbl);
                }
                if (n->kind == ND_POSTINC || n->kind == ND_POSTDEC) {
                    /* save original in xmm2 */
                    if (is_d) emit("  movsd %%xmm0, %%xmm2");
                    else      emit("  movss %%xmm0, %%xmm2");
                    if (n->kind == ND_POSTINC) {
                        if (is_d) emit("  addsd %%xmm1, %%xmm0");
                        else      emit("  addss %%xmm1, %%xmm0");
                    } else {
                        if (is_d) emit("  subsd %%xmm1, %%xmm0");
                        else      emit("  subss %%xmm1, %%xmm0");
                    }
                    emit("  popq %%rax");
                    if (is_d) emit("  movsd %%xmm0, (%%rax)");
                    else      emit("  movss %%xmm0, (%%rax)");
                    /* return old value */
                    if (is_d) emit("  movsd %%xmm2, %%xmm0");
                    else      emit("  movss %%xmm2, %%xmm0");
                } else {
                    if (n->kind == ND_PREINC) {
                        if (is_d) emit("  addsd %%xmm1, %%xmm0");
                        else      emit("  addss %%xmm1, %%xmm0");
                    } else {
                        if (is_d) emit("  subsd %%xmm1, %%xmm0");
                        else      emit("  subss %%xmm1, %%xmm0");
                    }
                    emit("  popq %%rax");
                    if (is_d) emit("  movsd %%xmm0, (%%rax)");
                    else      emit("  movss %%xmm0, (%%rax)");
                }
                return;
            }
            int sz = type_size(n->type);
            bool is_ptr = n->type && (n->type->kind == TY_PTR || n->type->kind == TY_ARRAY);
            int step = 1;
            if (is_ptr) {
                step = (n->type->base) ? type_size(n->type->base) : 1;
                if (step < 1) step = 1;
                sz = 8;
            }
            if      (sz == 8) emit("  movq (%%rax), %%rcx");
            else if (sz == 4) emit("  movslq (%%rax), %%rcx");
            else if (sz == 2) emit("  movswq (%%rax), %%rcx");
            else              emit("  movsbq (%%rax), %%rcx");
            if (n->kind == ND_POSTINC || n->kind == ND_POSTDEC) {
                emit("  movq %%rcx, %%rdx");
                if (n->kind == ND_POSTINC) emit("  addq $%d, %%rdx", step);
                else                       emit("  subq $%d, %%rdx", step);
                if      (sz == 8) emit("  movq  %%rdx, (%%rax)");
                else if (sz == 4) emit("  movl  %%edx, (%%rax)");
                else if (sz == 2) emit("  movw  %%dx,  (%%rax)");
                else              emit("  movb  %%dl,  (%%rax)");
                emit("  movq %%rcx, %%rax");
            } else {
                if (n->kind == ND_PREINC) emit("  addq $%d, %%rcx", step);
                else                      emit("  subq $%d, %%rcx", step);
                if      (sz == 8) emit("  movq  %%rcx, (%%rax)");
                else if (sz == 4) emit("  movl  %%ecx, (%%rax)");
                else if (sz == 2) emit("  movw  %%cx,  (%%rax)");
                else              emit("  movb  %%cl,  (%%rax)");
                emit("  movq %%rcx, %%rax");
            }
            return;
        }
        default:
            die("unhandled expression kind %d", n->kind);
    }
}

/* ------------------------------------------------------------------ */
/* Local variable list initializer -- runtime stores into stack slots  */
/* ------------------------------------------------------------------ */

static void emit_local_init_elem(struct Node *val, struct Type *type, int base_offset);

static void emit_local_init_list(struct Node *list, struct Type *type, int base_offset) {
    if (!list || list->kind != ND_INIT_LIST) {
        /* scalar wrapped in list -- shouldn't happen, but handle defensively */
        emit_local_init_elem(list, type, base_offset);
        return;
    }

    if (type->kind == TY_ARRAY) {
        struct Type *et = type->base;
        int esz = type_size(et);
        int count = type->array_size;

        /* Build index->node map (same as global emit_array_init) */
        int max_idx = 0;
        for (struct Node *e = list->stmts; e; e = e->next)
            if ((int)e->ival > max_idx) max_idx = (int)e->ival;
        int n_elems = (count > 0) ? count : max_idx + 1;

        /* Zero the entire array first -- simpler than tracking gaps */
        emit("  leaq  %d(%%rbp), %%rdi", base_offset);
        emit("  movl  $%d, %%ecx", type_size(type));
        emit("  xorl  %%eax, %%eax");
        emit("  rep stosb");

        /* Then store only the initialised elements */
        for (struct Node *e = list->stmts; e; e = e->next) {
            int idx = (int)e->ival;
            if (idx < 0 || (count > 0 && idx >= count)) continue;
            int elem_off = base_offset + idx * esz;
            if (e->lhs && e->lhs->kind == ND_INIT_LIST)
                emit_local_init_list(e->lhs, et, elem_off);
            else
                emit_local_init_elem(e->lhs, et, elem_off);
        }
        return;
    }

    if (type->kind == TY_STRUCT || type->kind == TY_UNION) {
        /* Zero the struct first */
        emit("  leaq  %d(%%rbp), %%rdi", base_offset);
        emit("  movl  $%d, %%ecx", type_size(type));
        emit("  xorl  %%eax, %%eax");
        emit("  rep stosb");

        int seq = 0;
        for (struct Node *e = list->stmts; e; e = e->next) {
            /* Find member */
            struct Member *m = NULL;
            if (e->name) {
                for (struct Member *mb = type->members; mb; mb = mb->next)
                    if (mb->name && strcmp(mb->name, e->name) == 0) { m = mb; break; }
            } else {
                int idx = 0;
                for (struct Member *mb = type->members; mb; mb = mb->next) {
                    if (!mb->name) continue;
                    if (idx++ == seq) { m = mb; break; }
                }
            }
            if (!m) { seq++; continue; }
            int moff = base_offset + m->offset;
            if (e->lhs && e->lhs->kind == ND_INIT_LIST)
                emit_local_init_list(e->lhs, m->type, moff);
            else
                emit_local_init_elem(e->lhs, m->type, moff);
            seq++;
        }
        return;
    }

    /* scalar wrapped in braces: use first element */
    struct Node *first = list->stmts ? list->stmts->lhs : NULL;
    emit_local_init_elem(first, type, base_offset);
}

static void emit_local_init_elem(struct Node *val, struct Type *type, int base_offset) {
    if (!val) return;   /* zero already written by rep stosb */
    int sz = type_size(type);
    if (is_float_type(type)) {
        gen_expr(val);
        if (type->kind == TY_FLOAT)
            emit("  movss %%xmm0, %d(%%rbp)", base_offset);
        else
            emit("  movsd %%xmm0, %d(%%rbp)", base_offset);
        return;
    }
    gen_expr(val);
    if      (sz == 8) emit("  movq  %%rax, %d(%%rbp)", base_offset);
    else if (sz == 4) emit("  movl  %%eax, %d(%%rbp)", base_offset);
    else if (sz == 2) emit("  movw  %%ax,  %d(%%rbp)", base_offset);
    else              emit("  movb  %%al,  %d(%%rbp)", base_offset);
}

static void gen_stmt(struct Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK:
            for (struct Node *s = n->stmts; s; s = s->next) gen_stmt(s);
            return;
        case ND_EXPR_STMT:
            gen_expr(n->lhs);
            return;
        case ND_DECL:
            if (n->init) {
                if (n->init->kind == ND_INIT_LIST) {
                    /* brace-initializer: emit element-by-element stores into the stack slot */
                    emit_local_init_list(n->init, n->type, n->offset);
                } else if (is_struct_type(n->type)) {
                    /* struct init from call or lvalue: memcpy into slot */
                    int sz = type_size(n->type);
                    bool init_is_call = (n->init->kind == ND_CALL);
                    if (init_is_call) {
                        gen_expr(n->init);
                        emit("  movq  %%rax, %%rsi");
                    } else {
                        gen_addr(n->init);
                        emit("  movq  %%rax, %%rsi");
                    }
                    emit("  leaq  %d(%%rbp), %%rdi", n->offset);
                    emit_memcpy("rdi", "rsi", sz);
                    if (init_is_call && n->init->ival > 0)
                        emit("  addq $%lld, %%rsp", n->init->ival);
                } else {
                    gen_expr(n->init);
                    int sz = n->type ? type_size(n->type) : 8;
                    if (is_float_type(n->type)) {
                        if (n->type->kind == TY_FLOAT)
                            emit("  movss %%xmm0, %d(%%rbp)", n->offset);
                        else
                            emit("  movsd %%xmm0, %d(%%rbp)", n->offset);
                    } else if (sz == 8)
                        emit("  movq  %%rax, %d(%%rbp)", n->offset);
                    else if (sz == 4)
                        emit("  movl  %%eax, %d(%%rbp)", n->offset);
                    else if (sz == 2)
                        emit("  movw  %%ax,  %d(%%rbp)", n->offset);
                    else
                        emit("  movb  %%al,  %d(%%rbp)", n->offset);
                }
            }
            return;
        case ND_RETURN:
            if (n->lhs) {
                struct Type *rt = current_func_ret_type;
                if (struct_needs_sret(rt)) {
                    /* large struct: copy return value into the hidden pointer */
                    int sz = type_size(rt);
                    gen_addr(n->lhs);
                    emit("  movq  %%rax, %%rsi");          /* source address */
                    emit("  movq  %d(%%rbp), %%rdi", current_func_sret_offset);
                    emit_memcpy("rdi", "rsi", sz);
                } else if (struct_fits_regs(rt)) {
                    /* small struct: load up to 16 bytes into rax[:rdx] */
                    int sz = type_size(rt);
                    gen_addr(n->lhs);                       /* rax = source addr */
                    emit("  movq  %%rax, %%rcx");           /* rcx = addr */
                    emit("  movq  (%%rcx), %%rax");         /* low 8 bytes */
                    if (sz > 8)
                        emit("  movq  8(%%rcx), %%rdx");    /* high bytes */
                } else {
                    gen_expr(n->lhs);
                }
            }
            emit("  jmp %s", current_func_end);
            return;
        case ND_IF: {
            int lbl = new_label();
            gen_expr(n->cond);
            emit("  cmpq $0, %%rax");
            if (n->els) {
                emit("  je .Lelse%d", lbl);
                gen_stmt(n->then);
                emit("  jmp .Lend%d", lbl);
                emit(".Lelse%d:", lbl);
                gen_stmt(n->els);
            } else {
                emit("  je .Lend%d", lbl);
                gen_stmt(n->then);
            }
            emit(".Lend%d:", lbl);
            return;
        }
        case ND_WHILE: {
            int lbl = new_label();
            char *old_brk = break_label, *old_cont = continue_label;
            char buf_brk[32], buf_cont[32];
            sprintf(buf_brk,  ".Lend%d",  lbl);
            sprintf(buf_cont, ".Lloop%d", lbl);
            break_label    = arena_strdup(buf_brk);
            continue_label = arena_strdup(buf_cont);
            emit(".Lloop%d:", lbl);
            gen_expr(n->cond);
            emit("  cmpq $0, %%rax");
            emit("  je .Lend%d", lbl);
            gen_stmt(n->body);
            emit("  jmp .Lloop%d", lbl);
            emit(".Lend%d:", lbl);
            break_label = old_brk; continue_label = old_cont;
            return;
        }
        case ND_DO_WHILE: {
            int lbl = new_label();
            char *old_brk = break_label, *old_cont = continue_label;
            char buf_brk[32], buf_cont[32];
            sprintf(buf_brk,  ".Lend%d",  lbl);
            sprintf(buf_cont, ".Lcont%d", lbl);
            break_label    = arena_strdup(buf_brk);
            continue_label = arena_strdup(buf_cont);
            emit(".Ldo%d:", lbl);
            gen_stmt(n->body);
            emit(".Lcont%d:", lbl);
            gen_expr(n->cond);
            emit("  cmpq $0, %%rax");
            emit("  jne .Ldo%d", lbl);
            emit(".Lend%d:", lbl);
            break_label = old_brk; continue_label = old_cont;
            return;
        }
        case ND_FOR: {
            int lbl = new_label();
            char *old_brk = break_label, *old_cont = continue_label;
            char buf_brk[32], buf_cont[32];
            sprintf(buf_brk,  ".Lend%d",  lbl);
            sprintf(buf_cont, ".Lstep%d", lbl);
            break_label    = arena_strdup(buf_brk);
            continue_label = arena_strdup(buf_cont);
            if (n->init) gen_expr(n->init);
            emit(".Lfor%d:", lbl);
            if (n->cond) {
                gen_expr(n->cond);
                emit("  cmpq $0, %%rax");
                emit("  je .Lend%d", lbl);
            }
            gen_stmt(n->body);
            emit(".Lstep%d:", lbl);
            if (n->step) gen_expr(n->step);
            emit("  jmp .Lfor%d", lbl);
            emit(".Lend%d:", lbl);
            break_label = old_brk; continue_label = old_cont;
            return;
        }
        case ND_BREAK:
            if (!break_label) die("break outside loop");
            emit("  jmp %s", break_label);
            return;
        case ND_CONTINUE:
            if (!continue_label) die("continue outside loop");
            emit("  jmp %s", continue_label);
            return;
        case ND_GOTO:
            emit("  jmp .L%s", n->name);
            return;
        case ND_LABEL:
            emit(".L%s:", n->name);
            gen_stmt(n->body);
            return;
        default:
            die("unhandled statement kind %d", n->kind);
    }
}

void gen_func(struct Node *fn) {
    char end_label[64];
    sprintf(end_label, ".Lret_%s", fn->name);
    current_func_end        = arena_strdup(end_label);
    current_func_ret_type   = fn->type;   /* fn->type holds the return type */
    current_func_sret_offset = 0;

    emit("  .text");
    emit("  .globl %s", fn->name);
    emit("%s:", fn->name);

    /* prologue */
    emit("  pushq %%rbp");
    emit("  movq %%rsp, %%rbp");
    int frame = (int)fn->ival;  /* set by resolve_func */
    if (frame > 0)
        emit("  subq $%d, %%rsp", frame);

    /* move register args to stack slots */
    int int_idx = 0, xmm_idx = 0;

    /* sret: if returning a large struct, the hidden pointer arrives in rdi
       (the first integer register). Spill it to a dedicated stack slot so we
       can always find it at epilogue time. resolve_func already reserved space
       for it (it is recorded as a synthetic param at the front of the list
       -- but we handle it here manually to keep the resolver simple). */
    if (struct_needs_sret(fn->type)) {
        /* The hidden slot offset was stored in current_func_sret_offset by
           resolve_func. We just need to spill rdi now. */
        current_func_sret_offset = fn->sret_offset;
        emit("  movq  %%rdi, %d(%%rbp)", current_func_sret_offset);
        int_idx = 1;   /* regular params start at rsi */
    }

    for (struct Node *p = fn->params; p; p = p->next) {
        if (is_float_type(p->type)) {
            if (xmm_idx >= 8) break;
            if (p->type->kind == TY_FLOAT)
                emit("  movss %%xmm%d, %d(%%rbp)", xmm_idx, p->offset);
            else
                emit("  movsd %%xmm%d, %d(%%rbp)", xmm_idx, p->offset);
            xmm_idx++;
        } else if (struct_fits_regs(p->type)) {
            /* small struct: arrives in 1 or 2 integer registers, spill each eightbyte */
            int sz = type_size(p->type);
            if (int_idx >= 6) break;
            emit("  movq  %%%s, %d(%%rbp)", arg_regs[int_idx], p->offset);
            int_idx++;
            if (sz > 8) {
                if (int_idx >= 6) break;
                emit("  movq  %%%s, %d(%%rbp)", arg_regs[int_idx], p->offset + 8);
                int_idx++;
            }
        } else if (struct_needs_sret(p->type)) {
            /* large struct: arrives as a pointer to a caller-allocated copy.
               Spill the pointer into a temporary, then memcpy into our local slot. */
            if (int_idx >= 6) break;
            int sz = type_size(p->type);
            /* use r11 as scratch to hold the incoming pointer */
            emit("  movq  %%%s, %%r11", arg_regs[int_idx]);
            int_idx++;
            /* destination = our local stack slot */
            emit("  leaq  %d(%%rbp), %%rdi", p->offset);
            emit("  movq  %%r11, %%rsi");
            emit_memcpy("rdi", "rsi", sz);
        } else {
            if (int_idx >= 6) break;
            int sz = p->type ? type_size(p->type) : 8;
            if (sz == 8)
                emit("  movq  %%%s, %d(%%rbp)", arg_regs[int_idx],   p->offset);
            else if (sz == 4)
                emit("  movl  %%%s, %d(%%rbp)", arg_regs32[int_idx], p->offset);
            else if (sz == 2)
                emit("  movw  %%%s, %d(%%rbp)", arg_regs16[int_idx], p->offset);
            else
                emit("  movb  %%%s, %d(%%rbp)", arg_regs8[int_idx],  p->offset);
            int_idx++;
        }
    }

    gen_stmt(fn->body);

    /* epilogue */
    emit("%s:", current_func_end);
    /* For large-struct sret: reload the hidden pointer into rax (ABI requires it) */
    if (struct_needs_sret(fn->type)) {
        emit("  movq  %d(%%rbp), %%rax", current_func_sret_offset);
    }
    emit("  movq %%rbp, %%rsp");
    emit("  popq %%rbp");
    emit("  ret");
}

/* ------------------------------------------------------------------ */
/* Static initializer emission helpers                                  */
/* ------------------------------------------------------------------ */

/* Emit bytes for a single scalar constant of the given type. */
static void emit_scalar_init(struct Node *val, struct Type *type) {
    int sz = type_size(type);
    if (!val) {
        emit("  .zero %d", sz);
        return;
    }
    if (val->kind == ND_FLOAT || (val->kind == ND_CAST && is_float_type(val->type))) {
        double fv = val->fval;
        if (sz == 4) {
            union { float f; uint32_t u; } u; u.f = (float)fv;
            emit("  .long %u", u.u);
        } else {
            union { double d; uint64_t u; } u; u.d = fv;
            emit("  .quad %llu", (unsigned long long)u.u);
        }
        return;
    }
    if (val->kind == ND_STR) {
        /* pointer to string literal: emit label in .quad, string in .rodata */
        emit("  .quad .LC_str_%p", (void*)val->sval);
        emit("  .section .rodata");
        emit(".LC_str_%p:", (void*)val->sval);
        emit_string_literal(val->sval);
        emit("  .data");
        return;
    }
    long long iv = (val->kind == ND_INT) ? val->ival : 0;
    switch (sz) {
        case 1: emit("  .byte %lld",  iv); break;
        case 2: emit("  .short %lld", iv); break;
        case 4: emit("  .long %lld",  iv); break;
        default:emit("  .quad %lld",  iv); break;
    }
}

/* Forward declaration */
static void emit_init(struct Node *init, struct Type *type);

/* Emit an array initializer list into consecutive bytes. */
static void emit_array_init(struct Node *list, struct Type *arr_type) {
    int count   = arr_type->array_size;    /* 0 = unbounded (flexible) */
    struct Type *et = arr_type->base;
    int esz     = type_size(et);

    /* Build a flat array of initializer pointers, indexed by position. */
    /* Use a simple pass: walk the element list and fill by index.       */
    /* Max 65536 elements for static data. */
    int max_idx = 0;
    for (struct Node *e = list ? list->stmts : NULL; e; e = e->next) {
        int idx = (int)e->ival;   /* set by parser */
        if (idx > max_idx) max_idx = idx;
    }
    int n_elems = (count > 0) ? count : max_idx + 1;

    /* Allocate a zero-initialized pointer array in the arena */
    struct Node **elems = arena_alloc(n_elems * sizeof(struct Node *));

    for (struct Node *e = list ? list->stmts : NULL; e; e = e->next) {
        int idx = (int)e->ival;
        if (idx >= 0 && idx < n_elems)
            elems[idx] = e->lhs;
    }

    for (int i = 0; i < n_elems; i++) {
        if (elems[i])
            emit_init(elems[i], et);
        else
            emit("  .zero %d", esz);
    }
}

/* Emit a struct initializer list into consecutive member bytes. */
static void emit_struct_init(struct Node *list, struct Type *st_type) {
    int written = 0;   /* bytes emitted so far (for padding) */
    struct Member *m = st_type->members;
    struct Node   *e = list ? list->stmts : NULL;

    for (; m; m = m->next) {
        if (!m->name) continue;   /* unnamed padding member */

        /* Emit padding before this member */
        if (m->offset > written) {
            emit("  .zero %d", m->offset - written);
            written = m->offset;
        }

        /* Find the matching initializer element */
        struct Node *val = NULL;
        /* Designated: search by name */
        for (struct Node *ie = list ? list->stmts : NULL; ie; ie = ie->next) {
            if (ie->name && strcmp(ie->name, m->name) == 0) {
                val = ie->lhs; break;
            }
        }
        /* Sequential: use current e if it has no designator */
        if (!val && e && !e->name && e->ival < 0) {
            val = e->lhs;
            e = e->next;
        } else if (!val && e && !e->name) {
            val = e->lhs;
            e = e->next;
        }

        int msz = type_size(m->type);
        if (val)
            emit_init(val, m->type);
        else
            emit("  .zero %d", msz);
        written += msz;
    }

    /* Trailing padding to struct total size */
    int total = type_size(st_type);
    if (total > written)
        emit("  .zero %d", total - written);
}

/* Dispatch to the right emitter based on type. */
static void emit_init(struct Node *init, struct Type *type) {
    if (!init) { emit("  .zero %d", type_size(type)); return; }

    if (init->kind == ND_INIT_LIST) {
        if (type->kind == TY_ARRAY)
            emit_array_init(init, type);
        else if (type->kind == TY_STRUCT || type->kind == TY_UNION)
            emit_struct_init(init, type);
        else
            /* brace around scalar: just use the first element */
            emit_init(init->stmts ? init->stmts->lhs : NULL, type);
        return;
    }
    /* scalar */
    emit_scalar_init(init, type);
}

/* ------------------------------------------------------------------ */
/* gen_gvar                                                             */
/* ------------------------------------------------------------------ */

void gen_gvar(struct Node *gv) {
    if (gv->init) {
        emit("  .data");
        emit("  .globl %s", gv->name);
        emit("%s:", gv->name);
        emit_init(gv->init, gv->type);
    } else {
        emit("  .bss");
        emit("  .globl %s", gv->name);
        emit("%s:", gv->name);
        emit("  .zero %d", type_size(gv->type));
    }
}

