#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Arena allocator                                                      */
/* ------------------------------------------------------------------ */

#define ARENA_SIZE (32 * 1024 * 1024)  /* 32 MB */

static char  arena_buf[ARENA_SIZE];
static size_t arena_top = 0;

static void *arena_alloc(size_t n) {
    n = (n + 7) & ~(size_t)7;          /* 8-byte align */
    if (arena_top + n > ARENA_SIZE) {
        fprintf(stderr, "out of arena memory\n");
        exit(1);
    }
    void *p = arena_buf + arena_top;
    arena_top += n;
    memset(p, 0, n);
    return p;
}

static char *arena_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* Error reporting                                                      */
/* ------------------------------------------------------------------ */

static int current_line = 1;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error (line %d): ", current_line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Tokens                                                               */
/* ------------------------------------------------------------------ */

enum TokenKind {
    /* literals */
    TK_INT_LIT, TK_FLOAT_LIT, TK_CHAR_LIT, TK_STR_LIT,
    /* identifiers */
    TK_IDENT,
    /* keywords */
    TK_AUTO, TK_BREAK, TK_CHAR, TK_CONTINUE, TK_DO,
    TK_DOUBLE, TK_ELSE, TK_EXTERN, TK_FLOAT, TK_FOR,
    TK_GOTO, TK_IF, TK_INT, TK_LONG, TK_REGISTER,
    TK_RETURN, TK_SHORT, TK_STATIC, TK_STRUCT, TK_UNION,
    TK_UNSIGNED, TK_VOID, TK_WHILE,
    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET, TK_SEMI, TK_COMMA,
    TK_DOT, TK_ARROW, TK_COLON, TK_QUESTION,
    /* operators */
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
    /* special */
    TK_EOF,
};

struct Token {
    enum TokenKind kind;
    int       line;
    /* value */
    char     *sval;   /* TK_IDENT, TK_STR_LIT */
    long long ival;   /* TK_INT_LIT, TK_CHAR_LIT */
    double    fval;   /* TK_FLOAT_LIT */
    bool      is_float; /* TK_FLOAT_LIT: true=float(f suffix), false=double */
};

/* ------------------------------------------------------------------ */
/* Lexer                                                                */
/* ------------------------------------------------------------------ */

static const char *src;
static int         src_pos;

static char lpeek(void) { return src[src_pos]; }
static char lnext(void) {
    char c = src[src_pos++];
    if (c == '\n') current_line++;
    return c;
}
static bool lmatch(char c) {
    if (src[src_pos] == c) { src_pos++; return true; }
    return false;
}

static struct Token make_tok(enum TokenKind k) {
    struct Token t = {0};
    t.kind = k;
    t.line = current_line;
    return t;
}

static struct { const char *word; enum TokenKind kind; } keywords[] = {
    {"auto",     TK_AUTO},     {"break",    TK_BREAK},
    {"char",     TK_CHAR},     {"continue", TK_CONTINUE},
    {"do",       TK_DO},       {"double",   TK_DOUBLE},
    {"else",     TK_ELSE},     {"extern",   TK_EXTERN},
    {"float",    TK_FLOAT},    {"for",      TK_FOR},
    {"goto",     TK_GOTO},     {"if",       TK_IF},
    {"int",      TK_INT},      {"long",     TK_LONG},
    {"register", TK_REGISTER}, {"return",   TK_RETURN},
    {"short",    TK_SHORT},    {"static",   TK_STATIC},
    {"struct",   TK_STRUCT},   {"union",    TK_UNION},
    {"unsigned", TK_UNSIGNED}, {"void",     TK_VOID},
    {"while",    TK_WHILE},    {NULL, 0}
};

/* token buffer -- we lex the whole file up front */
#define MAX_TOKENS 65536
static struct Token tokens[MAX_TOKENS];
static int   tok_count = 0;
static int   tok_pos   = 0;

