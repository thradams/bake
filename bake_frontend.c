#include "bake.h"
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

FILE *out;  /* output stream, set by main() */


/* ------------------------------------------------------------------ */
/* Arena allocator                                                      */
/* ------------------------------------------------------------------ */

#define ARENA_SIZE (32 * 1024 * 1024)  /* 32 MB */

static char  arena_buf[ARENA_SIZE];
static size_t arena_top = 0;

void *arena_alloc(size_t n) {
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

char *arena_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* Error reporting                                                      */
/* ------------------------------------------------------------------ */

int current_line = 1;

void die(const char *fmt, ...) {
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

void lex_all(void) {
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





struct Type *ty_void;
struct Type *ty_char;
struct Type *ty_short;
struct Type *ty_int;
struct Type *ty_long;
struct Type *ty_longlong;
struct Type *ty_float;
struct Type *ty_double;

static struct Type *new_type(enum TypeKind k) {
    struct Type *t = arena_alloc(sizeof(struct Type));
    t->kind = k;
    return t;
}

void init_types(void) {
    ty_void   = new_type(TY_VOID);     ty_void->size  = 0;  ty_void->align  = 1;
    ty_char   = new_type(TY_CHAR);     ty_char->size  = 1;  ty_char->align  = 1;
    ty_short  = new_type(TY_SHORT);    ty_short->size = 2;  ty_short->align = 2;
    ty_int    = new_type(TY_INT);      ty_int->size   = 4;  ty_int->align   = 4;
    ty_long   = new_type(TY_LONG);     ty_long->size  = 8;  ty_long->align  = 8;
    ty_longlong = new_type(TY_LONGLONG); ty_longlong->size = 8; ty_longlong->align = 8;
    ty_float  = new_type(TY_FLOAT);    ty_float->size = 4;  ty_float->align = 4;
    ty_double = new_type(TY_DOUBLE);   ty_double->size= 8;  ty_double->align= 8;
}

struct Type *ptr_to(struct Type *base) {
    struct Type *t = new_type(TY_PTR);
    t->base  = base;
    t->size  = 8;
    t->align = 8;
    return t;
}

struct Type *array_of(struct Type *base, int n) {
    struct Type *t = new_type(TY_ARRAY);
    t->base       = base;
    t->array_size = n;
    t->size       = base->size * n;
    t->align      = base->align;
    return t;
}

/* Build a TY_FUNC type used as the pointee of a function pointer */
struct Type *func_type(struct Type *ret, struct Type **params, int nparams, bool variadic) {
    struct Type *t = new_type(TY_FUNC);
    t->ret           = ret;
    t->size          = 8;
    t->align         = 8;
    t->nparams       = nparams;
    t->func_variadic = variadic;
    if (nparams > 0) {
        t->param_types = arena_alloc(sizeof(struct Type *) * (size_t)nparams);
        for (int i = 0; i < nparams; i++)
            t->param_types[i] = params[i];
    }
    return t;
}

int type_size(struct Type *t) {
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




static struct Node *new_node(enum NodeKind k) {
    struct Node *n = arena_alloc(sizeof(struct Node));
    n->kind = k;
    n->line = current_line;
    return n;
}

/* ------------------------------------------------------------------ */
/* Symbol table                                                         */
/* ------------------------------------------------------------------ */



/* struct/union tag table (flat, global) */

/* function prototype table */


/* internal frontend structures (not exposed to codegen) */
struct Scope {
    struct Sym   *syms;
    struct Scope *parent;
};

struct TagEntry {
    char           *tag;
    struct Type    *type;
    struct TagEntry *next;
};

static struct Scope      *current_scope = NULL;
static struct TagEntry   *tag_table     = NULL;
static struct ProtoEntry *proto_table   = NULL;

void push_scope(void) {
    struct Scope *s = arena_alloc(sizeof(struct Scope));
    s->parent = current_scope;
    current_scope = s;
}

void pop_scope(void) {
    current_scope = current_scope->parent;
}

struct Sym *find_sym(const char *name) {
    for (struct Scope *s = current_scope; s; s = s->parent)
        for (struct Sym *sym = s->syms; sym; sym = sym->next)
            if (strcmp(sym->name, name) == 0) return sym;
    return NULL;
}

struct Sym *add_sym(const char *name, struct Type *type, bool local, int offset) {
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

struct ProtoEntry *find_proto(const char *name) {
    for (struct ProtoEntry *e = proto_table; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

void add_proto(const char *name, struct Type *ret, int arity, bool variadic) {
    struct ProtoEntry *e = arena_alloc(sizeof(struct ProtoEntry));
    e->name        = arena_strdup(name);
    e->ret_type    = ret;
    e->arity       = arity;
    e->is_variadic = variadic;
    e->next        = proto_table;
    proto_table    = e;
}

/* ------------------------------------------------------------------ */

struct Member *find_member(struct Type *t, const char *name) {
    for (struct Member *m = t->members; m; m = m->next)
        if (m->name && strcmp(m->name, name) == 0) return m;
    die("no member '%s' in struct/union", name);
    return NULL;
}

/* Parser -- forward declarations                                       */
/* ------------------------------------------------------------------ */

static struct Node *parse_expr(void);
static struct Node *parse_stmt(void);
static struct Type *parse_type_spec(void);
static struct Type *parse_declarator(struct Type *base, char **name_out);
static struct Node *parse_initializer(struct Type *type);

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

/* Forward declaration needed because parse_fp_params calls parse_type_spec */
static struct Type *parse_type_spec(void);

/* Parse a function-pointer parameter list: (type, type, ...) already past '(' */
static struct Type *parse_fp_params(struct Type *ret_type, char **name_out_ignored) {
    /* caller has already consumed the opening '(' of the param list */
    struct Type *param_buf[64];
    int nparams = 0;
    bool variadic = false;
    if (!check(TK_RPAREN)) {
        do {
            if (match(TK_ELLIPSIS)) { variadic = true; break; }
            struct Type *pb = parse_type_spec();
            char *dummy = NULL;
            /* parse_declarator forward-declared below; use recursive call */
            extern struct Type *parse_declarator(struct Type *, char **);
            pb = parse_declarator(pb, &dummy);
            if (nparams < 64) param_buf[nparams++] = pb;
            if (!check(TK_COMMA)) break;
        } while (match(TK_COMMA));
    }
    expect(TK_RPAREN, "expected ')' after function pointer params");
    (void)name_out_ignored;
    return func_type(ret_type, param_buf, nparams, variadic);
}

/* parse pointer stars and array brackets wrapping a base type.
   Also handles grouped declarators for function pointers:
     ret (*name)(params)          -- pointer to function
     ret (*name[N])(params)       -- array of pointers to function
*/
static struct Type *parse_declarator(struct Type *base, char **name_out) {
    /* leading pointer stars (for plain pointers: int *p) */
    int stars = 0;
    while (match(TK_STAR)) stars++;

    /* grouped declarator: '(' follows the stars (or immediately) and the
       next token is '*' -- this is (*name...) syntax */
    if (check(TK_LPAREN) && peek2()->kind == TK_STAR) {
        advance();          /* consume '(' */
        int inner_stars = 0;
        while (match(TK_STAR)) inner_stars++;

        /* name inside parens */
        char *name = NULL;
        if (check(TK_IDENT)) name = advance()->sval;
        if (name_out) *name_out = name;

        /* optional array dimension(s) inside the parens: (*arr[N]) */
        int arr_dims[8];
        int ndims = 0;
        while (check(TK_LBRACKET)) {
            advance();
            if (!check(TK_INT_LIT)) die("array size must be a literal integer");
            arr_dims[ndims++] = (int)advance()->ival;
            expect(TK_RBRACKET, "expected ']'");
        }

        expect(TK_RPAREN, "expected ')' in grouped declarator");

        /* What follows determines the type:
           (params)  -> function pointer (or array of fp if ndims > 0)
           nothing   -> plain pointer (or array of ptr if ndims > 0)      */
        struct Type *t;
        if (check(TK_LPAREN)) {
            /* function pointer: build TY_FUNC then wrap in ptr */
            advance(); /* consume '(' of param list */
            struct Type *ft = parse_fp_params(base, NULL);
            /* wrap in pointer(s) */
            t = ptr_to(ft);
            for (int i = 1; i < inner_stars; i++) t = ptr_to(t);
        } else {
            /* plain grouped pointer, e.g. int (*p) or int (*p)[N] */
            t = base;
            for (int i = 0; i < inner_stars; i++) t = ptr_to(t);
            /* trailing array brackets after the closing ')' */
            while (check(TK_LBRACKET)) {
                advance();
                if (!check(TK_INT_LIT)) die("array size must be a literal integer");
                int sz = (int)advance()->ival;
                expect(TK_RBRACKET, "expected ']'");
                t = array_of(t, sz);
            }
        }

        /* wrap in array dimensions collected inside the parens (right-to-left) */
        for (int i = ndims - 1; i >= 0; i--)
            t = array_of(t, arr_dims[i]);

        /* outer pointer stars (rare, e.g. int (**pp)(void)) */
        for (int i = 0; i < stars; i++) t = ptr_to(t);

        return t;
    }

    /* --- plain declarator: stars already counted --- */
    char *name = NULL;
    if (check(TK_IDENT)) name = advance()->sval;
    if (name_out) *name_out = name;

    struct Type *t = base;
    for (int i = 0; i < stars; i++) t = ptr_to(t);

    /* array brackets */
    if (match(TK_LBRACKET)) {
        if (check(TK_RBRACKET)) {
            advance();
            t = ptr_to(t);
        } else {
            if (!check(TK_INT_LIT))
                die("array size must be a literal integer");
            int sz = (int)advance()->ival;
            expect(TK_RBRACKET, "expected ']'");
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
            /* function call: store both name (if ident) and callee node.
               resolve_expr decides whether this is a direct or indirect call. */
            advance();
            struct Node *call = new_node(ND_CALL);
            call->callee = n;                                      /* always set */
            call->name   = (n->kind == ND_IDENT) ? n->name : NULL; /* hint only */
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
        if (match(TK_ASSIGN)) n->init = parse_initializer(vtype);
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
/* Initializer parser                                                   */
/* Handles scalar, array, and struct initializers recursively.          */
/* Returns either a plain expr node (scalar) or ND_INIT_LIST.           */
/* ------------------------------------------------------------------ */

static struct Node *parse_initializer(struct Type *type) {
    /* brace-enclosed: { ... } */
    if (match(TK_LBRACE)) {
        struct Node *list = new_node(ND_INIT_LIST);
        list->type = type;
        struct Node *head = NULL, *tail = NULL;
        int seq_idx = 0;   /* sequential element counter */

        while (!check(TK_RBRACE) && !check(TK_EOF)) {
            struct Node *elem = new_node(ND_INIT_LIST);
            elem->ival = -1;   /* default: sequential */
            elem->name = NULL;

            /* designated initializer: .field = value  (struct/union) */
            if (check(TK_DOT)) {
                advance();
                struct Token *fname = expect(TK_IDENT, "expected field name after '.'");
                elem->name = fname->sval;
                expect(TK_ASSIGN, "expected '=' after designator");
            }
            /* designated initializer: [N] = value  (array) */
            else if (check(TK_LBRACKET)) {
                advance();
                struct Node *idx_expr = parse_expr();
                if (idx_expr->kind != ND_INT)
                    die("array designator must be an integer constant");
                elem->ival = idx_expr->ival;
                seq_idx = (int)elem->ival;
                expect(TK_RBRACKET, "expected ']' after array designator");
                expect(TK_ASSIGN, "expected '=' after designator");
            }

            /* determine element type for nested init */
            struct Type *elem_type = NULL;
            if (type) {
                if (type->kind == TY_ARRAY)
                    elem_type = type->base;
                else if ((type->kind == TY_STRUCT || type->kind == TY_UNION) && elem->name) {
                    struct Member *m = NULL;
                    for (struct Member *mb = type->members; mb; mb = mb->next)
                        if (mb->name && strcmp(mb->name, elem->name) == 0) { m = mb; break; }
                    if (m) elem_type = m->type;
                }
            }

            /* recursively parse the value */
            struct Node *val;
            if (check(TK_LBRACE))
                val = parse_initializer(elem_type);
            else
                val = parse_expr();

            elem->lhs = val;
            if (elem->ival < 0) elem->ival = seq_idx;
            seq_idx++;

            /* append element to list */
            elem->next = NULL;
            if (!head) head = tail = elem;
            else { tail->next = elem; tail = elem; }

            if (!match(TK_COMMA)) break;   /* allow trailing comma */
        }

        expect(TK_RBRACE, "expected '}' at end of initializer");
        list->stmts = head;
        return list;
    }

    /* scalar: just an expression */
    return parse_expr();
}

/* ------------------------------------------------------------------ */
/* Parser -- top level                                                  */
/* ------------------------------------------------------------------ */

struct Node *parse_program(void) {
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
                /* also emit ND_PROTO node so backends can see declarations */
                struct Node *proto_node = new_node(ND_PROTO);
                proto_node->name        = name;
                proto_node->type        = base;
                proto_node->is_variadic = variadic;
                proto_node->ival        = arity;
                if (!head) head = tail = proto_node;
                else { tail->next = proto_node; tail = proto_node; }
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
            if (match(TK_ASSIGN)) gv->init = parse_initializer(dtype);
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
static void resolve_init(struct Node *n, struct Type *type);

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
            {
                bool lp = n->lhs->type && (n->lhs->type->kind == TY_PTR || n->lhs->type->kind == TY_ARRAY);
                bool rp = n->rhs->type && (n->rhs->type->kind == TY_PTR || n->rhs->type->kind == TY_ARRAY);
                if (lp && rp && n->op == TK_MINUS)
                    n->type = ty_long;          /* ptr - ptr -> ptrdiff_t */
                else if (lp && (n->op == TK_PLUS || n->op == TK_MINUS))
                    n->type = n->lhs->type;     /* ptr +/- int -> ptr */
                else if (rp && n->op == TK_PLUS)
                    n->type = n->rhs->type;     /* int + ptr -> ptr */
                else
                    n->type = n->lhs->type;     /* default: lhs type */
            }
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
            /* Decide direct vs indirect:
               - If name matches a proto → direct call, clear callee
               - Otherwise → indirect call through a variable/expression   */
            if (n->name && find_proto(n->name)) {
                /* direct call */
                n->callee = NULL;
                struct ProtoEntry *pe = find_proto(n->name);
                n->type = pe->ret_type;
                if (!pe->is_variadic && pe->arity >= 0 && (int)n->ival != pe->arity)
                    die("wrong number of arguments to '%s' (expected %d, got %d)",
                        n->name, pe->arity, (int)n->ival);
            } else {
                /* indirect call through function pointer variable/expression */
                resolve_expr(n->callee);
                struct Type *ct = n->callee->type;
                /* unwrap: ptr-to-func */
                if (ct && ct->kind == TY_PTR && ct->base && ct->base->kind == TY_FUNC)
                    n->type = ct->base->ret;
                else if (ct && ct->kind == TY_FUNC)
                    n->type = ct->ret;
                else if (n->name)
                    die("call to undeclared function '%s'", n->name);
                n->name = NULL; /* mark as indirect */
            }
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
        case ND_INIT_LIST:
            resolve_init(n, n->type);
            break;
        default: break;
    }
}

/* Resolve all expressions inside an initializer (scalar or ND_INIT_LIST). */
static void resolve_init(struct Node *n, struct Type *type) {
    if (!n) return;
    if (n->kind == ND_INIT_LIST) {
        n->type = type;
        int seq = 0;
        for (struct Node *e = n->stmts; e; e = e->next) {
            struct Type *et = NULL;
            if (type) {
                if (type->kind == TY_ARRAY) {
                    et = type->base;
                } else if (type->kind == TY_STRUCT || type->kind == TY_UNION) {
                    if (e->name) {
                        for (struct Member *m = type->members; m; m = m->next)
                            if (m->name && strcmp(m->name, e->name) == 0) { et = m->type; break; }
                    } else {
                        int idx = 0;
                        for (struct Member *m = type->members; m; m = m->next) {
                            if (!m->name) continue;
                            if (idx++ == seq) { et = m->type; break; }
                        }
                    }
                }
            }
            resolve_init(e->lhs, et);
            seq++;
        }
        return;
    }
    resolve_expr(n);
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
            if (n->init) resolve_init(n->init, n->type);
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

/* forward declarations for struct-return helpers (defined in codegen section) */

bool is_struct_type(struct Type *t) {
    return t && (t->kind == TY_STRUCT || t->kind == TY_UNION);
}

bool struct_fits_regs(struct Type *t) {
    return is_struct_type(t) && type_size(t) <= 16;
}

bool struct_needs_sret(struct Type *t) {
    return is_struct_type(t) && type_size(t) > 16;
}

void resolve_func(struct Node *fn) {
    push_scope();
    local_offset = 0;

    /* sret: reserve a stack slot for the hidden pointer if returning large struct */
    if (struct_needs_sret(fn->type)) {
        local_offset += 8;
        fn->sret_offset = -local_offset;
    } else {
        fn->sret_offset = 0;
    }

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
    /* Align frame to 16 bytes.
     * At main entry rsp % 16 = 8 (callq pushed return addr).
     * pushq rbp makes rsp % 16 = 0.
     * subq $frame_size keeps it 0 iff frame_size % 16 == 0.
     * So round up to next multiple of 16. */
    fn->ival = (local_offset + 15) & ~15;
    pop_scope();
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

/* wasm backend hooks -- only linked when building bake_wasm */
void wasm_emit_module_open(struct Node *program)  __attribute__((weak));
void wasm_emit_module_close(struct Node *program) __attribute__((weak));

int main(int argc, char **argv) {
    const char *input_path  = NULL;
    const char *output_path = NULL;
    const char *target      = "x86";
    const char *elf_mode    = NULL;  /* "exe"/"dyn"/"obj" → invoke xasm */   /* default: x86-64 assembly */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -o requires an argument\n");
                return 1;
            }
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-target") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -target requires an argument (x86 or wasm)\n");
                return 1;
            }
            target = argv[++i];
            if (strcmp(target, "x86") != 0 && strcmp(target, "wasm") != 0) {
                fprintf(stderr, "error: unknown target '%s' (supported: x86, wasm)\n", target);
                return 1;
            }
        } else if (strcmp(argv[i], "-exe") == 0) {
            elf_mode = "exe";
        } else if (strcmp(argv[i], "-dyn") == 0) {
            elf_mode = "dyn";
        } else if (strcmp(argv[i], "-obj") == 0) {
            elf_mode = "obj";
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            if (input_path) {
                fprintf(stderr, "error: multiple input files not supported\n");
                return 1;
            }
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "usage: bake [-target x86|wasm] [-exe|-dyn|-obj] [-o output] <input.c>\n");
        fprintf(stderr, "  -dyn   link via xasm (dynamically linked ELF, default)\n");
        fprintf(stderr, "  -exe   link via xasm (statically linked ELF)\n");
        fprintf(stderr, "  -obj   assemble via xasm (relocatable object)\n");
        return 1;
    }

    bool is_wasm = (strcmp(target, "wasm") == 0);
    if (is_wasm && !wasm_emit_module_open) {
        fprintf(stderr, "error: wasm backend not linked (use bake_wasm)\n");
        return 1;
    }

    init_types();

    /* builtin declarations: malloc and free */
    push_scope();
    add_proto("malloc", ptr_to(ty_void), 1, false);
    add_proto("free",   ty_void,         1, false);

    src     = read_file(input_path);
    src_pos = 0;

    if (output_path) {
        out = fopen(output_path, "w");
        if (!out) {
            fprintf(stderr, "error: cannot open output file '%s'\n", output_path);
            return 1;
        }
    } else {
        out = stdout;
    }

    lex_all();
    struct Node *program = parse_program();

    /* first pass: register all globals in the symbol table */
    for (struct Node *n = program; n; n = n->next) {
        if (n->kind == ND_GVAR)
            add_sym(n->name, n->type, false, 0);
    }

    if (is_wasm) {
        /* wasm: open module, emit all funcs/gvars, close module */
        wasm_emit_module_open(program);
        for (struct Node *n = program; n; n = n->next) {
            if (n->kind == ND_FUNC) {
                resolve_func(n);
                gen_func(n);
            } else if (n->kind == ND_GVAR) {
                gen_gvar(n);
            }
        }
        wasm_emit_module_close(program);
    } else {
        /* x86: emit GNU-stack note then generate assembly */
        emit("  .section .note.GNU-stack,\"\",@progbits");
        for (struct Node *n = program; n; n = n->next) {
            if (n->kind == ND_FUNC) {
                resolve_func(n);
                gen_func(n);
            } else if (n->kind == ND_GVAR) {
                gen_gvar(n);
            }
        }
    }

    if (output_path) fclose(out);

    /* ----------------------------------------------------------------
     * If an ELF mode was requested (-exe/-dyn/-obj), invoke xasm to
     * assemble the .s into an ELF.  xasm is looked up next to bake.
     * ---------------------------------------------------------------- */
    if (elf_mode && !is_wasm && output_path) {
        /* Find xasm in same directory as bake */
        char xasm_path[4096];
        const char *slash = strrchr(argv[0], '/');
        if (slash) {
            size_t dlen = (size_t)(slash - argv[0]) + 1;
            memcpy(xasm_path, argv[0], dlen);
            strcpy(xasm_path + dlen, "xasm");
        } else {
            strcpy(xasm_path, "./xasm");
        }

        /* Derive ELF output: strip .s suffix from the assembly file */
        char elf_out[4096];
        size_t olen = strlen(output_path);
        if (olen > 2 && strcmp(output_path + olen - 2, ".s") == 0) {
            memcpy(elf_out, output_path, olen - 2);
            elf_out[olen - 2] = '\0';
        } else {
            snprintf(elf_out, sizeof(elf_out), "%s.elf", output_path);
        }

        /* fork + exec xasm */
        char *xargs[] = { xasm_path, (char *)output_path,
                          elf_out,   (char *)elf_mode, NULL };
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            execv(xasm_path, xargs);
            xargs[0] = "xasm";          /* fallback: find on PATH */
            execvp("xasm", xargs);
            fprintf(stderr, "error: cannot exec xasm\n");
            _exit(1);
        }
        int wst = 0;
        waitpid(pid, &wst, 0);
        if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
            fprintf(stderr, "error: xasm failed\n");
            return 1;
        }
        chmod(elf_out, 0755);
        fprintf(stderr, "output: %s\n", elf_out);
    }

    return 0;
}
