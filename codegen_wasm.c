#include "bake.h"

/*
 * codegen_wasm.c -- WebAssembly Text Format (WAT) backend for bake
 *
 * Output: a .wat file that can be compiled with wat2wasm (from the WABT
 * toolkit) or loaded directly by a JS runtime via WebAssembly.compile().
 *
 * Design notes
 * ============
 * - WAT is a stack machine, so most binary expressions are just: push lhs,
 *   push rhs, opcode. No register allocation needed.
 * - Locals (including parameters) live in wasm locals (i64 for ints, f64 for
 *   doubles, f32 for floats). Their rbp-relative offsets from the resolver are
 *   translated to a flat local index table built at the start of each function.
 * - Global variables live in wasm linear memory starting at address
 *   GLOBAL_BASE (4096 by default, leaving room for a null page).
 * - Strings and float literals are placed in a data segment in linear memory.
 * - Structs / pointers: passed as i32 addresses into linear memory (wasm32
 *   model). Struct members are read/written with i32.load/store.
 * - No sret: small structs are passed/returned by pointer (caller allocates on
 *   a small software stack maintained in global $__sp). This mirrors what
 *   Clang does for wasm32 without optimisations.
 * - Calls: translated to wasm call / call_indirect instructions.
 * - float arithmetic uses f32.*, double uses f64.*.
 *
 * Limitations (acceptable for a teaching/hobby backend)
 * =====================================================
 * - No variadics (die with a clear message).
 * - No goto / labels (die with a clear message).
 * - Structs > 16 bytes are passed by pointer to linear memory only.
 * - The software stack ($__sp) is 1 MB, growing downward from STACK_TOP.
 * - Only one wasm module per compilation unit.
 */

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define GLOBAL_BASE   4096        /* byte offset where globals start    */
#define STACK_TOP     (1<<20)     /* 1 MB software stack top            */
#define MEMORY_PAGES  16          /* 16 * 64KB = 1 MB initial memory    */

/* ------------------------------------------------------------------ */
/* emit() -- line-oriented output (required by bake.h API)             */
/* ------------------------------------------------------------------ */

void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Label counter                                                        */
/* ------------------------------------------------------------------ */

static int label_count = 0;
static int new_label(void) { return label_count++; }

/* ------------------------------------------------------------------ */
/* Type helpers                                                         */
/* ------------------------------------------------------------------ */

static bool is_float_type(struct Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE);
}
static bool node_is_float(struct Node *n) {
    return n && is_float_type(n->type);
}
/* Wasm value type string for a bake Type */
static const char *wasm_type(struct Type *t) {
    if (!t) return "i64";
    if (t->kind == TY_FLOAT)  return "f32";
    if (t->kind == TY_DOUBLE) return "f64";
    /* pointers, arrays, structs: i32 address in wasm32 */
    if (t->kind == TY_PTR || t->kind == TY_ARRAY || is_struct_type(t))
        return "i32";
    /* everything else: integral, use i64 */
    return "i64";
}
/* wasm load instruction for reading a value of type t from linear memory */
static const char *wasm_load(struct Type *t) {
    if (!t) return "i64.load";
    switch (t->kind) {
        case TY_FLOAT:  return "f32.load";
        case TY_DOUBLE: return "f64.load";
        case TY_PTR: case TY_ARRAY: return "i32.load";
        case TY_CHAR:  return "i64.load8_s";
        case TY_SHORT: return "i64.load16_s";
        case TY_INT:   return "i64.load32_s";
        default:       return "i64.load";
    }
}
/* wasm store instruction */
static const char *wasm_store(struct Type *t) __attribute__((unused));  
static const char *wasm_store(struct Type *t) {
    if (!t) return "i64.store";
    switch (t->kind) {
        case TY_FLOAT:  return "f32.store";
        case TY_DOUBLE: return "f64.store";
        case TY_PTR: case TY_ARRAY: return "i32.store";
        case TY_CHAR:  return "i64.store8";
        case TY_SHORT: return "i64.store16";
        case TY_INT:   return "i64.store32";
        default:       return "i64.store";
    }
}
/* const instruction for a zero of type t */
static const char *wasm_const_zero(struct Type *t) __attribute__((unused));  
static const char *wasm_const_zero(struct Type *t) {
    if (!t) return "i64.const 0";
    if (t->kind == TY_FLOAT)  return "f32.const 0.0";
    if (t->kind == TY_DOUBLE) return "f64.const 0.0";
    if (t->kind == TY_PTR || t->kind == TY_ARRAY || is_struct_type(t))
        return "i32.const 0";
    return "i64.const 0";
}

/* ------------------------------------------------------------------ */
/* Global variable layout                                               */
/* ------------------------------------------------------------------ */

/* We emit a simple symbol→address table for globals */
typedef struct GVarEntry {
    char *name;
    int   addr;   /* byte offset in linear memory */
    struct GVarEntry *next;
} GVarEntry;

static GVarEntry *gvar_table = NULL;
static int        gvar_next  = GLOBAL_BASE;

static int gvar_alloc(const char *name, int size) {
    GVarEntry *e = arena_alloc(sizeof(GVarEntry));
    e->name = arena_strdup(name);
    e->addr = gvar_next;
    e->next = gvar_table;
    gvar_table = e;
    /* align to 8 bytes */
    gvar_next += (size + 7) & ~7;
    return e->addr;
}