static void lex_all(void) {
    while (1) {
        char c = lpeek();

        /* skip whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lnext(); continue;
        }
        /* line comments */
        if (c == '/' && src[src_pos+1] == '/') {
            while (lpeek() && lpeek() != '\n') lnext();
            continue;
        }
        /* block comments */
        if (c == '/' && src[src_pos+1] == '*') {
            lnext(); lnext();
            while (lpeek()) {
                if (lnext() == '*' && lpeek() == '/') { lnext(); break; }
            }
            continue;
        }
        /* preprocessor -- hard error */
        if (c == '#') die("preprocessor directives not supported");

        if (c == '\0') break;

        struct Token t = make_tok(TK_EOF);

        /* integer / float literals */
        if (isdigit((unsigned char)c)) {
            long long ival = 0;
            /* hex: c=='0', next is 'x'/'X' */
            if (c == '0' && (src[src_pos+1] == 'x' || src[src_pos+1] == 'X')) {
                lnext(); lnext(); /* consume '0' and 'x' */
                while (isxdigit((unsigned char)lpeek())) {
                    char h = lnext();
                    ival = ival * 16 + (isdigit((unsigned char)h)
                           ? h - '0' : tolower((unsigned char)h) - 'a' + 10);
                }
                while (lpeek()=='l'||lpeek()=='L'||lpeek()=='u'||lpeek()=='U') lnext();
                t.kind = TK_INT_LIT; t.ival = ival;
                if (tok_count >= MAX_TOKENS) die("too many tokens");
                tokens[tok_count++] = t;
                continue;
            }
            /* octal: c=='0', next is a digit */
            if (c == '0' && isdigit((unsigned char)src[src_pos+1])) {
                lnext(); /* consume '0' */
                while (isdigit((unsigned char)lpeek()))
                    ival = ival * 8 + (lnext() - '0');
                while (lpeek()=='l'||lpeek()=='L'||lpeek()=='u'||lpeek()=='U') lnext();
                t.kind = TK_INT_LIT; t.ival = ival;
                if (tok_count >= MAX_TOKENS) die("too many tokens");
                tokens[tok_count++] = t;
                continue;
            }
            /* decimal / float: consume c then keep reading */
            lnext();
            ival = c - '0';
            while (isdigit((unsigned char)lpeek()))
                ival = ival * 10 + (lnext() - '0');
            if (lpeek() == '.' || lpeek() == 'e' || lpeek() == 'E') {
                /* float */
                char buf[64]; int bi = 0;
                sprintf(buf, "%lld", ival);
                bi = (int)strlen(buf);
                if (lpeek() == '.') { buf[bi++] = lnext(); }
                while (isdigit((unsigned char)lpeek())) buf[bi++] = lnext();
                if (lpeek() == 'e' || lpeek() == 'E') {
                    buf[bi++] = lnext();
                    if (lpeek() == '+' || lpeek() == '-') buf[bi++] = lnext();
                    while (isdigit((unsigned char)lpeek())) buf[bi++] = lnext();
                }
                buf[bi] = '\0';
                t.kind = TK_FLOAT_LIT;
                t.fval = atof(buf);
                /* consume optional f/F (float) or l/L (long double, treated as double) suffix */
                if (lpeek() == 'f' || lpeek() == 'F') { lnext(); t.is_float = true; }
                else if (lpeek() == 'l' || lpeek() == 'L') { lnext(); t.is_float = false; }
                else { t.is_float = false; }
            } else {
                /* skip L/U suffixes */
                while (lpeek() == 'l' || lpeek() == 'L' ||
                       lpeek() == 'u' || lpeek() == 'U') lnext();
                t.kind = TK_INT_LIT;
                t.ival = ival;
            }
            if (tok_count >= MAX_TOKENS) die("too many tokens");
            tokens[tok_count++] = t;
            continue;
        }

        /* char literal */
        if (c == '\'') {
            lnext();
            long long val = 0;
            if (lpeek() == '\\') {
                lnext();
                char e = lnext();
                switch (e) {
                    case 'n': val = '\n'; break;
                    case 't': val = '\t'; break;
                    case 'r': val = '\r'; break;
                    case '0': val = '\0'; break;
                    case '\\': val = '\\'; break;
                    case '\'': val = '\''; break;
                    default:   val = e;   break;
                }
            } else {
                val = lnext();
            }
            if (lnext() != '\'') die("unterminated char literal");
            t.kind = TK_CHAR_LIT;
            t.ival = val;
            tokens[tok_count++] = t;
            continue;
        }

        /* string literal */
        if (c == '"') {
            lnext();
            char buf[4096]; int bi = 0;
            while (lpeek() && lpeek() != '"') {
                if (lpeek() == '\\') {
                    lnext();
                    char e = lnext();
                    switch (e) {
                        case 'n': buf[bi++] = '\n'; break;
                        case 't': buf[bi++] = '\t'; break;
                        case 'r': buf[bi++] = '\r'; break;
                        case '0': buf[bi++] = '\0'; break;
                        case '\\': buf[bi++] = '\\'; break;
                        case '"':  buf[bi++] = '"';  break;
                        default:   buf[bi++] = e;    break;
                    }
                } else {
                    buf[bi++] = lnext();
                }
            }
            if (lnext() != '"') die("unterminated string literal");
            buf[bi] = '\0';
            t.kind = TK_STR_LIT;
            t.sval = arena_strdup(buf);
            tokens[tok_count++] = t;
            continue;
        }

        /* identifiers and keywords */
        if (isalpha((unsigned char)c) || c == '_') {
            char buf[256]; int bi = 0;
            while (isalnum((unsigned char)lpeek()) || lpeek() == '_')
                buf[bi++] = lnext();
            buf[bi] = '\0';
            t.kind = TK_IDENT;
            t.sval = arena_strdup(buf);
            for (int i = 0; keywords[i].word; i++) {
                if (strcmp(buf, keywords[i].word) == 0) {
                    t.kind = keywords[i].kind;
                    break;
                }
            }
            tokens[tok_count++] = t;
            continue;
        }

        /* punctuation and operators */
        lnext();
        switch (c) {
            case '(': t.kind = TK_LPAREN;    break;
            case ')': t.kind = TK_RPAREN;    break;
            case '{': t.kind = TK_LBRACE;    break;
            case '}': t.kind = TK_RBRACE;    break;
            case '[': t.kind = TK_LBRACKET;  break;
            case ']': t.kind = TK_RBRACKET;  break;
            case ';': t.kind = TK_SEMI;      break;
            case ',': t.kind = TK_COMMA;     break;
            case '?': t.kind = TK_QUESTION;  break;
            case ':': t.kind = TK_COLON;     break;
            case '~': t.kind = TK_TILDE;     break;
            case '.':
                if (src[src_pos] == '.' && src[src_pos+1] == '.') {
                    src_pos += 2; t.kind = TK_ELLIPSIS;
                } else t.kind = TK_DOT;
                break;
            case '+':
                if (lmatch('+'))      t.kind = TK_PLUSPLUS;
                else if (lmatch('=')) t.kind = TK_PLUS_ASSIGN;
                else                  t.kind = TK_PLUS;
                break;
            case '-':
                if (lmatch('-'))      t.kind = TK_MINUSMINUS;
                else if (lmatch('>')) t.kind = TK_ARROW;
                else if (lmatch('=')) t.kind = TK_MINUS_ASSIGN;
                else                  t.kind = TK_MINUS;
                break;
            case '*':
                t.kind = lmatch('=') ? TK_STAR_ASSIGN   : TK_STAR;    break;
            case '/':
                t.kind = lmatch('=') ? TK_SLASH_ASSIGN  : TK_SLASH;   break;
            case '%':
                t.kind = lmatch('=') ? TK_PERCENT_ASSIGN: TK_PERCENT; break;
            case '&':
                if (lmatch('&'))      t.kind = TK_AND;
                else if (lmatch('=')) t.kind = TK_AMP_ASSIGN;
                else                  t.kind = TK_AMP;
                break;
            case '|':
                if (lmatch('|'))      t.kind = TK_OR;
                else if (lmatch('=')) t.kind = TK_PIPE_ASSIGN;
                else                  t.kind = TK_PIPE;
                break;
            case '^':
                t.kind = lmatch('=') ? TK_CARET_ASSIGN : TK_CARET;   break;
            case '!':
                t.kind = lmatch('=') ? TK_NEQ           : TK_BANG;    break;
            case '=':
                t.kind = lmatch('=') ? TK_EQ            : TK_ASSIGN;  break;
            case '<':
                if (lmatch('<'))      t.kind = lmatch('=') ? TK_LSHIFT_ASSIGN : TK_LSHIFT;
                else if (lmatch('=')) t.kind = TK_LEQ;
                else                  t.kind = TK_LT;
                break;
            case '>':
                if (lmatch('>'))      t.kind = lmatch('=') ? TK_RSHIFT_ASSIGN : TK_RSHIFT;
                else if (lmatch('=')) t.kind = TK_GEQ;
                else                  t.kind = TK_GT;
                break;
            default:
                die("unexpected character '%c'", c);
        }
        if (tok_count >= MAX_TOKENS) die("too many tokens");
        tokens[tok_count++] = t;
    }
    tokens[tok_count++] = make_tok(TK_EOF);
}

/* ------------------------------------------------------------------ */
/* struct Token stream helpers                                                 */
/* ------------------------------------------------------------------ */

static struct Token *peek(void)       { return &tokens[tok_pos]; }
static struct Token *peek2(void)      { return &tokens[tok_pos < tok_count-1 ? tok_pos+1 : tok_pos]; }
static struct Token *advance(void)    { return &tokens[tok_pos++]; }
static bool   check(enum TokenKind k) { return peek()->kind == k; }
static bool   match(enum TokenKind k) {
    if (check(k)) { advance(); return true; } return false;
}
static struct Token *expect(enum TokenKind k, const char *msg) {
    if (!check(k)) die("%s", msg);
    return advance();
}

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

enum TypeKind {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_LONGLONG,
    TY_FLOAT, TY_DOUBLE, TY_UNSIGNED,
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNC,
};


struct Member {
    char   *name;
    struct Type   *type;
    int     offset;  /* byte offset within struct/union */
    struct Member *next;
};

struct Type {
    enum TypeKind kind;
    struct Type    *base;       /* for PTR, ARRAY */
    int      array_size; /* for ARRAY -- literal only */
    /* struct/union */
    char    *tag;
    struct Member  *members;
    int      size;       /* total size in bytes */
    int      align;
    /* func */
    struct Type    *ret;
    /* unsigned modifier */
    bool     is_unsigned;
};

