#pragma once
/* bake.h -- shared declarations between bake_frontend.c and codegen_x86.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Arena / error                                                        */
/* ------------------------------------------------------------------ */

void  *arena_alloc(size_t n);
char  *arena_strdup(const char *s);
extern int current_line;
void   die(const char *fmt, ...) __attribute__((noreturn, format(printf,1,2)));

/* ------------------------------------------------------------------ */
/* Tokens                                                               */
/* ------------------------------------------------------------------ */

enum TokenKind {
    TK_INT_LIT, TK_FLOAT_LIT, TK_CHAR_LIT, TK_STR_LIT,
    TK_IDENT,
    TK_AUTO, TK_BREAK, TK_CHAR, TK_CONTINUE, TK_DO,
    TK_DOUBLE, TK_ELSE, TK_EXTERN, TK_FLOAT, TK_FOR,
    TK_GOTO, TK_IF, TK_INT, TK_LONG, TK_REGISTER,
    TK_RETURN, TK_SHORT, TK_STATIC, TK_STRUCT, TK_UNION,
    TK_UNSIGNED, TK_VOID, TK_WHILE,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET, TK_SEMI, TK_COMMA,
    TK_DOT, TK_ARROW, TK_COLON, TK_QUESTION,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_LSHIFT, TK_RSHIFT,
    TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LEQ, TK_GEQ,
    TK_AND, TK_OR,
    TK_ASSIGN,
    TK_PLUS_ASSIGN, TK_MINUS_ASSIGN, TK_STAR_ASSIGN,
    TK_SLASH_ASSIGN, TK_PERCENT_ASSIGN,
    TK_AMP_ASSIGN, TK_PIPE_ASSIGN, TK_CARET_ASSIGN,
    TK_LSHIFT_ASSIGN, TK_RSHIFT_ASSIGN,
    TK_PLUSPLUS, TK_MINUSMINUS,
    TK_ELLIPSIS,
    TK_EOF,
};

struct Token {
    enum TokenKind kind;
    int            line;
    char          *sval;
    long long      ival;
    double         fval;
    bool           is_float;
};

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

enum TypeKind {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_LONGLONG,
    TY_FLOAT, TY_DOUBLE, TY_UNSIGNED,
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNC,
};

struct Member {
    char         *name;
    struct Type  *type;
    int           offset;
    struct Member *next;
};

struct Type {
    enum TypeKind  kind;
    struct Type   *base;
    int            array_size;
    char          *tag;
    struct Member *members;
    int            size;
    int            align;
    struct Type   *ret;
    struct Type  **param_types;
    int            nparams;
    bool           func_variadic;
    bool           is_unsigned;
};

/* ------------------------------------------------------------------ */
/* AST nodes                                                            */
/* ------------------------------------------------------------------ */

enum NodeKind {
    ND_INT, ND_FLOAT, ND_STR, ND_IDENT,
    ND_UNARY, ND_BINARY, ND_TERNARY,
    ND_ASSIGN, ND_CALL,
    ND_INDEX, ND_MEMBER, ND_ARROW,
    ND_CAST,
    ND_PREINC, ND_PREDEC, ND_POSTINC, ND_POSTDEC,
    ND_ADDR, ND_DEREF,
    ND_BLOCK,
    ND_IF,
    ND_WHILE, ND_DO_WHILE, ND_FOR,
    ND_RETURN, ND_BREAK, ND_CONTINUE,
    ND_GOTO, ND_LABEL,
    ND_EXPR_STMT,
    ND_DECL,
    ND_FUNC,
    ND_GVAR,
    ND_STRUCT_DEF,
    ND_PROTO,
};

struct Node {
    enum NodeKind  kind;
    int            line;
    struct Type   *type;
    long long      ival;
    double         fval;
    char          *sval;
    struct Node   *lhs;
    struct Node   *rhs;
    struct Node   *cond;
    struct Node   *then;
    struct Node   *els;
    struct Node   *body;
    struct Node   *init;
    struct Node   *step;
    struct Node   *next;
    struct Node   *args;
    struct Node   *params;
    struct Node   *stmts;
    enum TokenKind op;
    char          *name;
    struct Node   *callee;
    bool           is_local;
    bool           is_variadic;
    bool           is_float;
    int            offset;
    int            sret_offset;
};

/* ------------------------------------------------------------------ */
/* Symbol / prototype tables                                            */
/* ------------------------------------------------------------------ */

struct Sym {
    char        *name;
    struct Type *type;
    bool         is_local;
    int          offset;
    struct Sym  *next;
};

struct ProtoEntry {
    char             *name;
    struct Type      *ret_type;
    int               arity;
    bool              is_variadic;
    struct ProtoEntry *next;
};

/* ------------------------------------------------------------------ */
/* Frontend API                                                         */
/* ------------------------------------------------------------------ */

/* arena */
void   *arena_alloc(size_t n);
char   *arena_strdup(const char *s);

/* type helpers */
int            type_size(struct Type *t);
bool           is_struct_type(struct Type *t);
bool           struct_fits_regs(struct Type *t);
bool           struct_needs_sret(struct Type *t);
struct Member *find_member(struct Type *t, const char *name);

/* type globals */
extern struct Type *ty_void;
extern struct Type *ty_char;
extern struct Type *ty_short;
extern struct Type *ty_int;
extern struct Type *ty_long;
extern struct Type *ty_longlong;
extern struct Type *ty_float;
extern struct Type *ty_double;

/* type constructors */
void         init_types(void);
struct Type *ptr_to(struct Type *base);
struct Type *array_of(struct Type *base, int n);
struct Type *func_type(struct Type *ret, struct Type **params, int nparams, bool variadic);

/* symbol / scope */
void              push_scope(void);
void              pop_scope(void);
struct Sym       *add_sym(const char *name, struct Type *type, bool local, int offset);
struct Sym       *find_sym(const char *name);
void              add_proto(const char *name, struct Type *ret, int arity, bool variadic);
struct ProtoEntry *find_proto(const char *name);

/* pipeline */
void         lex_all(void);
struct Node *parse_program(void);
void         resolve_func(struct Node *fn);

/* ------------------------------------------------------------------ */
/* Codegen API (implemented by each backend, called by main)           */
/* ------------------------------------------------------------------ */

/* output stream — set by main(), used by the active backend */
extern FILE *out;

void gen_func(struct Node *fn);
void gen_gvar(struct Node *gv);
void emit(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* ------------------------------------------------------------------ */
/* Wasm-specific hooks (only in codegen_wasm.c / bake_wasm binary)    */
/* ------------------------------------------------------------------ */
void wasm_emit_module_open(struct Node *program);
void wasm_emit_module_close(struct Node *program);