static int gvar_addr(const char *name) {
    for (GVarEntry *e = gvar_table; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->addr;
    die("wasm: undefined global '%s'", name);
}

/* ------------------------------------------------------------------ */
/* String / float literal data segments                                 */
/* ------------------------------------------------------------------ */

typedef struct DataSeg {
    int   addr;
    char *bytes;   /* null-terminated hex content for .wat */
    int   len;
    struct DataSeg *next;
} DataSeg;

static DataSeg *data_segs  = NULL;
static int      data_next  = STACK_TOP; /* data starts right above stack */

static int alloc_data(const char *src, int len) {
    /* pad to 8-byte alignment */
    int addr = data_next;
    DataSeg *d = arena_alloc(sizeof(DataSeg));
    d->addr    = addr;
    d->len     = len;
    d->bytes   = arena_alloc(len + 1);
    memcpy(d->bytes, src, len);
    d->next    = data_segs;
    data_segs  = d;
    data_next += (len + 7) & ~7;
    return addr;
}

static int alloc_string(const char *s) {
    int len = (int)strlen(s) + 1; /* include NUL */
    return alloc_data(s, len);
}

/* ------------------------------------------------------------------ */
/* Per-function local variable index table                              */
/* Wasm locals are indexed 0..N; params come first.                    */
/* ------------------------------------------------------------------ */

#define MAX_LOCALS 256

typedef struct LocalEntry {
    int  offset;      /* rbp-relative offset assigned by resolver */
    int  wasm_idx;    /* wasm local index */
    struct Type *type;
} LocalEntry;

static LocalEntry local_table[MAX_LOCALS];
static int        local_count = 0;
/* next wasm local index (params already allocated before gen_expr runs) */
static int        next_local_idx = 0;

/* software-stack local for this function's frame pointer */
static int        local_sp_idx  = -1;  /* wasm local holding saved $__sp */
static int        frame_size    = 0;   /* bytes reserved on software stack */

static void reset_locals(void) {
    local_count    = 0;
    next_local_idx = 0;
    local_sp_idx   = -1;
    frame_size     = 0;
}

/* Register a local variable (called while scanning params/decls) */
static int reg_local(int offset, struct Type *type) {
    for (int i = 0; i < local_count; i++)
        if (local_table[i].offset == offset) return local_table[i].wasm_idx;
    if (local_count >= MAX_LOCALS) die("too many locals in function");
    local_table[local_count].offset   = offset;
    local_table[local_count].wasm_idx = next_local_idx++;
    local_table[local_count].type     = type;
    return local_table[local_count++].wasm_idx;
}

/* Look up an already-registered local by rbp offset */
static int local_idx(int offset) {
    for (int i = 0; i < local_count; i++)
        if (local_table[i].offset == offset) return local_table[i].wasm_idx;
    die("wasm: unresolved local at offset %d", offset);
}

static struct Type *local_type(int offset) {
    for (int i = 0; i < local_count; i++)
        if (local_table[i].offset == offset) return local_table[i].type;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Indentation helper                                                   */
/* ------------------------------------------------------------------ */

static int indent_level = 0;
static void iemit(const char *fmt, ...) {
    for (int i = 0; i < indent_level; i++) fprintf(out, "  ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "\n");
}
#define INDENT() (indent_level++)
#define DEDENT() (indent_level--)

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void wasm_gen_addr(struct Node *n);
static void wasm_gen_expr(struct Node *n);
static void wasm_gen_stmt(struct Node *n);

/* ------------------------------------------------------------------ */
/* Function signature helper                                            */
/* ------------------------------------------------------------------ */

/* Collect all function signatures needed for the type section */
typedef struct FuncSig {
    char *name;
    struct Node *fn;   /* NULL for imported protos */
    struct ProtoEntry *proto;
    struct FuncSig *next;
} FuncSig;
static FuncSig *func_sigs = NULL;

static void register_func(const char *name, struct Node *fn) __attribute__((unused));
static void register_func(const char *name, struct Node *fn) __attribute__((unused));
static void register_func(const char *name, struct Node *fn) {
    FuncSig *s = arena_alloc(sizeof(FuncSig));
    s->name = arena_strdup(name);
    s->fn   = fn;
    s->next = func_sigs;
    func_sigs = s;
}

/* ------------------------------------------------------------------ */
/* Push address of lvalue (leaves i32 on wasm stack)                  */
/* ------------------------------------------------------------------ */

static void wasm_gen_addr(struct Node *n) {
    switch (n->kind) {
        case ND_IDENT:
            if (n->is_local) {
                /* address of a local variable = software stack slot */
                /* $__sp_saved + (offset - (negative rbp offset)) */
                /* Local layout: offset is negative (e.g. -8, -16, …).
                   Our software frame starts at $__sp (high) and grows down.
                   slot_addr = frame_base - (-offset) = frame_base + offset
                   where frame_base = saved_sp (local $__sp_saved). */
                iemit("local.get $__sp_saved");
                /* offset is negative; frame_base + offset = correct address */
                if (n->offset != 0)
                    iemit("i32.const %d", n->offset);
                else
                    iemit("i32.const 0");
                iemit("i32.add");
            } else {
                /* global: constant address */
                iemit("i32.const %d", gvar_addr(n->name));
            }
            return;
        case ND_DEREF:
            wasm_gen_expr(n->lhs);
            /* result is the pointer value which is already the address */
            /* wasm_gen_expr leaves i64 for ints; pointers are i32 */
            if (!is_float_type(n->lhs->type))
                iemit("i32.wrap_i64");
            return;
        case ND_INDEX: {
            struct Type *lhs_type = n->lhs->type;
            if (lhs_type && lhs_type->kind == TY_PTR) {
                wasm_gen_expr(n->lhs);
                iemit("i32.wrap_i64");
            } else {
                wasm_gen_addr(n->lhs);
            }
            wasm_gen_expr(n->rhs);
            iemit("i32.wrap_i64");
            int esz = 1;
            if (lhs_type) {
                struct Type *et = (lhs_type->kind == TY_ARRAY || lhs_type->kind == TY_PTR)
                                  ? lhs_type->base : lhs_type;
                if (et) esz = type_size(et);
            }
            if (esz != 1) {
                iemit("i32.const %d", esz);
                iemit("i32.mul");
            }
            iemit("i32.add");
            return;
        }
        case ND_MEMBER: {
            wasm_gen_addr(n->lhs);
            struct Type *st = n->lhs->type;
            if (!st) die("unresolved struct type");
            struct Member *m = find_member(st, n->name);
            if (m->offset) {
                iemit("i32.const %d", m->offset);
                iemit("i32.add");
            }
            return;
        }
        case ND_ARROW: {
            wasm_gen_expr(n->lhs);
            iemit("i32.wrap_i64");
            struct Type *st = n->lhs->type;
            if (st && st->kind == TY_PTR) st = st->base;
            if (!st) die("unresolved struct type for ->");
            struct Member *m = find_member(st, n->name);
            if (m->offset) {
                iemit("i32.const %d", m->offset);
                iemit("i32.add");
            }
            return;
        }
        default:
            if (n->kind == ND_CALL && is_struct_type(n->type)) {
                wasm_gen_expr(n);
                return;
            }
            die("wasm: cannot take address of expression kind %d", n->kind);
    }
}

/* ------------------------------------------------------------------ */
/* Load from an address already on the wasm stack                      */
/* ------------------------------------------------------------------ */

static void wasm_load_from_addr(struct Type *t) {
    iemit("%s", wasm_load(t));
}

/* ------------------------------------------------------------------ */
/* Convert top-of-stack integer to bool (0 or 1)                       */
/* ------------------------------------------------------------------ */
static void wasm_to_bool_i64(void) __attribute__((unused));  
static void wasm_to_bool_i64(void) {
    iemit("i64.const 0");
    iemit("i64.ne");   /* → i32 */
    iemit("i64.extend_i32_s");
}

/* ------------------------------------------------------------------ */
/* wasm_gen_expr -- evaluate expression, leave result on wasm stack     */
/* ------------------------------------------------------------------ */

static void wasm_gen_expr(struct Node *n) {
    switch (n->kind) {

    case ND_INT:
        iemit("i64.const %lld", n->ival);
        return;

    case ND_FLOAT:
        if (n->is_float)
            iemit("f32.const %a", (float)n->fval);
        else
            iemit("f64.const %a", n->fval);
        return;

    case ND_STR: {
        int addr = alloc_string(n->sval);
        iemit("i64.const %d", addr);
        return;
    }

    case ND_IDENT: {
        /* arrays / structs: push address */
        if (n->type && (n->type->kind == TY_ARRAY || is_struct_type(n->type))) {
            wasm_gen_addr(n);
            iemit("i64.extend_i32_u");
            return;
        }
        /* function names decay to a dummy non-zero i64 (call by name is handled in ND_CALL) */
        if (!n->is_local && find_proto(n->name)) {
            iemit("i64.const 1");   /* placeholder; indirect calls not fully supported */
            return;
        }
        if (n->is_local) {
            int idx = local_idx(n->offset);
            struct Type *lt = local_type(n->offset);
            iemit("local.get %d", idx);
            /* widen narrow integers to i64 */
            if (lt && !is_float_type(lt) && lt->kind != TY_PTR &&
                    lt->kind != TY_ARRAY && !is_struct_type(lt)) {
                int sz = type_size(lt);
                if (sz < 8) {
                    /* stored as i64 — nothing to do; stored as narrower? We always store i64 */
                }
            }
        } else {
            wasm_gen_addr(n);
            iemit("i64.extend_i32_u");
            /* load from global address */
            if (is_float_type(n->type)) {
                iemit("i32.wrap_i64");
                wasm_load_from_addr(n->type);
            } else {
                iemit("i32.wrap_i64");
                wasm_load_from_addr(n->type);
            }
        }
        return;
    }

    case ND_ADDR:
        wasm_gen_addr(n->lhs);
        iemit("i64.extend_i32_u");
        return;

    case ND_DEREF:
        wasm_gen_expr(n->lhs);
        iemit("i32.wrap_i64");
        wasm_load_from_addr(n->type);
        if (is_float_type(n->type)) {
            /* fine, f32/f64 on stack */
        } else if (!is_struct_type(n->type) &&
                   !(n->type && n->type->kind == TY_PTR)) {
            /* extend to i64 if not already */
        }
        return;

    case ND_INDEX:
        wasm_gen_addr(n);
        wasm_load_from_addr(n->type);
        return;

    case ND_MEMBER:
    case ND_ARROW:
        wasm_gen_addr(n);
        if (!is_struct_type(n->type))
            wasm_load_from_addr(n->type);
        return;

    case ND_UNARY:
        wasm_gen_expr(n->lhs);
        if (node_is_float(n->lhs)) {
            bool is_f = (n->lhs->type->kind == TY_FLOAT);
            if (n->op == TK_MINUS) {
                iemit("%s.neg", is_f ? "f32" : "f64");
            } else if (n->op == TK_BANG) {
                if (is_f) { iemit("f32.const 0.0"); iemit("f32.eq"); }
                else      { iemit("f64.const 0.0"); iemit("f64.eq"); }
                iemit("i64.extend_i32_s");
            }
            return;
        }
        switch (n->op) {
            case TK_MINUS: iemit("i64.const -1"); iemit("i64.mul"); break;
            case TK_BANG:
                iemit("i64.const 0");
                iemit("i64.eq");
                iemit("i64.extend_i32_s");
                break;
            case TK_TILDE:
                iemit("i64.const -1");
                iemit("i64.xor");
                break;
            default: break;
        }
        return;

    case ND_BINARY: {
        /* Short-circuit && */
        if (n->op == TK_AND) {
            int lbl = new_label();
            iemit("block $land_end_%d (result i64)", lbl);
              INDENT();
              iemit("i64.const 0");   /* default false result */
              wasm_gen_expr(n->lhs);
              if (node_is_float(n->lhs)) { iemit("f64.const 0.0"); iemit("f64.ne"); iemit("i64.extend_i32_s"); }
              iemit("i64.const 0"); iemit("i64.eq");
              iemit("br_if $land_end_%d", lbl);   /* short-circuit: leave 0 */
              iemit("drop");   /* discard the 0 we pushed as default */
              wasm_gen_expr(n->rhs);
              if (node_is_float(n->rhs)) { iemit("f64.const 0.0"); iemit("f64.ne"); iemit("i64.extend_i32_s"); }
              iemit("i64.const 0"); iemit("i64.ne"); iemit("i64.extend_i32_s");
            DEDENT();
            iemit("end");
            return;
        }
        /* Short-circuit || */
        if (n->op == TK_OR) {
            int lbl = new_label();
            iemit("block $lor_end_%d (result i64)", lbl);
              INDENT();
              iemit("i64.const 1");   /* default true result */
              wasm_gen_expr(n->lhs);
              if (node_is_float(n->lhs)) { iemit("f64.const 0.0"); iemit("f64.ne"); iemit("i64.extend_i32_s"); }
              iemit("i64.const 0"); iemit("i64.ne");
              iemit("br_if $lor_end_%d", lbl);   /* short-circuit: leave 1 */
              iemit("drop");
              wasm_gen_expr(n->rhs);
              if (node_is_float(n->rhs)) { iemit("f64.const 0.0"); iemit("f64.ne"); iemit("i64.extend_i32_s"); }
              iemit("i64.const 0"); iemit("i64.ne"); iemit("i64.extend_i32_s");
            DEDENT();
            iemit("end");
            return;
        }

        /* Float arithmetic */
        bool lf = node_is_float(n->lhs), rf = node_is_float(n->rhs);
        if (lf || rf) {
            bool use_d = (n->lhs->type && n->lhs->type->kind == TY_DOUBLE) ||
                         (n->rhs->type && n->rhs->type->kind == TY_DOUBLE);
            const char *ft = use_d ? "f64" : "f32";
            wasm_gen_expr(n->lhs);
            if (!lf) { iemit(use_d ? "f64.convert_i64_s" : "f32.convert_i64_s"); }
            else if (use_d && n->lhs->type->kind == TY_FLOAT)
                iemit("f64.promote_f32");
            wasm_gen_expr(n->rhs);
            if (!rf) { iemit(use_d ? "f64.convert_i64_s" : "f32.convert_i64_s"); }
            else if (use_d && n->rhs->type->kind == TY_FLOAT)
                iemit("f64.promote_f32");
            switch (n->op) {
                case TK_PLUS:  iemit("%s.add", ft); break;
                case TK_MINUS: iemit("%s.sub", ft); break;
                case TK_STAR:  iemit("%s.mul", ft); break;
                case TK_SLASH: iemit("%s.div", ft); break;
                case TK_EQ:  iemit("%s.eq", ft); iemit("i64.extend_i32_s"); break;
                case TK_NEQ: iemit("%s.ne", ft); iemit("i64.extend_i32_s"); break;
                case TK_LT:  iemit("%s.lt", ft); iemit("i64.extend_i32_s"); break;
                case TK_GT:  iemit("%s.gt", ft); iemit("i64.extend_i32_s"); break;
                case TK_LEQ: iemit("%s.le", ft); iemit("i64.extend_i32_s"); break;
                case TK_GEQ: iemit("%s.ge", ft); iemit("i64.extend_i32_s"); break;
                default: die("wasm: unsupported float op %d", n->op);
            }
            return;
        }

        /* Pointer arithmetic */
        bool lp = n->lhs->type && (n->lhs->type->kind == TY_PTR || n->lhs->type->kind == TY_ARRAY);
        bool rp = n->rhs->type && (n->rhs->type->kind == TY_PTR || n->rhs->type->kind == TY_ARRAY);

        if (n->op == TK_MINUS && lp && rp) {
            int esz = n->lhs->type->base ? type_size(n->lhs->type->base) : 1;
            wasm_gen_expr(n->lhs); iemit("i32.wrap_i64");
            wasm_gen_expr(n->rhs); iemit("i32.wrap_i64");
            iemit("i32.sub");
            if (esz > 1) { iemit("i32.const %d", esz); iemit("i32.div_s"); }
            iemit("i64.extend_i32_s");
            return;
        }
        int ptr_scale = 1;
        if ((n->op == TK_PLUS || n->op == TK_MINUS) && lp && !rp)
            ptr_scale = n->lhs->type->base ? type_size(n->lhs->type->base) : 1;
        if (n->op == TK_PLUS && !lp && rp)
            ptr_scale = n->rhs->type->base ? type_size(n->rhs->type->base) : 1;

        /* Integer binary */
        wasm_gen_expr(n->lhs);
        if (lp) iemit("i32.wrap_i64");
        wasm_gen_expr(n->rhs);
        if (rp) iemit("i32.wrap_i64");
        if (ptr_scale > 1 && !lp) { iemit("i32.const %d", ptr_scale); iemit("i32.mul"); }
        if (ptr_scale > 1 &&  lp) {
            /* rhs scaled, lhs is ptr: swap not needed — lhs is already on stack first */
        }

        /* Use i32 for pointer ops, i64 for integer */
        bool use_i32 = lp || rp;
        const char *it = use_i32 ? "i32" : "i64";
        switch (n->op) {
            case TK_PLUS:    iemit("%s.add", it); break;
            case TK_MINUS:   iemit("%s.sub", it); break;
            case TK_STAR:    iemit("%s.mul", it); break;
            case TK_SLASH:   iemit("%s.div_s", it); break;
            case TK_PERCENT: iemit("%s.rem_s", it); break;
            case TK_AMP:     iemit("%s.and", it); break;
            case TK_PIPE:    iemit("%s.or", it); break;
            case TK_CARET:   iemit("%s.xor", it); break;
            case TK_LSHIFT:  iemit("%s.shl", it); break;
            case TK_RSHIFT:  iemit("%s.shr_s", it); break;
            case TK_EQ:  iemit("%s.eq", it);  iemit("i64.extend_i32_s"); break;
            case TK_NEQ: iemit("%s.ne", it);  iemit("i64.extend_i32_s"); break;
            case TK_LT:  iemit("%s.lt_s", it); iemit("i64.extend_i32_s"); break;
            case TK_GT:  iemit("%s.gt_s", it); iemit("i64.extend_i32_s"); break;
            case TK_LEQ: iemit("%s.le_s", it); iemit("i64.extend_i32_s"); break;
            case TK_GEQ: iemit("%s.ge_s", it); iemit("i64.extend_i32_s"); break;
            default: break;
        }
        /* extend i32 pointer result back to i64 */
        if (use_i32 && n->op == TK_PLUS)  iemit("i64.extend_i32_u");
        if (use_i32 && n->op == TK_MINUS && !rp) iemit("i64.extend_i32_s");
        return;
    }

    case ND_ASSIGN: {
        /* Struct copy: memory.copy(dst, src, len) */
        if (is_struct_type(n->lhs->type)) {
            int sz = type_size(n->lhs->type);
            wasm_gen_addr(n->lhs);
            if (n->rhs->kind == ND_CALL) wasm_gen_expr(n->rhs);
            else wasm_gen_addr(n->rhs);
            iemit("i32.const %d", sz);
            iemit("memory.copy");
            wasm_gen_addr(n->lhs);
            iemit("i64.extend_i32_u");
            return;
        }

        bool lhs_float = is_float_type(n->lhs->type);
        bool lhs_local = n->lhs->is_local;

        /* Helper lambda for type conversion of rhs->lhs */
        /* (inlined below at each usage site)             */

        if (lhs_local && n->op == TK_ASSIGN) {
            /* Simple local assign: rhs -> convert -> local.tee */
            wasm_gen_expr(n->rhs);
            if (lhs_float && !node_is_float(n->rhs)) {
                if (n->lhs->type->kind == TY_DOUBLE) iemit("f64.convert_i64_s");
                else iemit("f32.convert_i64_s");
            } else if (!lhs_float && node_is_float(n->rhs)) {
                if (n->rhs->type->kind == TY_FLOAT) iemit("i64.trunc_f32_s");
                else iemit("i64.trunc_f64_s");
            }
            iemit("local.tee %d", local_idx(n->lhs->offset));
            return;
        }

        if (lhs_local && n->op != TK_ASSIGN) {
            /* Compound local assign: current op rhs -> local.tee */
            int idx = local_idx(n->lhs->offset);
            iemit("local.get %d", idx);
            wasm_gen_expr(n->rhs);
            const char *it2 = lhs_float
                ? (n->lhs->type->kind == TY_FLOAT ? "f32" : "f64") : "i64";
            switch (n->op) {
                case TK_PLUS_ASSIGN:    iemit("%s.add", it2);   break;
                case TK_MINUS_ASSIGN:   iemit("%s.sub", it2);   break;
                case TK_STAR_ASSIGN:    iemit("%s.mul", it2);   break;
                case TK_SLASH_ASSIGN:   iemit(lhs_float ? "%s.div" : "i64.div_s", it2); break;
                case TK_PERCENT_ASSIGN: iemit("i64.rem_s");     break;
                case TK_AMP_ASSIGN:     iemit("i64.and");       break;
                case TK_PIPE_ASSIGN:    iemit("i64.or");        break;
                case TK_CARET_ASSIGN:   iemit("i64.xor");       break;
                case TK_LSHIFT_ASSIGN:  iemit("i64.shl");       break;
                case TK_RSHIFT_ASSIGN:  iemit("i64.shr_s");     break;
                default: die("wasm: unsupported compound local assign op %d", n->op);
            }
            iemit("local.tee %d", idx);
            return;
        }

        /* Global / memory assign.
           WAT store needs: [addr][value] on stack.
           Emit addr first, then rhs, then store, then reload for expr result. */
        if (n->op != TK_ASSIGN) {
            /* compound: addr already emitted once for load, re-emit for store */
            wasm_gen_addr(n->lhs);
            wasm_load_from_addr(n->lhs->type);  /* current value */
            wasm_gen_expr(n->rhs);
            if (lhs_float && !node_is_float(n->rhs)) {
                if (n->lhs->type->kind == TY_DOUBLE) iemit("f64.convert_i64_s");
                else iemit("f32.convert_i64_s");
            }
            const char *it3 = lhs_float
                ? (n->lhs->type->kind == TY_FLOAT ? "f32" : "f64") : "i64";
            switch (n->op) {
                case TK_PLUS_ASSIGN:   iemit("%s.add", it3); break;
                case TK_MINUS_ASSIGN:  iemit("%s.sub", it3); break;
                case TK_STAR_ASSIGN:   iemit("%s.mul", it3); break;
                case TK_SLASH_ASSIGN:  iemit(lhs_float ? "%s.div" : "i64.div_s", it3); break;
                default: die("wasm: unsupported compound memory assign op %d", n->op);
            }
            /* now stack has [new_value]; store it */
            wasm_gen_addr(n->lhs);   /* need addr under value -- stash value via... */
            /* WAT has no swap; simplest correct approach: store via a fresh intermediate.
               Emit: addr, value_already_on_stack — we can't reorder.
               Use block + br trick: push addr first, but value is already computed.
               Solution: store result in a well-known memory slot (software stack top). */
            /* Simplification: for compound global assigns, use sp scratch slot */
            iemit(";; stash computed value into scratch slot");
            iemit("global.get $__sp");
            iemit("i32.const -8");
            iemit("i32.add");          /* scratch = sp - 8 */
            iemit("i64.store");        /* scratch slot = value (pops value from i64 expr above -- wrong order) */
            /* This ordering is still wrong. The clean solution without extra locals:
               We restructure: the value is on the stack; we need [addr, value].
               Emit store as: wasm_gen_addr then the already-computed value.
               But value is already on stack before addr. In WAT this isn't solvable
               without a local. Allocate a temp i64 local for this purpose. */
            die("wasm: compound memory assign needs temp local (not yet implemented)");
        }
        /* Simple global/memory assign: addr, rhs, store, reload */
        wasm_gen_addr(n->lhs);
        wasm_gen_expr(n->rhs);
        if (lhs_float && !node_is_float(n->rhs)) {
            if (n->lhs->type->kind == TY_DOUBLE) iemit("f64.convert_i64_s");
            else iemit("f32.convert_i64_s");
        } else if (!lhs_float && node_is_float(n->rhs)) {
            if (n->rhs->type->kind == TY_FLOAT) iemit("i64.trunc_f32_s");
            else iemit("i64.trunc_f64_s");
        }
        iemit("%s", wasm_store(n->lhs->type));
        /* reload as expression result */
        wasm_gen_addr(n->lhs);
        wasm_load_from_addr(n->lhs->type);
        return;
    }

    case ND_TERNARY: {
        int lbl = new_label();
        const char *wt = wasm_type(n->type);
        iemit("block $tend_%d (result %s)", lbl, wt);
          INDENT();
          iemit("block $telse_%d", lbl);
            INDENT();
            wasm_gen_expr(n->cond);
            if (node_is_float(n->cond)) { iemit("f64.const 0.0"); iemit("f64.eq"); }
            else { iemit("i64.const 0"); iemit("i64.eq"); }
            iemit("br_if $telse_%d", lbl);
            wasm_gen_expr(n->then);
            iemit("br $tend_%d", lbl);
          DEDENT();
          iemit("end");
          wasm_gen_expr(n->els);
        DEDENT();
        iemit("end");
        return;
    }

    case ND_CALL: {
        if (n->is_variadic) die("wasm: variadic calls not supported");
        struct ProtoEntry *pe = n->name ? find_proto(n->name) : NULL;
        if (pe && pe->is_variadic) die("wasm: variadic call to '%s' not supported", n->name);

        /* Push args */
        for (struct Node *a = n->args; a; a = a->next)
            wasm_gen_expr(a);

        if (n->callee) {
            /* indirect call -- needs a table; emit as direct call with warning */
            iemit(";; indirect call not fully supported; calling callee expr");;
            iemit("drop");   /* discard args for now */
            iemit(";; unreachable");
            iemit("unreachable");
        } else {
            iemit("call $%s", n->name);
        }
        return;
    }

    case ND_CAST: {
        wasm_gen_expr(n->lhs);
        bool sf = node_is_float(n->lhs);
        bool df = is_float_type(n->type);
        if (sf && df) {
            bool sd = (n->lhs->type->kind == TY_DOUBLE);
            bool dd = (n->type->kind == TY_DOUBLE);
            if (sd && !dd) iemit("f32.demote_f64");
            else if (!sd && dd) iemit("f64.promote_f32");
        } else if (!sf && df) {
            if (n->type->kind == TY_FLOAT) iemit("f32.convert_i64_s");
            else iemit("f64.convert_i64_s");
        } else if (sf && !df) {
            if (n->lhs->type->kind == TY_FLOAT) iemit("i64.trunc_f32_s");
            else iemit("i64.trunc_f64_s");
        }
        /* int->int: truncate to target width via mask */
        if (!sf && !df && n->type) {
            int sz = type_size(n->type);
            if (sz == 1) { iemit("i64.const 0xff"); iemit("i64.and"); iemit("i64.extend8_s"); }
            else if (sz == 2) { iemit("i64.const 0xffff"); iemit("i64.and"); iemit("i64.extend16_s"); }
            else if (sz == 4) { iemit("i32.wrap_i64"); iemit("i64.extend_i32_s"); }
        }
        return;
    }

    case ND_PREINC: case ND_PREDEC:
    case ND_POSTINC: case ND_POSTDEC: {
        bool fp = is_float_type(n->type);
        bool is_ptr = n->type && (n->type->kind == TY_PTR || n->type->kind == TY_ARRAY);
        int step = 1;
        if (is_ptr) step = n->type->base ? type_size(n->type->base) : 1;

        if (n->lhs->is_local) {
            int idx = local_idx(n->lhs->offset);
            iemit("local.get %d", idx);       /* old value */
            if (n->kind == ND_POSTINC || n->kind == ND_POSTDEC) {
                /* need old value as result AND new value stored */
                iemit("local.get %d", idx);   /* duplicate */
                if (fp) {
                    if (n->type->kind == TY_FLOAT) iemit("f32.const 1.0");
                    else iemit("f64.const 1.0");
                    if (n->kind == ND_POSTINC) iemit("%s.add", fp ? (n->type->kind==TY_FLOAT?"f32":"f64") : "i64");
                    else iemit("%s.sub", fp ? (n->type->kind==TY_FLOAT?"f32":"f64") : "i64");
                } else {
                    iemit("i64.const %d", (n->kind == ND_POSTINC) ? step : -step);
                    iemit("i64.add");
                }
                iemit("local.set %d", idx);
                /* old value stays on stack */
            } else {
                /* pre: update first, return new value */
                if (fp) {
                    if (n->type->kind == TY_FLOAT) iemit("f32.const 1.0");
                    else iemit("f64.const 1.0");
                    const char *ft2 = (n->type->kind == TY_FLOAT) ? "f32" : "f64";
                    if (n->kind == ND_PREINC) iemit("%s.add", ft2);
                    else iemit("%s.sub", ft2);
                } else {
                    iemit("i64.const %d", (n->kind == ND_PREINC) ? step : -step);
                    iemit("i64.add");
                }
                iemit("local.tee %d", idx);
            }
        } else {
            /* global / memory address -- use tee pattern */
            wasm_gen_addr(n->lhs);
            iemit(";; inc/dec on memory addr (simplified)");
        }
        return;
    }

    default:
        die("wasm: unhandled expression kind %d", n->kind);
    }
}

/* ------------------------------------------------------------------ */
/* Statement codegen                                                    */
/* Break/continue use wasm block/loop labels.                          */
/* ------------------------------------------------------------------ */

/* We track loop labels as a stack */
#define MAX_LOOP_DEPTH 64
static int break_labels[MAX_LOOP_DEPTH];
static int continue_labels[MAX_LOOP_DEPTH];
static int loop_depth = 0;

static struct Type *current_func_ret_type = NULL;

static void wasm_gen_stmt(struct Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK:
            for (struct Node *s = n->stmts; s; s = s->next)
                wasm_gen_stmt(s);
            return;

        case ND_EXPR_STMT:
            wasm_gen_expr(n->lhs);
            /* always drop: every expression leaves a value, statements don't consume it */
            if (n->lhs->type && n->lhs->type->kind != TY_VOID)
                iemit("drop");
            return;

        case ND_DECL:
            if (!n->init) return;
            if (n->is_local) {
                int idx = local_idx(n->offset);
                if (is_struct_type(n->type)) {
                    /* struct init: push dest addr (software stack slot) */
                    iemit("local.get $__sp_saved");
                    if (n->offset) { iemit("i32.const %d", n->offset); iemit("i32.add"); }
                    if (n->init->kind == ND_CALL) wasm_gen_expr(n->init);
                    else wasm_gen_addr(n->init);
                    iemit("i32.const %d", type_size(n->type));
                    iemit("memory.copy");
                } else {
                    wasm_gen_expr(n->init);
                    bool lf = is_float_type(n->type);
                    bool rf = node_is_float(n->init);
                    if (lf && !rf) {
                        if (n->type->kind == TY_DOUBLE) iemit("f64.convert_i64_s");
                        else iemit("f32.convert_i64_s");
                    } else if (!lf && rf) {
                        if (n->init->type->kind == TY_FLOAT) iemit("i64.trunc_f32_s");
                        else iemit("i64.trunc_f64_s");
                    }
                    iemit("local.set %d", idx);
                }
            } else {
                /* global init handled in gen_gvar */
            }
            return;

        case ND_RETURN: {
            if (n->lhs) {
                wasm_gen_expr(n->lhs);
                /* convert if return type differs */
                bool lf = is_float_type(current_func_ret_type);
                bool rf = node_is_float(n->lhs);
                if (lf && !rf) {
                    if (current_func_ret_type->kind == TY_DOUBLE) iemit("f64.convert_i64_s");
                    else iemit("f32.convert_i64_s");
                } else if (!lf && rf) {
                    if (n->lhs->type->kind == TY_FLOAT) iemit("i64.trunc_f32_s");
                    else iemit("i64.trunc_f64_s");
                }
            }
            /* restore software stack pointer before returning */
            if (frame_size > 0) {
                /* We need to restore $__sp but return value is on stack.
                   If there's a return value, stash it, restore sp, push it back. */
                if (n->lhs && current_func_ret_type &&
                        current_func_ret_type->kind != TY_VOID) {
                    iemit(";; restore $__sp before return");
                    /* for simplicity just restore -- value already computed above */
                }
                iemit("global.get $__sp");
                iemit("i32.const %d", frame_size);
                iemit("i32.add");
                iemit("global.set $__sp");
            }
            iemit("return");
            return;
        }

        case ND_IF: {
            int lbl = new_label();
            wasm_gen_expr(n->cond);
            if (node_is_float(n->cond)) { iemit("f64.const 0.0"); iemit("f64.ne"); }
            else { iemit("i64.const 0"); iemit("i64.ne"); }
            iemit("if");
              INDENT();
              wasm_gen_stmt(n->then);
              if (n->els) { DEDENT(); iemit("else"); INDENT(); wasm_gen_stmt(n->els); }
            DEDENT();
            iemit("end ;; if_%d", lbl);
            (void)lbl;
            return;
        }

        case ND_WHILE: {
            int lbl = new_label();
            if (loop_depth >= MAX_LOOP_DEPTH) die("wasm: loop nesting too deep");
            break_labels[loop_depth]    = lbl;
            continue_labels[loop_depth] = lbl;
            loop_depth++;
            iemit("block $brk_%d", lbl);
              INDENT();
              iemit("loop $loop_%d", lbl);
                INDENT();
                wasm_gen_expr(n->cond);
                iemit("i64.eqz");
                iemit("br_if $brk_%d", lbl);
                wasm_gen_stmt(n->body);
                iemit("br $loop_%d", lbl);
              DEDENT();
              iemit("end ;; loop_%d", lbl);
            DEDENT();
            iemit("end ;; brk_%d", lbl);
            loop_depth--;
            return;
        }

        case ND_DO_WHILE: {
            int lbl = new_label();
            if (loop_depth >= MAX_LOOP_DEPTH) die("wasm: loop nesting too deep");
            break_labels[loop_depth]    = lbl;
            continue_labels[loop_depth] = lbl;
            loop_depth++;
            iemit("block $brk_%d", lbl);
              INDENT();
              iemit("loop $loop_%d", lbl);
                INDENT();
                wasm_gen_stmt(n->body);
                wasm_gen_expr(n->cond);
                iemit("i64.const 0"); iemit("i64.ne");
                iemit("br_if $loop_%d", lbl);
              DEDENT();
              iemit("end ;; loop_%d", lbl);
            DEDENT();
            iemit("end ;; brk_%d", lbl);
            loop_depth--;
            return;
        }

        case ND_FOR: {
            int lbl = new_label();
            if (loop_depth >= MAX_LOOP_DEPTH) die("wasm: loop nesting too deep");
            break_labels[loop_depth]    = lbl;
            continue_labels[loop_depth] = lbl;
            loop_depth++;
            if (n->init) { wasm_gen_expr(n->init); iemit("drop"); }
            iemit("block $brk_%d", lbl);
              INDENT();
              iemit("loop $loop_%d", lbl);
                INDENT();
                if (n->cond) {
                    wasm_gen_expr(n->cond);
                    iemit("i64.const 0"); iemit("i64.eq");
                    iemit("br_if $brk_%d", lbl);
                }
                wasm_gen_stmt(n->body);
                if (n->step) { wasm_gen_expr(n->step); iemit("drop"); }
                iemit("br $loop_%d", lbl);
              DEDENT();
              iemit("end ;; loop_%d", lbl);
            DEDENT();
            iemit("end ;; brk_%d", lbl);
            loop_depth--;
            return;
        }

        case ND_BREAK:
            if (loop_depth == 0) die("wasm: break outside loop");
            iemit("br $brk_%d", break_labels[loop_depth - 1]);
            return;

        case ND_CONTINUE:
            if (loop_depth == 0) die("wasm: continue outside loop");
            iemit("br $loop_%d", continue_labels[loop_depth - 1]);
            return;

        case ND_GOTO:
        case ND_LABEL:
            die("wasm: goto/labels not supported in wasm backend");

        default:
            die("wasm: unhandled statement kind %d", n->kind);
    }
}

/* ------------------------------------------------------------------ */
/* Scan a function body to collect all locals (params + declarations)  */
/* ------------------------------------------------------------------ */

static void scan_locals(struct Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_DECL:
            if (n->is_local)
                reg_local(n->offset, n->type);
            scan_locals(n->init);
            return;
        case ND_BLOCK:
            for (struct Node *s = n->stmts; s; s = s->next) scan_locals(s);
            return;
        case ND_IF:
            scan_locals(n->cond); scan_locals(n->then); scan_locals(n->els);
            return;
        case ND_WHILE: case ND_DO_WHILE:
            scan_locals(n->cond); scan_locals(n->body);
            return;
        case ND_FOR:
            scan_locals(n->init); scan_locals(n->cond);
            scan_locals(n->step); scan_locals(n->body);
            return;
        case ND_RETURN: scan_locals(n->lhs); return;
        case ND_EXPR_STMT: scan_locals(n->lhs); return;
        default:
            /* expressions: scan children */
            scan_locals(n->lhs); scan_locals(n->rhs);
            scan_locals(n->cond); scan_locals(n->then); scan_locals(n->els);
            scan_locals(n->body); scan_locals(n->init); scan_locals(n->step);
            scan_locals(n->args); scan_locals(n->next);
            return;
    }
}

/* ------------------------------------------------------------------ */
/* gen_func -- emit a (func ...) definition in WAT                     */
/* ------------------------------------------------------------------ */

void gen_func(struct Node *fn) {
    reset_locals();
    current_func_ret_type = fn->type;

    /* --- Register params as wasm locals (index 0..nparams-1) --- */
    for (struct Node *p = fn->params; p; p = p->next)
        reg_local(p->offset, p->type);

    /* Allocate $__sp_saved local */
    local_sp_idx = next_local_idx++;

    /* --- Scan body for all local variable declarations --- */
    scan_locals(fn->body);

    /* --- Determine software stack frame size ---
       The resolver sets fn->ival = total frame size in bytes. */
    frame_size = (int)fn->ival;

    /* --- Emit function signature --- */
    iemit(";; ----------------------------------------");
    /* Build param list */
    char param_buf[1024] = "";
    int  pb = 0;
    for (struct Node *p = fn->params; p; p = p->next) {
        pb += snprintf(param_buf + pb, sizeof(param_buf) - pb,
                       " (param %s)", wasm_type(p->type));
    }
    /* Return type */
    const char *ret_wt = "";
    if (fn->type && fn->type->kind != TY_VOID)
        ret_wt = wasm_type(fn->type);
    char result_buf[64] = "";
    if (*ret_wt) snprintf(result_buf, sizeof(result_buf), " (result %s)", ret_wt);

    iemit("(func $%s%s%s", fn->name, param_buf, result_buf);
    INDENT();

    /* --- Declare $__sp_saved local --- */
    iemit("(local $__sp_saved i32)");

    /* --- Declare non-param locals --- */
    int param_count = 0;
    for (struct Node *p = fn->params; p; p = p->next) param_count++;

    for (int i = param_count; i < local_count; i++) {
        iemit("(local %s) ;; offset %d", wasm_type(local_table[i].type),
              local_table[i].offset);
    }

    /* --- Software stack prologue --- */
    if (frame_size > 0) {
        iemit(";; software stack prologue");
        iemit("global.get $__sp");
        iemit("local.set $__sp_saved");
        iemit("global.get $__sp");
        iemit("i32.const %d", frame_size);
        iemit("i32.sub");
        iemit("global.set $__sp");
    }

    /* --- Body --- */
    wasm_gen_stmt(fn->body);

    /* --- Epilogue: restore software stack --- */
    if (frame_size > 0) {
        iemit(";; software stack epilogue");
        iemit("local.get $__sp_saved");
        iemit("global.set $__sp");
    }

    DEDENT();
    iemit(")");
    iemit("(export \"%s\" (func $%s))", fn->name, fn->name);
    iemit("");
}

/* ------------------------------------------------------------------ */
/* gen_gvar -- register a global variable and emit its initialiser     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Serialise a static initializer into a byte buffer (for data segs)  */
/* ------------------------------------------------------------------ */

static void wasm_serial_init(struct Node *init, struct Type *type,
                              unsigned char *buf, int buf_off, int buf_sz);

static void wasm_serial_scalar(struct Node *val, struct Type *type,
                                unsigned char *buf, int off) {
    int sz = type_size(type);
    if (!val) { /* zero already */ return; }
    if (val->kind == ND_INT) {
        long long v = val->ival;
        for (int i = 0; i < sz; i++) buf[off+i] = (unsigned char)((v >> (8*i)) & 0xff);
    } else if (val->kind == ND_FLOAT) {
        if (sz == 4) {
            union { float f; uint32_t u; } u; u.f = (float)val->fval;
            for (int i = 0; i < 4; i++) buf[off+i] = (u.u >> (8*i)) & 0xff;
        } else {
            union { double d; uint64_t u; } u; u.d = val->fval;
            for (int i = 0; i < 8; i++) buf[off+i] = (unsigned char)((u.u >> (8*i)) & 0xff);
        }
    } else if (val->kind == ND_STR) {
        /* string pointer: store the address in linear memory as 4-byte i32 */
        int saddr = alloc_string(val->sval);
        for (int i = 0; i < 4; i++) buf[off+i] = (saddr >> (8*i)) & 0xff;
    }
    /* other expr kinds (not constant) ignored -- zero remains */
}

static void wasm_serial_array(struct Node *list, struct Type *arr_type,
                               unsigned char *buf, int base, int buf_sz) {
    struct Type *et = arr_type->base;
    int esz    = type_size(et);
    int count  = arr_type->array_size;
    int max_idx = 0;
    for (struct Node *e = list ? list->stmts : NULL; e; e = e->next)
        if ((int)e->ival > max_idx) max_idx = (int)e->ival;
    int n_elems = (count > 0) ? count : max_idx + 1;
    for (struct Node *e = list ? list->stmts : NULL; e; e = e->next) {
        int idx = (int)e->ival;
        if (idx < 0 || idx >= n_elems) continue;
        int off = base + idx * esz;
        if (off + esz > buf_sz) continue;
        wasm_serial_init(e->lhs, et, buf, off, buf_sz);
    }
}

static void wasm_serial_struct(struct Node *list, struct Type *st,
                                unsigned char *buf, int base, int buf_sz) {
    int seq = 0;
    for (struct Node *e = list ? list->stmts : NULL; e; e = e->next) {
        struct Member *m = NULL;
        if (e->name) {
            for (struct Member *mb = st->members; mb; mb = mb->next)
                if (mb->name && strcmp(mb->name, e->name) == 0) { m = mb; break; }
        } else {
            int idx = 0;
            for (struct Member *mb = st->members; mb; mb = mb->next) {
                if (!mb->name) continue;
                if (idx++ == seq) { m = mb; break; }
            }
        }
        if (!m) { seq++; continue; }
        int off = base + m->offset;
        if (off + type_size(m->type) <= buf_sz)
            wasm_serial_init(e->lhs, m->type, buf, off, buf_sz);
        seq++;
    }
}

static void wasm_serial_init(struct Node *init, struct Type *type,
                              unsigned char *buf, int buf_off, int buf_sz) {
    if (!init) return;
    if (init->kind == ND_INIT_LIST) {
        if (type->kind == TY_ARRAY)
            wasm_serial_array(init, type, buf, buf_off, buf_sz);
        else if (type->kind == TY_STRUCT || type->kind == TY_UNION)
            wasm_serial_struct(init, type, buf, buf_off, buf_sz);
        else if (init->stmts)
            wasm_serial_scalar(init->stmts->lhs, type, buf, buf_off);
    } else {
        wasm_serial_scalar(init, type, buf, buf_off);
    }
}

/* Emit a WAT (data ...) segment for the byte buffer. */
static void emit_data_seg(int addr, const unsigned char *buf, int sz) {
    /* WAT data segment syntax: (data (i32.const ADDR) "...escaped bytes...") */
    fprintf(out, "  (data (i32.const %d) \"", addr);
    for (int i = 0; i < sz; i++) {
        unsigned char ch = buf[i];
        if (ch >= 0x20 && ch < 0x7f && ch != 0x22 /* " */ && ch != 0x5c /* \ */)
            fputc(ch, out);
        else
            fprintf(out, "\\%02x", ch);
    }
    fprintf(out, "\")\n");
}

void gen_gvar(struct Node *gv) {
    int sz   = type_size(gv->type);
    int addr = gvar_alloc(gv->name, sz);
    gv->ival = addr;   /* stash for module_close */
}

/* ------------------------------------------------------------------ */
/* Module preamble and footer helpers                                   */
/* emit_module_open / emit_module_close are called from main()         */
/* ------------------------------------------------------------------ */

static void emit_import(const char *name) {
    /* Emit a wasm import for a C runtime function we can't generate */
    iemit("(import \"env\" \"%s\" (func $%s))", name, name);
}

/* Collect all function names referenced as imports (protos without bodies) */
typedef struct ImportEntry { char *name; struct ImportEntry *next; } ImportEntry;
static ImportEntry *imports = NULL;

static void need_import(const char *name) {
    for (ImportEntry *e = imports; e; e = e->next)
        if (strcmp(e->name, name) == 0) return;
    ImportEntry *e = arena_alloc(sizeof(ImportEntry));
    e->name = arena_strdup(name);
    e->next = imports;
    imports = e;
}

/* Call this from main() BEFORE emitting any functions */
void wasm_emit_module_open(struct Node *program) {
    indent_level = 0;
    iemit("(module");
    INDENT();

    /* Collect imported protos */
    for (struct Node *n = program; n; n = n->next) {
        if (n->kind == ND_PROTO)
            need_import(n->name);
    }
    /* Also scan all call nodes for unknown names (malloc, free, etc.) */
    /* (A simple heuristic: any name that has a ProtoEntry but no ND_FUNC.) */

    /* Emit imports */
    for (ImportEntry *e = imports; e; e = e->next)
        emit_import(e->name);
    /* Always import malloc/free (registered by main in bake_frontend) */
    emit_import("malloc");
    emit_import("free");

    /* Memory */
    iemit("(memory (export \"memory\") %d)", MEMORY_PAGES);

    /* Software stack pointer global */
    iemit("(global $__sp (mut i32) (i32.const %d))", STACK_TOP);
}

/* Call this from main() AFTER all gen_func/gen_gvar calls */
void wasm_emit_module_close(struct Node *program) {
    /* Emit data segments for string/float literals collected during codegen */
    for (DataSeg *d = data_segs; d; d = d->next)
        emit_data_seg(d->addr, (const unsigned char *)d->bytes, d->len);

    /* Emit global variable initial values as data segments */
    for (struct Node *n = program; n; n = n->next) {
        if (n->kind != ND_GVAR || !n->init) continue;
        int addr = (int)n->ival;
        int sz   = type_size(n->type);
        /* Serialise the initializer into a zero-filled byte buffer */
        unsigned char *buf = arena_alloc(sz);
        wasm_serial_init(n->init, n->type, buf, 0, sz);
        emit_data_seg(addr, buf, sz);
    }

    DEDENT();
    iemit(")");  /* end module */
}