static struct Type *ty_void;
static struct Type *ty_char;
static struct Type *ty_short;
static struct Type *ty_int;
static struct Type *ty_long;
static struct Type *ty_longlong;
static struct Type *ty_float;
static struct Type *ty_double;

static struct Type *new_type(enum TypeKind k) {
    struct Type *t = arena_alloc(sizeof(struct Type));
    t->kind = k;
    return t;
}

static void init_types(void) {
    ty_void   = new_type(TY_VOID);     ty_void->size  = 0;  ty_void->align  = 1;
    ty_char   = new_type(TY_CHAR);     ty_char->size  = 1;  ty_char->align  = 1;
    ty_short  = new_type(TY_SHORT);    ty_short->size = 2;  ty_short->align = 2;
    ty_int    = new_type(TY_INT);      ty_int->size   = 4;  ty_int->align   = 4;
    ty_long   = new_type(TY_LONG);     ty_long->size  = 8;  ty_long->align  = 8;
    ty_longlong = new_type(TY_LONGLONG); ty_longlong->size = 8; ty_longlong->align = 8;
    ty_float  = new_type(TY_FLOAT);    ty_float->size = 4;  ty_float->align = 4;
    ty_double = new_type(TY_DOUBLE);   ty_double->size= 8;  ty_double->align= 8;
}

static struct Type *ptr_to(struct Type *base) {
    struct Type *t = new_type(TY_PTR);
    t->base  = base;
    t->size  = 8;
    t->align = 8;
    return t;
}

static struct Type *array_of(struct Type *base, int n) {
    struct Type *t = new_type(TY_ARRAY);
    t->base       = base;
    t->array_size = n;
    t->size       = base->size * n;
    t->align      = base->align;
    return t;
}

static int type_size(struct Type *t) {
    switch (t->kind) {
        case TY_PTR:   return 8;
        case TY_ARRAY: return t->size;
        case TY_STRUCT:
        case TY_UNION: return t->size;
        default:       return t->size;
    }
}

/* ------------------------------------------------------------------ */
/* AST nodes                                                            */
/* ------------------------------------------------------------------ */

enum NodeKind {
    /* expressions */
    ND_INT, ND_FLOAT, ND_STR, ND_IDENT,
    ND_UNARY, ND_BINARY, ND_TERNARY,
    ND_ASSIGN, ND_CALL,
    ND_INDEX,    /* a[i] */
    ND_MEMBER,   /* a.b  */
    ND_ARROW,    /* a->b */
    ND_CAST,
    ND_PREINC, ND_PREDEC, ND_POSTINC, ND_POSTDEC,
    ND_ADDR, ND_DEREF,
    /* statements */
    ND_BLOCK,
    ND_IF,
    ND_WHILE, ND_DO_WHILE, ND_FOR,
    ND_RETURN, ND_BREAK, ND_CONTINUE,
    ND_GOTO, ND_LABEL,
    ND_EXPR_STMT,
    ND_DECL,     /* local variable declaration */
    /* top-level */
    ND_FUNC,
    ND_GVAR,
    ND_STRUCT_DEF,
    ND_PROTO,
};


struct Node {
    enum NodeKind kind;
    int      line;
    struct Type    *type;    /* resolved type (filled in symbol resolution) */

    /* literals */
    long long ival;
    double    fval;
    char    *sval;

    /* general children */
    struct Node    *lhs;
    struct Node    *rhs;
    struct Node    *cond;
    struct Node    *then;
    struct Node    *els;
    struct Node    *body;
    struct Node    *init;
    struct Node    *step;

    /* lists (null-terminated via next) */
    struct Node    *next;    /* sibling in a list */
    struct Node    *args;    /* call arguments */
    struct Node    *params;  /* func parameters */
    struct Node    *stmts;   /* block statements */

    /* operator */
    enum TokenKind op;

    /* name (variable, function, label, member) */
    char    *name;

    /* declaration */
    bool     is_local;
    bool     is_variadic; /* for ND_FUNC: has ... parameter */
    bool     is_float;    /* for ND_FLOAT: true=float, false=double */
    int      offset;  /* stack offset for locals (negative from rbp) */
};

static struct Node *new_node(enum NodeKind k) {
    struct Node *n = arena_alloc(sizeof(struct Node));
    n->kind = k;
    n->line = current_line;
    return n;
}

/* ------------------------------------------------------------------ */
/* Symbol table                                                         */
/* ------------------------------------------------------------------ */

struct Sym {
    char *name;
    struct Type *type;
    bool  is_local;
    int   offset;    /* local: rbp offset; global: 0 */
    struct Sym  *next;
};

struct Scope {
    struct Sym   *syms;
    struct Scope *parent;
};

/* struct/union tag table (flat, global) */
struct TagEntry {
    char      *tag;
    struct Type      *type;
    struct TagEntry  *next;
};

/* function prototype table */
struct ProtoEntry {
    char       *name;
    struct Type       *ret_type;
    int         arity;       /* -1 = unspecified (empty params) */
    bool        is_variadic; /* true if declared with ... */
    struct ProtoEntry *next;
};

static struct Scope      *current_scope = NULL;
static struct TagEntry   *tag_table     = NULL;
static struct ProtoEntry *proto_table   = NULL;

static void push_scope(void) {
    struct Scope *s = arena_alloc(sizeof(struct Scope));
    s->parent = current_scope;
    current_scope = s;
}

static void pop_scope(void) {
    current_scope = current_scope->parent;
}

static struct Sym *find_sym(const char *name) {
    for (struct Scope *s = current_scope; s; s = s->parent)
        for (struct Sym *sym = s->syms; sym; sym = sym->next)
            if (strcmp(sym->name, name) == 0) return sym;
    return NULL;
}

static struct Sym *add_sym(const char *name, struct Type *type, bool local, int offset) {
    struct Sym *sym = arena_alloc(sizeof(struct Sym));
    sym->name     = arena_strdup(name);
    sym->type     = type;
    sym->is_local = local;
    sym->offset   = offset;
    sym->next     = current_scope->syms;
    current_scope->syms = sym;
    return sym;
}

static struct Type *find_tag(const char *tag) {
    for (struct TagEntry *e = tag_table; e; e = e->next)
        if (strcmp(e->tag, tag) == 0) return e->type;
    return NULL;
}

static void add_tag(const char *tag, struct Type *type) {
    struct TagEntry *e = arena_alloc(sizeof(struct TagEntry));
    e->tag  = arena_strdup(tag);
    e->type = type;
    e->next = tag_table;
    tag_table = e;
}

static struct ProtoEntry *find_proto(const char *name) {
    for (struct ProtoEntry *e = proto_table; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

static void add_proto(const char *name, struct Type *ret, int arity, bool variadic) {
    struct ProtoEntry *e = arena_alloc(sizeof(struct ProtoEntry));
    e->name        = arena_strdup(name);
    e->ret_type    = ret;
    e->arity       = arity;
    e->is_variadic = variadic;
    e->next        = proto_table;
    proto_table    = e;
}

/* ------------------------------------------------------------------ */
/* Parser -- forward declarations                                       */
/* ------------------------------------------------------------------ */

static struct Node *parse_expr(void);
static struct Node *parse_stmt(void);
static struct Type *parse_type_spec(void);
static struct Type *parse_declarator(struct Type *base, char **name_out);
static struct Member *find_member(struct Type *t, const char *name);

/* ------------------------------------------------------------------ */
/* Parser -- types                                                      */
/* ------------------------------------------------------------------ */

static bool is_type_token(enum TokenKind k) {
    switch (k) {
        case TK_VOID: case TK_CHAR: case TK_SHORT: case TK_INT:
        case TK_LONG: case TK_FLOAT: case TK_DOUBLE:
        case TK_UNSIGNED: case TK_STRUCT: case TK_UNION:
        case TK_AUTO: case TK_EXTERN: case TK_REGISTER: case TK_STATIC:
            return true;
        default: return false;
    }
}

static struct Type *parse_struct_or_union(bool is_union) {
    char *tag = NULL;
    if (check(TK_IDENT)) {
        tag = advance()->sval;
        if (!check(TK_LBRACE)) {
            /* reference to existing tag — or forward reference */
            struct Type *t = tag ? find_tag(tag) : NULL;
            if (!t) {
                /* forward-declared incomplete type — create a placeholder */
                t = new_type(is_union ? TY_UNION : TY_STRUCT);
                t->tag = arena_strdup(tag);
                add_tag(tag, t);
            }
            return t;
        }
    }
    expect(TK_LBRACE, "expected '{' in struct/union");

    struct Type *t = new_type(is_union ? TY_UNION : TY_STRUCT);
    t->tag  = tag ? arena_strdup(tag) : NULL;

    /* register tag before parsing members to allow self-reference */
    if (tag) add_tag(tag, t);

    struct Member *head = NULL, *tail = NULL;
    int offset = 0, max_align = 1;

    while (!check(TK_RBRACE)) {
        struct Type *base = parse_type_spec();
        char *mname = NULL;
        struct Type *mtype = parse_declarator(base, &mname);

        /* reject nested struct/union definitions inline as members
           but allow pointer-to-struct */
        if ((mtype->kind == TY_STRUCT || mtype->kind == TY_UNION) && mname == NULL)
            die("nested struct/union definitions not allowed");

        expect(TK_SEMI, "expected ';' after struct member");

        struct Member *m = arena_alloc(sizeof(struct Member));
        m->name = mname ? arena_strdup(mname) : NULL;
        m->type = mtype;

        int msz  = type_size(mtype);
        int mal  = mtype->align ? mtype->align : 1;
        if (max_align < mal) max_align = mal;

        if (is_union) {
            m->offset = 0;
            if (msz > offset) offset = msz;
        } else {
            /* align offset */
            offset = (offset + mal - 1) & ~(mal - 1);
            m->offset = offset;
            offset += msz;
        }

        m->next = NULL;
        if (!head) head = tail = m;
        else { tail->next = m; tail = m; }
    }
    expect(TK_RBRACE, "expected '}' after struct/union body");

    t->members = head;
    /* total size: align up to max_align */
    t->size  = (offset + max_align - 1) & ~(max_align - 1);
    t->align = max_align;

    /* tag already registered above; if a placeholder existed, it's now filled */
    return t;
}

static struct Type *parse_type_spec(void) {
    /* skip storage class */
    while (check(TK_AUTO) || check(TK_EXTERN) ||
           check(TK_REGISTER) || check(TK_STATIC))
        advance();

    bool unsign = false;
    if (match(TK_UNSIGNED)) unsign = true;

    struct Type *base = NULL;
    switch (peek()->kind) {
        case TK_VOID:   advance(); base = ty_void;   break;
        case TK_CHAR:   advance(); base = ty_char;   break;
        case TK_SHORT:  advance(); base = ty_short;  break;
        case TK_INT:    advance(); base = ty_int;    break;
        case TK_LONG:
            advance();
            /* check for 'long long' */
            if (check(TK_LONG)) { advance(); base = ty_longlong; }
            else                 { base = ty_long; }
            break;
        case TK_FLOAT:  advance(); base = ty_float;  break;
        case TK_DOUBLE: advance(); base = ty_double; break;
        case TK_STRUCT: advance(); base = parse_struct_or_union(false); break;
        case TK_UNION:  advance(); base = parse_struct_or_union(true);  break;
        default:
            if (unsign) { base = ty_int; break; }
            die("expected type specifier, got token kind %d", peek()->kind);
    }

    if (unsign) {
        /* wrap in unsigned variant */
        struct Type *u = new_type(base->kind);
        *u = *base;
        u->is_unsigned = true;
        base = u;
    }
    return base;
}

/* parse pointer stars and array brackets wrapping a base type */
static struct Type *parse_declarator(struct Type *base, char **name_out) {
    /* pointers */
    int stars = 0;
    while (match(TK_STAR)) stars++;

    /* optional name */
    char *name = NULL;
    if (check(TK_IDENT)) name = advance()->sval;
    if (name_out) *name_out = name;

    /* array brackets -- only literal integer size */
    struct Type *t = base;
    for (int i = 0; i < stars; i++) t = ptr_to(t);

    /* wrap in void* if base was void and we have stars */
    /* (handled above naturally) */

    /* array brackets — support multiple dimensions */
    if (match(TK_LBRACKET)) {
        if (check(TK_RBRACKET)) {
            /* int a[] in parameter -- decay to pointer */
            advance();
            t = ptr_to(t);
        } else {
            if (!check(TK_INT_LIT))
                die("array size must be a literal integer");
            int sz = (int)advance()->ival;
            expect(TK_RBRACKET, "expected ']'");
            /* parse additional dimensions right-to-left so inner dim is base */
            while (match(TK_LBRACKET)) {
                if (!check(TK_INT_LIT))
                    die("array size must be a literal integer");
                int inner = (int)advance()->ival;
                expect(TK_RBRACKET, "expected ']'");
                t = array_of(t, inner);
            }
            t = array_of(t, sz);
        }
    }

    return t;
}

/* ------------------------------------------------------------------ */
/* Parser -- expressions                                                */
/* ------------------------------------------------------------------ */

static struct Node *parse_primary(void) {
    struct Token *t = peek();
    struct Node  *n;

    if (t->kind == TK_INT_LIT) {
        advance();
        n = new_node(ND_INT);
        n->ival = t->ival;
        return n;
    }
    if (t->kind == TK_CHAR_LIT) {
        advance();
        n = new_node(ND_INT);
        n->ival = t->ival;
        return n;
    }
    if (t->kind == TK_FLOAT_LIT) {
        advance();
        n = new_node(ND_FLOAT);
        n->fval = t->fval;
        n->is_float = t->is_float;
        return n;
    }
    if (t->kind == TK_STR_LIT) {
        advance();
        n = new_node(ND_STR);
        n->sval = t->sval;
        return n;
    }
    if (t->kind == TK_IDENT) {
        advance();
        n = new_node(ND_IDENT);
        n->name = t->sval;
        return n;
    }
    if (t->kind == TK_LPAREN) {
        advance();
        /* cast: (type) expr */
        if (is_type_token(peek()->kind)) {
            struct Type *cast_type = parse_type_spec();
            char *dummy = NULL;
            cast_type = parse_declarator(cast_type, &dummy);
            expect(TK_RPAREN, "expected ')' after cast type");
            n = new_node(ND_CAST);
            n->type = cast_type;
            n->lhs  = parse_expr(); /* should be unary, good enough */
            return n;
        }
        n = parse_expr();
        expect(TK_RPAREN, "expected ')'");
        return n;
    }
    die("expected expression");
    return NULL;
}

static struct Node *parse_postfix(void) {
    struct Node *n = parse_primary();
    for (;;) {
        if (match(TK_LBRACKET)) {
            struct Node *idx = parse_expr();
            expect(TK_RBRACKET, "expected ']'");
            struct Node *r = new_node(ND_INDEX);
            r->lhs = n; r->rhs = idx;
            n = r;
        } else if (match(TK_DOT)) {
            struct Token *m = expect(TK_IDENT, "expected member name");
            struct Node *r = new_node(ND_MEMBER);
            r->lhs  = n;
            r->name = m->sval;
            n = r;
        } else if (match(TK_ARROW)) {
            struct Token *m = expect(TK_IDENT, "expected member name");
            struct Node *r = new_node(ND_ARROW);
            r->lhs  = n;
            r->name = m->sval;
            n = r;
        } else if (check(TK_LPAREN)) {
            /* function call */
            advance();
            struct Node *call = new_node(ND_CALL);
            call->name = n->name; /* simple ident call */
            struct Node *arg_head = NULL, *arg_tail = NULL;
            int   argc = 0;
            while (!check(TK_RPAREN)) {
                struct Node *arg = parse_expr();
                arg->next = NULL;
                if (!arg_head) arg_head = arg_tail = arg;
                else { arg_tail->next = arg; arg_tail = arg; }
                argc++;
                if (!match(TK_COMMA)) break;
            }
            expect(TK_RPAREN, "expected ')' after arguments");
            call->args = arg_head;
            call->ival = argc;
            n = call;
        } else if (match(TK_PLUSPLUS)) {
            struct Node *r = new_node(ND_POSTINC); r->lhs = n; n = r;
        } else if (match(TK_MINUSMINUS)) {
            struct Node *r = new_node(ND_POSTDEC); r->lhs = n; n = r;
        } else {
            break;
        }
    }
    return n;
}

static struct Node *parse_unary(void) {
    if (match(TK_PLUSPLUS)) {
        struct Node *n = new_node(ND_PREINC); n->lhs = parse_unary(); return n;
    }
    if (match(TK_MINUSMINUS)) {
        struct Node *n = new_node(ND_PREDEC); n->lhs = parse_unary(); return n;
    }
    if (match(TK_AMP)) {
        struct Node *n = new_node(ND_ADDR);  n->lhs = parse_unary(); return n;
    }
    if (match(TK_STAR)) {
        struct Node *n = new_node(ND_DEREF); n->lhs = parse_unary(); return n;
    }
    if (match(TK_MINUS)) {
        struct Node *n = new_node(ND_UNARY); n->op = TK_MINUS; n->lhs = parse_unary(); return n;
    }
    if (match(TK_BANG)) {
        struct Node *n = new_node(ND_UNARY); n->op = TK_BANG;  n->lhs = parse_unary(); return n;
    }
    if (match(TK_TILDE)) {
        struct Node *n = new_node(ND_UNARY); n->op = TK_TILDE; n->lhs = parse_unary(); return n;
    }
    return parse_postfix();
}

static struct Node *parse_binary(int min_prec);

static int binary_prec(enum TokenKind k) {
    switch (k) {
        case TK_STAR: case TK_SLASH: case TK_PERCENT: return 12;
        case TK_PLUS: case TK_MINUS:                  return 11;
        case TK_LSHIFT: case TK_RSHIFT:               return 10;
        case TK_LT: case TK_GT: case TK_LEQ: case TK_GEQ: return 9;
        case TK_EQ: case TK_NEQ:                      return 8;
        case TK_AMP:                                   return 7;
        case TK_CARET:                                 return 6;
        case TK_PIPE:                                  return 5;
        case TK_AND:                                   return 4;
        case TK_OR:                                    return 3;
        default:                                       return -1;
    }
}

static struct Node *parse_binary(int min_prec) {
    struct Node *lhs = parse_unary();
    for (;;) {
        int prec = binary_prec(peek()->kind);
        if (prec < min_prec) break;
        enum TokenKind op = advance()->kind;
        struct Node *rhs = parse_binary(prec + 1);
        struct Node *n   = new_node(ND_BINARY);
        n->op  = op;
        n->lhs = lhs;
        n->rhs = rhs;
        lhs = n;
    }
    return lhs;
}

static bool is_assign_op(enum TokenKind k) {
    switch (k) {
        case TK_ASSIGN: case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN:
        case TK_STAR_ASSIGN: case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
        case TK_AMP_ASSIGN: case TK_PIPE_ASSIGN: case TK_CARET_ASSIGN:
        case TK_LSHIFT_ASSIGN: case TK_RSHIFT_ASSIGN:
            return true;
        default: return false;
    }
}

static struct Node *parse_expr(void) {
    struct Node *lhs = parse_binary(3);

    /* ternary */
    if (match(TK_QUESTION)) {
        struct Node *then = parse_expr();
        expect(TK_COLON, "expected ':' in ternary");
        struct Node *els = parse_expr();
        struct Node *n   = new_node(ND_TERNARY);
        n->cond = lhs; n->then = then; n->els = els;
        return n;
    }

    /* assignment */
    if (is_assign_op(peek()->kind)) {
        enum TokenKind op = advance()->kind;
        struct Node *rhs    = parse_expr();
        struct Node *n      = new_node(ND_ASSIGN);
        n->op  = op;
        n->lhs = lhs;
        n->rhs = rhs;
        return n;
    }

    return lhs;
}

/* ------------------------------------------------------------------ */
/* Parser -- statements                                                 */
/* ------------------------------------------------------------------ */

static struct Node *parse_block(void);

static struct Node *parse_stmt(void) {
    struct Token *t = peek();

    /* labeled statement */
    if (t->kind == TK_IDENT && peek2()->kind == TK_COLON) {
        char *lname = advance()->sval;
        advance(); /* colon */
        struct Node *n = new_node(ND_LABEL);
        n->name = lname;
        n->body = parse_stmt();
        return n;
    }

    if (match(TK_IF)) {
        expect(TK_LPAREN, "expected '(' after if");
        struct Node *cond = parse_expr();
        expect(TK_RPAREN, "expected ')' after if condition");
        struct Node *then = parse_stmt();
        struct Node *els  = NULL;
        if (match(TK_ELSE)) els = parse_stmt();
        struct Node *n = new_node(ND_IF);
        n->cond = cond; n->then = then; n->els = els;
        return n;
    }

    if (match(TK_WHILE)) {
        expect(TK_LPAREN, "expected '(' after while");
        struct Node *cond = parse_expr();
        expect(TK_RPAREN, "expected ')' after while condition");
        struct Node *body = parse_stmt();
        struct Node *n = new_node(ND_WHILE);
        n->cond = cond; n->body = body;
        return n;
    }

    if (match(TK_DO)) {
        struct Node *body = parse_stmt();
        expect(TK_WHILE, "expected 'while' after do body");
        expect(TK_LPAREN, "expected '('");
        struct Node *cond = parse_expr();
        expect(TK_RPAREN, "expected ')'");
        expect(TK_SEMI, "expected ';' after do-while");
        struct Node *n = new_node(ND_DO_WHILE);
        n->body = body; n->cond = cond;
        return n;
    }

    if (match(TK_FOR)) {
        expect(TK_LPAREN, "expected '(' after for");
        struct Node *init = NULL, *cond = NULL, *step = NULL;
        if (!check(TK_SEMI)) init = parse_expr();
        expect(TK_SEMI, "expected ';' in for");
        if (!check(TK_SEMI)) cond = parse_expr();
        expect(TK_SEMI, "expected ';' in for");
        if (!check(TK_RPAREN)) step = parse_expr();
        expect(TK_RPAREN, "expected ')' after for");
        struct Node *body = parse_stmt();
        struct Node *n = new_node(ND_FOR);
        n->init = init; n->cond = cond; n->step = step; n->body = body;
        return n;
    }

    if (match(TK_RETURN)) {
        struct Node *n = new_node(ND_RETURN);
        if (!check(TK_SEMI)) n->lhs = parse_expr();
        expect(TK_SEMI, "expected ';' after return");
        return n;
    }

    if (match(TK_BREAK)) {
        expect(TK_SEMI, "expected ';' after break");
        return new_node(ND_BREAK);
    }

    if (match(TK_CONTINUE)) {
        expect(TK_SEMI, "expected ';' after continue");
        return new_node(ND_CONTINUE);
    }

    if (match(TK_GOTO)) {
        struct Token *label = expect(TK_IDENT, "expected label after goto");
        expect(TK_SEMI, "expected ';' after goto");
        struct Node *n = new_node(ND_GOTO);
        n->name = label->sval;
        return n;
    }

    if (check(TK_LBRACE)) return parse_block();

    /* declaration or expression statement */
    if (is_type_token(t->kind)) {
        struct Type *base = parse_type_spec();
        char *name = NULL;
        struct Type *vtype = parse_declarator(base, &name);
        if (!name) die("expected variable name in declaration");

        /* block-scope static is rejected */
        /* (storage class already consumed in parse_type_spec,
           we'd need to thread it through -- simple approach: reject TK_STATIC
           before calling parse_type_spec in block context) */

        struct Node *n = new_node(ND_DECL);
        n->name = name;
        n->type = vtype;
        if (match(TK_ASSIGN)) n->init = parse_expr();
        expect(TK_SEMI, "expected ';' after declaration");
        return n;
    }

    /* expression statement */
    struct Node *n = new_node(ND_EXPR_STMT);
    n->lhs = parse_expr();
    expect(TK_SEMI, "expected ';'");
    return n;
}

static struct Node *parse_block(void) {
    expect(TK_LBRACE, "expected '{'");
    struct Node *block = new_node(ND_BLOCK);
    struct Node *head = NULL, *tail = NULL;
    while (!check(TK_RBRACE) && !check(TK_EOF)) {
        struct Node *s = parse_stmt();
        s->next = NULL;
        if (!head) head = tail = s;
        else { tail->next = s; tail = s; }
    }
    expect(TK_RBRACE, "expected '}'");
    block->stmts = head;
    return block;
}

/* ------------------------------------------------------------------ */
/* Parser -- top level                                                  */
/* ------------------------------------------------------------------ */

static struct Node *parse_program(void) {
    struct Node *head = NULL, *tail = NULL;

    while (!check(TK_EOF)) {
        struct Type *base = parse_type_spec();
        char *name = NULL;
        struct Type *dtype = parse_declarator(base, &name);

        if (!name) {
            /* could be a standalone struct/union definition: struct Foo { ... }; */
            if (check(TK_SEMI)) { advance(); continue; }
            die("expected name at top level");
        }

        /* prototype or function definition */
        if (check(TK_LPAREN) || dtype->kind == TY_FUNC) {
            /* parse parameter list */
            expect(TK_LPAREN, "expected '('");
            struct Node *params = NULL, *ptail = NULL;
            int   arity     = 0;
            bool  variadic  = false;

            if (!check(TK_RPAREN)) {
                do {
                    if (check(TK_RPAREN)) break;
                    /* variadic marker */
                    if (match(TK_ELLIPSIS)) { variadic = true; break; }
                    struct Type *pbase  = parse_type_spec();
                    char *pname  = NULL;
                    struct Type *ptype  = parse_declarator(pbase, &pname);
                    struct Node *param  = new_node(ND_DECL);
                    param->type  = ptype;
                    param->name  = pname;
                    param->next  = NULL;
                    if (!params) params = ptail = param;
                    else { ptail->next = param; ptail = param; }
                    arity++;
                } while (match(TK_COMMA));
            }
            expect(TK_RPAREN, "expected ')' after parameters");

            /* prototype */
            if (match(TK_SEMI)) {
                add_proto(name, base, arity, variadic);
                continue;
            }

            /* function definition */
            add_proto(name, base, arity, variadic);
            struct Node *fn = new_node(ND_FUNC);
            fn->name        = name;
            fn->type        = base;
            fn->params      = params;
            fn->ival        = arity;
            fn->is_variadic = variadic;
            fn->body        = parse_block();
            fn->next   = NULL;
            if (!head) head = tail = fn;
            else { tail->next = fn; tail = fn; }

        } else {
            /* global variable */
            struct Node *gv = new_node(ND_GVAR);
            gv->name = name;
            gv->type = dtype;
            if (match(TK_ASSIGN)) gv->init = parse_expr();
            expect(TK_SEMI, "expected ';' after global variable");
            gv->next = NULL;
            if (!head) head = tail = gv;
            else { tail->next = gv; tail = gv; }
        }
    }
    return head;
}

/* ------------------------------------------------------------------ */
/* Symbol resolution pass                                               */
/* ------------------------------------------------------------------ */

static int local_offset; /* tracks current frame size during resolution */

static void resolve_expr(struct Node *n);
static void resolve_stmt(struct Node *n);

static void resolve_expr(struct Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_IDENT: {
            struct Sym *sym = find_sym(n->name);
            if (!sym) {
                /* check proto table for functions */
                struct ProtoEntry *pe = find_proto(n->name);
                if (!pe) die("undeclared identifier '%s'", n->name);
            } else {
                n->is_local = sym->is_local;
                n->offset   = sym->offset;
                n->type     = sym->type;
            }
            break;
        }
        case ND_INT:
            n->type = ty_int;
            break;
        case ND_FLOAT:
            n->type = n->is_float ? ty_float : ty_double;
            break;
        case ND_STR:
            break;
        case ND_UNARY: case ND_PREINC: case ND_PREDEC:
        case ND_POSTINC: case ND_POSTDEC:
            resolve_expr(n->lhs);
            n->type = n->lhs->type;
            break;
        case ND_ADDR:
            resolve_expr(n->lhs);
            /* type is pointer to lhs type -- simplified, reuse lhs type */
            n->type = n->lhs->type;
            break;
        case ND_DEREF:
            resolve_expr(n->lhs);
            if (n->lhs->type && (n->lhs->type->kind == TY_PTR || n->lhs->type->kind == TY_ARRAY))
                n->type = n->lhs->type->base;
            else
                n->type = n->lhs->type;
            break;
        case ND_BINARY:
            resolve_expr(n->lhs);
            resolve_expr(n->rhs);
            /* result type: if ptr+int or ptr-int, keep pointer type */
            if (n->lhs->type && (n->lhs->type->kind == TY_PTR || n->lhs->type->kind == TY_ARRAY)
                && (n->op == TK_PLUS || n->op == TK_MINUS))
                n->type = n->lhs->type;
            else
                n->type = n->lhs->type;  /* default: lhs type */
            break;
        case ND_ASSIGN:
            resolve_expr(n->lhs);
            resolve_expr(n->rhs);
            n->type = n->lhs->type;
            break;
        case ND_TERNARY:
            resolve_expr(n->cond);
            resolve_expr(n->then);
            resolve_expr(n->els);
            break;
        case ND_CALL: {
            struct ProtoEntry *pe = find_proto(n->name);
            if (!pe) die("call to undeclared function '%s'", n->name);
            n->type = pe->ret_type;
            /* for variadic functions accept any number of args >= arity */
            if (!pe->is_variadic && pe->arity >= 0 && (int)n->ival != pe->arity)
                die("wrong number of arguments to '%s' (expected %d, got %d)",
                    n->name, pe->arity, (int)n->ival);
            for (struct Node *a = n->args; a; a = a->next) resolve_expr(a);
            break;
        }
        case ND_INDEX:
            resolve_expr(n->lhs);
            resolve_expr(n->rhs);
            /* element type: base of array or pointer */
            if (n->lhs->type) {
                if (n->lhs->type->kind == TY_ARRAY || n->lhs->type->kind == TY_PTR)
                    n->type = n->lhs->type->base;
                else
                    n->type = n->lhs->type;
            }
            break;
        case ND_MEMBER: {
            resolve_expr(n->lhs);
            struct Type *st = n->lhs->type;
            if (st && (st->kind == TY_STRUCT || st->kind == TY_UNION)) {
                struct Member *m = find_member(st, n->name);
                if (m) n->type = m->type;
            }
            break;
        }
        case ND_ARROW: {
            resolve_expr(n->lhs);
            struct Type *st = n->lhs->type;
            if (st && st->kind == TY_PTR) st = st->base;
            if (st && (st->kind == TY_STRUCT || st->kind == TY_UNION)) {
                struct Member *m = find_member(st, n->name);
                if (m) n->type = m->type;
            }
            break;
        }
        case ND_CAST:
            resolve_expr(n->lhs);
            break;
        default: break;
    }
}

static void resolve_stmt(struct Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK:
            push_scope();
            for (struct Node *s = n->stmts; s; s = s->next) resolve_stmt(s);
            pop_scope();
            break;
        case ND_DECL: {
            /* allocate stack slot */
            int sz = type_size(n->type);
            int al = n->type->align ? n->type->align : 8;
            local_offset = (local_offset + sz + al - 1) & ~(al - 1);
            n->offset    = -local_offset;
            n->is_local  = true;
            add_sym(n->name, n->type, true, n->offset);
            if (n->init) resolve_expr(n->init);
            break;
        }
        case ND_EXPR_STMT: resolve_expr(n->lhs);     break;
        case ND_RETURN:    resolve_expr(n->lhs);     break;
        case ND_IF:
            resolve_expr(n->cond);
            resolve_stmt(n->then);
            resolve_stmt(n->els);
            break;
        case ND_WHILE: case ND_DO_WHILE:
            resolve_expr(n->cond);
            resolve_stmt(n->body);
            break;
        case ND_FOR:
            resolve_expr(n->init);
            resolve_expr(n->cond);
            resolve_expr(n->step);
            resolve_stmt(n->body);
            break;
        case ND_LABEL:
            resolve_stmt(n->body);
            break;
        case ND_GOTO: case ND_BREAK: case ND_CONTINUE:
            break;
        default: break;
    }
}

static void resolve_func(struct Node *fn) {
    push_scope();
    local_offset = 0;

    /* add parameters to scope */
    int arg_regs_used = 0;
    for (struct Node *p = fn->params; p; p = p->next) {
        int sz = type_size(p->type);
        int al = p->type->align ? p->type->align : 8;
        local_offset = (local_offset + sz + al - 1) & ~(al - 1);
        p->offset    = -local_offset;
        p->is_local  = true;
        if (p->name)
            add_sym(p->name, p->type, true, p->offset);
        arg_regs_used++;
    }

    resolve_stmt(fn->body);
    /* align frame to 16 bytes */
    fn->ival = (local_offset + 15) & ~15;
    pop_scope();
}

/* ------------------------------------------------------------------ */
/* Code generation                                                      */
/* ------------------------------------------------------------------ */

static FILE *out;
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
static char *current_func_end;  /* label for return */
static char *break_label;
static char *continue_label;

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

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    va_end(ap);
}

/* size suffix for AT&T asm */
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
static struct Member *find_member(struct Type *t, const char *name) {
    for (struct Member *m = t->members; m; m = m->next)
        if (m->name && strcmp(m->name, name) == 0) return m;
    die("no member '%s' in struct/union", name);
    return NULL;
}

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
            /* arrays decay to pointer (their address); scalars get loaded */
            if (n->type && n->type->kind == TY_ARRAY) {
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
            /* detect pointer arithmetic: ptr +/- int needs scaling */
            bool lhs_is_ptr = n->lhs->type &&
                              (n->lhs->type->kind == TY_PTR ||
                               n->lhs->type->kind == TY_ARRAY);
            bool rhs_is_ptr = n->rhs->type &&
                              (n->rhs->type->kind == TY_PTR ||
                               n->rhs->type->kind == TY_ARRAY);
            int ptr_scale = 1;
            if ((n->op == TK_PLUS || n->op == TK_MINUS) && lhs_is_ptr && !rhs_is_ptr) {
                struct Type *base = n->lhs->type->base;
                if (base) ptr_scale = type_size(base);
            }
            (void)rhs_is_ptr;

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
               - integer/pointer args: rdi, rsi, rdx, rcx, r8, r9
               - float/double args:    xmm0..xmm7
               - stack args: right-to-left
               - for variadic: al = number of XMM registers used */
            int int_arg_idx = 0;
            int xmm_arg_idx = 0;

            /* count stack args: args beyond first 6 int + 8 xmm go on stack */
            /* simplified: track separately */
            int stack_argc = 0;
            {
                int ii = 0, xi = 0;
                for (int i = 0; i < argc; i++) {
                    if (is_float_type(args[i]->type)) {
                        if (xi < 8) xi++; else stack_argc++;
                    } else {
                        if (ii < 6) ii++; else stack_argc++;
                    }
                }
            }

            int pad = (stack_argc % 2 != 0) ? 1 : 0;
            if (pad) emit("  subq $8, %%rsp");

            /* push stack args right-to-left */
            {
                int ii = 0, xi = 0;
                for (int i = argc - 1; i >= 0; i--) {
                    bool fa = is_float_type(args[i]->type);
                    int goes_to_reg = fa ? (xi < 8) : (ii < 6);
                    /* count in forward order first to know register indices */
                    (void)goes_to_reg;
                }
                /* recount forward to find which go to stack */
                int fwd_ii = 0, fwd_xi = 0;
                bool is_stack[64] = {false};
                for (int i = 0; i < argc; i++) {
                    if (is_float_type(args[i]->type)) {
                        if (fwd_xi < 8) fwd_xi++; else is_stack[i] = true;
                    } else {
                        if (fwd_ii < 6) fwd_ii++; else is_stack[i] = true;
                    }
                }
                for (int i = argc - 1; i >= 0; i--) {
                    if (!is_stack[i]) continue;
                    gen_expr(args[i]);
                    if (is_float_type(args[i]->type)) {
                        emit("  subq $8, %%rsp");
                        if (args[i]->type->kind == TY_FLOAT) {
                            /* widen float to double for stack pass (System V) */
                            emit("  cvtss2sd %%xmm0, %%xmm0");
                            emit("  movsd %%xmm0, (%%rsp)");
                        } else {
                            emit("  movsd %%xmm0, (%%rsp)");
                        }
                    } else {
                        emit("  pushq %%rax");
                    }
                }
            }

            /* evaluate register args and temporarily spill all to stack,
               then load into the proper registers */
            /* Spill integer reg args first (right-to-left), then XMM (right-to-left) */
            {
                int fwd_ii = 0, fwd_xi = 0;
                bool is_stack[64] = {false};
                for (int i = 0; i < argc; i++) {
                    if (is_float_type(args[i]->type)) {
                        if (fwd_xi < 8) fwd_xi++; else is_stack[i] = true;
                    } else {
                        if (fwd_ii < 6) fwd_ii++; else is_stack[i] = true;
                    }
                }
                int_arg_idx = 0; xmm_arg_idx = 0;
                /* evaluate and push all register args right-to-left as 8-byte slots */
                for (int i = argc - 1; i >= 0; i--) {
                    if (is_stack[i]) continue;
                    gen_expr(args[i]);
                    if (is_float_type(args[i]->type)) {
                        emit("  subq $8, %%rsp");
                        if (args[i]->type->kind == TY_FLOAT)
                            emit("  movss %%xmm0, (%%rsp)");
                        else
                            emit("  movsd %%xmm0, (%%rsp)");
                    } else {
                        emit("  pushq %%rax");
                    }
                }
                /* pop into registers in forward order */
                for (int i = 0; i < argc; i++) {
                    if (is_stack[i]) continue;
                    if (is_float_type(args[i]->type)) {
                        if (args[i]->type->kind == TY_FLOAT)
                            emit("  movss (%%rsp), %%xmm%d", xmm_arg_idx);
                        else
                            emit("  movsd (%%rsp), %%xmm%d", xmm_arg_idx);
                        emit("  addq $8, %%rsp");
                        xmm_arg_idx++;
                    } else {
                        emit("  popq %%%s", arg_regs[int_arg_idx]);
                        int_arg_idx++;
                    }
                }
            }

            /* for variadic: al = number of XMM args */
            struct ProtoEntry *pe = find_proto(n->name);
            bool is_var = pe && pe->is_variadic;
            if (is_var)
                emit("  movb $%d, %%al", xmm_arg_idx);
            else
                emit("  xorq %%rax, %%rax");
            emit("  callq %s", n->name);

            /* clean up stack args + padding */
            int cleanup = (stack_argc + pad) * 8;
            if (cleanup) emit("  addq $%d, %%rsp", cleanup);

            /* float return: xmm0 already has the result; int return: rax */
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
                gen_expr(n->init);
                int sz = n->type ? type_size(n->type) : 8;
                if      (sz == 8) emit("  movq  %%rax, %d(%%rbp)", n->offset);
                else if (sz == 4) emit("  movl  %%eax, %d(%%rbp)", n->offset);
                else if (sz == 2) emit("  movw  %%ax,  %d(%%rbp)", n->offset);
                else              emit("  movb  %%al,  %d(%%rbp)", n->offset);
            }
            return;
        case ND_RETURN:
            if (n->lhs) gen_expr(n->lhs);
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

static void gen_func(struct Node *fn) {
    char end_label[64];
    sprintf(end_label, ".Lret_%s", fn->name);
    current_func_end = arena_strdup(end_label);

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
    for (struct Node *p = fn->params; p; p = p->next) {
        if (is_float_type(p->type)) {
            if (xmm_idx >= 8) break;
            if (p->type->kind == TY_FLOAT)
                emit("  movss %%xmm%d, %d(%%rbp)", xmm_idx, p->offset);
            else
                emit("  movsd %%xmm%d, %d(%%rbp)", xmm_idx, p->offset);
            xmm_idx++;
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
    emit("  movq %%rbp, %%rsp");
    emit("  popq %%rbp");
    emit("  ret");
}

static void gen_gvar(struct Node *gv) {
    if (gv->init) {
        emit("  .data");
        emit("  .globl %s", gv->name);
        emit("%s:", gv->name);
        int sz = type_size(gv->type);
        if (gv->init->kind == ND_FLOAT) {
            /* float or double literal initializer */
            if (sz == 4) {
                union { float f; uint32_t u; } u;
                u.f = (float)gv->init->fval;
                emit("  .long %u", u.u);
            } else {
                union { double d; uint64_t u; } u;
                u.d = gv->init->fval;
                emit("  .quad %llu", (unsigned long long)u.u);
            }
        } else if (gv->init->kind == ND_INT) {
            switch (sz) {
                case 1: emit("  .byte %lld",  gv->init->ival); break;
                case 2: emit("  .short %lld", gv->init->ival); break;
                case 4: emit("  .long %lld",  gv->init->ival); break;
                default:emit("  .quad %lld",  gv->init->ival); break;
            }
        } else {
            emit("  .zero %d", sz);
        }
    } else {
        emit("  .bss");
        emit("  .globl %s", gv->name);
        emit("%s:", gv->name);
        emit("  .zero %d", type_size(gv->type));
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: krc <input.c>\n");
        return 1;
    }

    init_types();

    /* builtin declarations: malloc and free */
    push_scope();
    add_proto("malloc", ptr_to(ty_void), 1, false);
    add_proto("free",   ty_void,         1, false);

    src     = read_file(argv[1]);
    src_pos = 0;
    out     = stdout;

    lex_all();
    struct Node *program = parse_program();

    /* emit GNU-stack note to mark stack non-executable */
    emit("  .section .note.GNU-stack,\"\",@progbits");

    /* resolve and generate */
    /* first pass: register all globals in the symbol table */
    for (struct Node *n = program; n; n = n->next) {
        if (n->kind == ND_GVAR)
            add_sym(n->name, n->type, false, 0);
    }
    /* second pass: resolve and emit */
    for (struct Node *n = program; n; n = n->next) {
        if (n->kind == ND_FUNC) {
            resolve_func(n);
            gen_func(n);
        } else if (n->kind == ND_GVAR) {
            gen_gvar(n);
        }
    }

    return 0;
}