#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_LINE 512
#define MAX_SYMBOLS 200
#define MAX_RELOCS 200
#define MAX_INCLUDE_DEPTH 10

typedef struct {
    int is_mem;
    int base_reg;
    int index_reg;
    int scale;
    int disp;
    char disp_symbol[32];
    int has_disp_symbol;
} MemOperand;

typedef struct {
    char name[32];
    uint32_t value;
    int defined;
} Symbol;

typedef struct {
    uint32_t offset;
    char symbol[32];
    int type;
} Reloc;

Symbol symbols[MAX_SYMBOLS];
Reloc relocs[MAX_RELOCS];
int sym_count = 0, reloc_count = 0;
uint8_t *code = NULL, *data = NULL;
uint32_t code_size = 0, code_capacity = 1024;
uint32_t data_size = 0, data_capacity = 1024;
int current_section = 0;
int include_depth = 0;
int error_count = 0;
int line_number = 0;
int if_depth = 0;
int if_stack[MAX_INCLUDE_DEPTH];

// Forward declarations
int parse_imm(const char *s, int32_t *val);
int is_number(const char *s);
int get_symbol(const char *name);
void emit_byte(uint8_t b);
void emit_word(uint16_t w);
void emit_dword(uint32_t d);
void emit_qword(uint64_t q);
void add_reloc(uint32_t offset, const char *symbol, int type);
int parse_reg(const char *s);
int get_reg_size(const char *s);
int is_64bit_reg(const char *s);
int needs_rex(const char *reg);
void emit_rex(int w, int r, int x, int b);
void emit_rex_w(int r, int x, int b);
int parse_mem_operand(const char *s, MemOperand *mem);
void emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm);
void emit_sib(uint8_t scale, uint8_t index, uint8_t base);
void emit_mem_operand(MemOperand *mem, uint8_t reg_opcode);
void assemble_line(char *line);
void resolve_relocs(void);
void write_elf(const char *filename);
void write_obj(const char *filename);
static uint32_t elf_text_va(void);
static uint32_t elf_data_va(void);

int get_symbol(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(symbols[i].name, name) == 0) return i;
    if (sym_count < MAX_SYMBOLS) {
        strncpy(symbols[sym_count].name, name,
                sizeof(symbols[sym_count].name) - 1);
        symbols[sym_count].name[sizeof(symbols[sym_count].name) - 1] = '\0';
        symbols[sym_count].value = 0;
        symbols[sym_count].defined = 0;
        return sym_count++;
    }
    fprintf(stderr, "error: symbol table full (max %d), cannot add '%s'\n",
            MAX_SYMBOLS, name);
    return -1;
}

void emit_byte(uint8_t b) {
    if (current_section == 0) {
        if (code_size >= code_capacity) {
            code_capacity *= 2;
            uint8_t *tmp = realloc(code, code_capacity);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            code = tmp;
        }
        code[code_size++] = b;
    } else {
        if (data_size >= data_capacity) {
            data_capacity *= 2;
            uint8_t *tmp = realloc(data, data_capacity);
            if (!tmp) { fprintf(stderr, "Out of memory\n"); exit(1); }
            data = tmp;
        }
        data[data_size++] = b;
    }
}

void emit_word(uint16_t w) {
    emit_byte(w & 0xff);
    emit_byte((w >> 8) & 0xff);
}

void emit_dword(uint32_t d) {
    emit_byte(d & 0xff);
    emit_byte((d >> 8) & 0xff);
    emit_byte((d >> 16) & 0xff);
    emit_byte((d >> 24) & 0xff);
}

void emit_qword(uint64_t q) {
    emit_byte(q & 0xff);
    emit_byte((q >> 8) & 0xff);
    emit_byte((q >> 16) & 0xff);
    emit_byte((q >> 24) & 0xff);
    emit_byte((q >> 32) & 0xff);
    emit_byte((q >> 40) & 0xff);
    emit_byte((q >> 48) & 0xff);
    emit_byte((q >> 56) & 0xff);
}

void add_reloc(uint32_t offset, const char *symbol, int type) {
    if (reloc_count < MAX_RELOCS) {
        (void)offset;
        relocs[reloc_count].offset = (current_section == 0 ? code_size : data_size);

        /* Strip ELF dynamic-linking suffixes: @PLT, @GOT, @GOTPCREL, etc.
         * These are used in GAS output for shared-library calls but we
         * assemble as a static binary, so just use the bare symbol name. */
        char clean[64];
        strncpy(clean, symbol, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        char *at = strchr(clean, '@');
        if (at) *at = '\0';

        strncpy(relocs[reloc_count].symbol, clean,
                sizeof(relocs[reloc_count].symbol) - 1);
        relocs[reloc_count].symbol[sizeof(relocs[reloc_count].symbol)-1] = '\0';
        relocs[reloc_count].type = type | (current_section << 8);
        reloc_count++;
    }
}

int parse_reg(const char *s) {
    // 64-bit registers
    if (strcmp(s, "rax") == 0) return 0;
    if (strcmp(s, "rcx") == 0) return 1;
    if (strcmp(s, "rdx") == 0) return 2;
    if (strcmp(s, "rbx") == 0) return 3;
    if (strcmp(s, "rsp") == 0) return 4;
    if (strcmp(s, "rbp") == 0) return 5;
    if (strcmp(s, "rsi") == 0) return 6;
    if (strcmp(s, "rdi") == 0) return 7;
    if (strcmp(s, "r8") == 0) return 8;
    if (strcmp(s, "r9") == 0) return 9;
    if (strcmp(s, "r10") == 0) return 10;
    if (strcmp(s, "r11") == 0) return 11;
    if (strcmp(s, "r12") == 0) return 12;
    if (strcmp(s, "r13") == 0) return 13;
    if (strcmp(s, "r14") == 0) return 14;
    if (strcmp(s, "r15") == 0) return 15;
    // 32-bit registers
    if (strcmp(s, "eax") == 0) return 0;
    if (strcmp(s, "ecx") == 0) return 1;
    if (strcmp(s, "edx") == 0) return 2;
    if (strcmp(s, "ebx") == 0) return 3;
    if (strcmp(s, "esp") == 0) return 4;
    if (strcmp(s, "ebp") == 0) return 5;
    if (strcmp(s, "esi") == 0) return 6;
    if (strcmp(s, "edi") == 0) return 7;
    if (strcmp(s, "r8d") == 0) return 8;
    if (strcmp(s, "r9d") == 0) return 9;
    if (strcmp(s, "r10d") == 0) return 10;
    if (strcmp(s, "r11d") == 0) return 11;
    if (strcmp(s, "r12d") == 0) return 12;
    if (strcmp(s, "r13d") == 0) return 13;
    if (strcmp(s, "r14d") == 0) return 14;
    if (strcmp(s, "r15d") == 0) return 15;
    // 16-bit registers
    if (strcmp(s, "ax") == 0) return 0;
    if (strcmp(s, "cx") == 0) return 1;
    if (strcmp(s, "dx") == 0) return 2;
    if (strcmp(s, "bx") == 0) return 3;
    if (strcmp(s, "sp") == 0) return 4;
    if (strcmp(s, "bp") == 0) return 5;
    if (strcmp(s, "si") == 0) return 6;
    if (strcmp(s, "di") == 0) return 7;
    if (strcmp(s, "r8w") == 0) return 8;
    if (strcmp(s, "r9w") == 0) return 9;
    if (strcmp(s, "r10w") == 0) return 10;
    if (strcmp(s, "r11w") == 0) return 11;
    if (strcmp(s, "r12w") == 0) return 12;
    if (strcmp(s, "r13w") == 0) return 13;
    if (strcmp(s, "r14w") == 0) return 14;
    if (strcmp(s, "r15w") == 0) return 15;
    // 8-bit registers
    if (strcmp(s, "al") == 0) return 0;
    if (strcmp(s, "cl") == 0) return 1;
    if (strcmp(s, "dl") == 0) return 2;
    if (strcmp(s, "bl") == 0) return 3;
    if (strcmp(s, "ah") == 0) return 4;
    if (strcmp(s, "ch") == 0) return 5;
    if (strcmp(s, "dh") == 0) return 6;
    if (strcmp(s, "bh") == 0) return 7;
    if (strcmp(s, "r8b") == 0) return 8;
    if (strcmp(s, "r9b") == 0) return 9;
    if (strcmp(s, "r10b") == 0) return 10;
    if (strcmp(s, "r11b") == 0) return 11;
    if (strcmp(s, "r12b") == 0) return 12;
    if (strcmp(s, "r13b") == 0) return 13;
    if (strcmp(s, "r14b") == 0) return 14;
    if (strcmp(s, "r15b") == 0) return 15;
    return -1;
}

int get_reg_size(const char *s) {
    // 64-bit
    if (strcmp(s, "rax") == 0 || strcmp(s, "rcx") == 0 || strcmp(s, "rdx") == 0 ||
        strcmp(s, "rbx") == 0 || strcmp(s, "rsp") == 0 || strcmp(s, "rbp") == 0 ||
        strcmp(s, "rsi") == 0 || strcmp(s, "rdi") == 0 ||
        strcmp(s, "r8") == 0 || strcmp(s, "r9") == 0 || strcmp(s, "r10") == 0 ||
        strcmp(s, "r11") == 0 || strcmp(s, "r12") == 0 || strcmp(s, "r13") == 0 ||
        strcmp(s, "r14") == 0 || strcmp(s, "r15") == 0) return 8;
    // 32-bit
    if (strcmp(s, "eax") == 0 || strcmp(s, "ecx") == 0 || strcmp(s, "edx") == 0 ||
        strcmp(s, "ebx") == 0 || strcmp(s, "esp") == 0 || strcmp(s, "ebp") == 0 ||
        strcmp(s, "esi") == 0 || strcmp(s, "edi") == 0 ||
        strcmp(s, "r8d") == 0 || strcmp(s, "r9d") == 0 || strcmp(s, "r10d") == 0 ||
        strcmp(s, "r11d") == 0 || strcmp(s, "r12d") == 0 || strcmp(s, "r13d") == 0 ||
        strcmp(s, "r14d") == 0 || strcmp(s, "r15d") == 0) return 4;
    // 16-bit
    if (strcmp(s, "ax") == 0 || strcmp(s, "cx") == 0 || strcmp(s, "dx") == 0 ||
        strcmp(s, "bx") == 0 || strcmp(s, "sp") == 0 || strcmp(s, "bp") == 0 ||
        strcmp(s, "si") == 0 || strcmp(s, "di") == 0 ||
        strcmp(s, "r8w") == 0 || strcmp(s, "r9w") == 0 || strcmp(s, "r10w") == 0 ||
        strcmp(s, "r11w") == 0 || strcmp(s, "r12w") == 0 || strcmp(s, "r13w") == 0 ||
        strcmp(s, "r14w") == 0 || strcmp(s, "r15w") == 0) return 2;
    // 8-bit
    if (strcmp(s, "al") == 0 || strcmp(s, "cl") == 0 || strcmp(s, "dl") == 0 ||
        strcmp(s, "bl") == 0 || strcmp(s, "ah") == 0 || strcmp(s, "ch") == 0 ||
        strcmp(s, "dh") == 0 || strcmp(s, "bh") == 0 ||
        strcmp(s, "r8b") == 0 || strcmp(s, "r9b") == 0 || strcmp(s, "r10b") == 0 ||
        strcmp(s, "r11b") == 0 || strcmp(s, "r12b") == 0 || strcmp(s, "r13b") == 0 ||
        strcmp(s, "r14b") == 0 || strcmp(s, "r15b") == 0) return 1;
    return 4;
}

int is_64bit_reg(const char *s) {
    return get_reg_size(s) == 8;
}

int needs_rex(const char *reg) {
    return parse_reg(reg) >= 8;
}

void emit_rex(int w, int r, int x, int b) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    emit_byte(rex);
}

// Always emit REX.W for 64-bit operations
void emit_rex_w(int r, int x, int b) {
    emit_rex(1, r, x, b);
}

int parse_mem_operand(const char *s, MemOperand *mem) {
    memset(mem, 0, sizeof(MemOperand));
    mem->base_reg = -1;
    mem->index_reg = -1;
    mem->scale = 1;
    
    if (s[0] != '[') return 0;
    s++;
    
    char buf[64];
    int i = 0;
    
    while (*s && *s != ']') {
        if (*s == '+' || *s == '-' || *s == '*') {
            buf[i] = '\0';
            if (i > 0) {
                int reg = parse_reg(buf);
                if (reg != -1) {
                    if (mem->base_reg == -1) mem->base_reg = reg;
                    else mem->index_reg = reg;
                } else {
                    /* Use parse_imm only for pure numeric literals.
                       Symbol names (labels) must become disp_symbol so
                       they are resolved via reloc — even if the symbol is
                       already defined with a tagged value like 0x80000000. */
                    int32_t val;
                    if (is_number(buf) && parse_imm(buf, &val)) {
                        mem->disp = val;
                    } else {
                        strncpy(mem->disp_symbol, buf,
                                sizeof(mem->disp_symbol) - 1);
                        mem->disp_symbol[sizeof(mem->disp_symbol)-1] = '\0';
                        mem->has_disp_symbol = 1;
                    }
                }
            }
            i = 0;
            if (*s == '*') {
                s++;
                while (*s && *s != ']' && *s != '+') buf[i++] = *s++;
                buf[i] = '\0';
                mem->scale = atoi(buf);
                i = 0;
            }
        } else {
            if (i < 63) buf[i++] = *s;
        }
        s++;
    }
    
    if (i > 0) {
        buf[i] = '\0';
        int reg = parse_reg(buf);
        if (reg != -1) {
            if (mem->base_reg == -1) mem->base_reg = reg;
            else mem->index_reg = reg;
        } else {
            int32_t val;
            if (is_number(buf) && parse_imm(buf, &val)) {
                mem->disp = val;
            } else {
                strncpy(mem->disp_symbol, buf,
                        sizeof(mem->disp_symbol) - 1);
                mem->disp_symbol[sizeof(mem->disp_symbol)-1] = '\0';
                mem->has_disp_symbol = 1;
            }
        }
    }
    
    mem->is_mem = 1;
    return 1;
}

void emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    emit_byte((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

void emit_sib(uint8_t scale, uint8_t index, uint8_t base) {
    emit_byte((scale << 6) | ((index & 7) << 3) | (base & 7));
}

void emit_mem_operand(MemOperand *mem, uint8_t reg_opcode) {
    int needs_sib = 0;
    int needs_disp32 = 0;
    int needs_disp8 = 0;
    
    // Determine addressing mode
    if (mem->index_reg != -1) needs_sib = 1;
    if (mem->base_reg == 4) needs_sib = 1; // esp needs SIB
    
    // Calculate displacement
    int32_t disp = mem->disp;
    if (mem->has_disp_symbol) {
        needs_disp32 = 1;
        disp = 0;
    } else if (disp != 0 || mem->base_reg == -1) {
        if (disp >= -128 && disp <= 127) {
            needs_disp8 = 1;
        } else {
            needs_disp32 = 1;
        }
    }
    
    uint8_t mod = 0;
    if (needs_disp32) mod = 2;
    else if (needs_disp8) mod = 1;
    else if (mem->base_reg == -1) mod = 0;
    
    // Special case: [disp32] only - use RIP-relative in 64-bit mode
    if (mem->base_reg == -1 && mem->index_reg == -1) {
        emit_modrm(0, reg_opcode, 5); // [rip+disp32]
        if (mem->has_disp_symbol) {
            add_reloc(code_size, mem->disp_symbol, 0);
            emit_dword(0);
        } else {
            emit_dword(disp);
        }
        return;
    }
    
    if (needs_sib) {
        uint8_t scale = 0;
        if (mem->scale == 2) scale = 1;
        else if (mem->scale == 4) scale = 2;
        else if (mem->scale == 8) scale = 3;
        
        uint8_t index = mem->index_reg == -1 ? 4 : mem->index_reg;
        uint8_t base = mem->base_reg == -1 ? 5 : mem->base_reg;
        
        emit_modrm(mod, reg_opcode, 4); // SIB
        emit_sib(scale, index, base);
        
        if (needs_disp32) {
            if (mem->has_disp_symbol) {
                add_reloc(code_size, mem->disp_symbol, 0);
                emit_dword(0);
            } else {
                emit_dword(disp);
            }
        } else if (needs_disp8) {
            emit_byte(disp);
        }
    } else {
        emit_modrm(mod, reg_opcode, mem->base_reg);
        
        if (mem->base_reg == 5 && mod == 0) {
            // ebp with mod 0 needs disp32
            if (mem->has_disp_symbol) {
                add_reloc(code_size, mem->disp_symbol, 0);
                emit_dword(0);
            } else {
                emit_dword(disp);
            }
        } else if (needs_disp32) {
            if (mem->has_disp_symbol) {
                add_reloc(code_size, mem->disp_symbol, 0);
                emit_dword(0);
            } else {
                emit_dword(disp);
            }
        } else if (needs_disp8) {
            emit_byte(disp);
        }
    }
}

int is_number(const char *s) {
    if (s[0] == '-') s++;
    while (*s) {
        if (*s >= '0' && *s <= '9') { s++; continue; }
        if ((*s == 'x' || *s == 'X') && s > (s - (s[0]=='-'?2:1))) {
            s++;
            while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) s++;
            return (*s == 0);
        }
        return 0;
    }
    return 1;
}

int parse_imm(const char *s, int32_t *val) {
    if (!s) return 0;
    
    // Check if it's a simple number
    if (is_number(s)) {
        if (strstr(s, "0x") || strstr(s, "0X")) {
            *val = (int32_t)strtol(s, NULL, 0);
        } else {
            *val = atoi(s);
        }
        return 1;
    }
    
    // Check if it's a symbol
    int sym_idx = get_symbol(s);
    if (sym_idx >= 0 && symbols[sym_idx].defined) {
        *val = symbols[sym_idx].value;
        return 1;
    }
    
    // Check for operators
    if (strchr(s, '+') || strchr(s, '-') || strchr(s, '*') || 
        strchr(s, '/') || strchr(s, '<') || strchr(s, '>') ||
        strchr(s, '&') || strchr(s, '|') || strchr(s, '^')) {
        
        char expr[128];
        strncpy(expr, s, sizeof(expr) - 1);
        expr[sizeof(expr) - 1] = '\0';
        
        // Parse with simple left-to-right evaluation
        int32_t result = 0;
        char buf[128];
        int buf_idx = 0;
        int have_val = 0;
        char last_op = '+';
        
        for (int i = 0; expr[i]; i++) {
            if (expr[i] == '+' || expr[i] == '-' || expr[i] == '*' ||
                expr[i] == '/' || expr[i] == '&' || expr[i] == '|' ||
                expr[i] == '^') {
                if (buf_idx > 0) {
                    buf[buf_idx] = '\0';
                    int32_t term_val;
                    if (is_number(buf)) {
                        term_val = (strstr(buf, "0x") || strstr(buf, "0X")) ? 
                                   (int32_t)strtol(buf, NULL, 0) : atoi(buf);
                    } else {
                        int idx = get_symbol(buf);
                        if (idx >= 0 && symbols[idx].defined) {
                            term_val = symbols[idx].value;
                        } else {
                            return 0;
                        }
                    }
                    if (!have_val) {
                        result = term_val;
                        have_val = 1;
                    } else {
                        switch (last_op) {
                            case '+': result += term_val; break;
                            case '-': result -= term_val; break;
                            case '*': result *= term_val; break;
                            case '/': result /= term_val; break;
                            case '&': result &= term_val; break;
                            case '|': result |= term_val; break;
                            case '^': result ^= term_val; break;
                        }
                    }
                    buf_idx = 0;
                }
                last_op = expr[i];
            } else {
                buf[buf_idx++] = expr[i];
            }
        }
        
        if (buf_idx > 0) {
            buf[buf_idx] = '\0';
            int32_t term_val;
            if (is_number(buf)) {
                term_val = (strstr(buf, "0x") || strstr(buf, "0X")) ? 
                           (int32_t)strtol(buf, NULL, 0) : atoi(buf);
            } else {
                int idx = get_symbol(buf);
                if (idx >= 0 && symbols[idx].defined) {
                    term_val = symbols[idx].value;
                } else {
                    return 0;
                }
            }
            if (!have_val) {
                result = term_val;
            } else {
                switch (last_op) {
                    case '+': result += term_val; break;
                    case '-': result -= term_val; break;
                    case '*': result *= term_val; break;
                    case '/': result /= term_val; break;
                    case '&': result &= term_val; break;
                    case '|': result |= term_val; break;
                    case '^': result ^= term_val; break;
                }
            }
        }
        
        *val = result;
        return 1;
    }
    
    *val = 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 * AT&T (GAS) syntax support                                           *
 *                                                                     *
 * Normalises a raw line from AT&T syntax into the Intel syntax that   *
 * the rest of the assembler already understands, in-place.            *
 *                                                                     *
 * Transformations applied:                                            *
 *  - strip '%' register prefix                                        *
 *  - strip '$' immediate prefix                                       *
 *  - strip size suffixes (q/l/w/b) from known mnemonics              *
 *  - swap AT&T src,dst → Intel dst,src operand order                 *
 *  - convert AT&T memory syntax disp(%base,%idx,sc) → [base+idx*sc+d]*
 *  - sym(%rip) → label reference (strip %rip)                        *
 *  - .string → .asciz                                                 *
 *  - .rodata / .note.* / unknown .section flags → handled gracefully  *
 * ------------------------------------------------------------------ */

/* Strip leading/trailing whitespace in-place, return pointer */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}


/* Convert AT&T memory operand like "-8(%rbp)" or "(%rsp)"
 * or "sym(%rip)" into Intel bracket form "[rbp-8]" / "[rsp]" / "sym"
 * Writes result into out (caller must supply buf of sufficient size). */
static void convert_att_mem(const char *s, char *out, size_t outsz) {
    /* Check for disp?(%reg...) pattern */
    const char *paren = strchr(s, '(');
    if (!paren) {
        /* Plain symbol or number — pass through */
        snprintf(out, outsz, "%s", s);
        return;
    }

    /* Extract displacement (everything before '(') */
    char disp[64] = {0};
    size_t displen = (size_t)(paren - s);
    if (displen > 0 && displen < sizeof(disp))
        memcpy(disp, s, displen);

    /* Extract interior: base , index , scale */
    const char *inner_start = paren + 1;
    const char *inner_end   = strchr(inner_start, ')');
    if (!inner_end) { snprintf(out, outsz, "%s", s); return; }

    char inner[64] = {0};
    size_t innerlen = (size_t)(inner_end - inner_start);
    if (innerlen >= sizeof(inner)) innerlen = sizeof(inner) - 1;
    memcpy(inner, inner_start, innerlen);

    /* Tokenise inner by comma */
    char base[32]  = {0};
    char idx[32]   = {0};
    char scale[8]  = {0};

    char tmp[64];
    strncpy(tmp, inner, sizeof(tmp) - 1); tmp[sizeof(tmp)-1] = '\0';
    char *_saveptr = NULL;
    char *tok = strtok_r(tmp, ",", &_saveptr);
    if (tok) { strncpy(base,  trim(tok), sizeof(base)  - 1); tok = strtok_r(NULL, ",", &_saveptr); }
    if (tok) { strncpy(idx,   trim(tok), sizeof(idx)   - 1); tok = strtok_r(NULL, ",", &_saveptr); }
    if (tok) { strncpy(scale, trim(tok), sizeof(scale) - 1); }

    /* Strip % from registers */
    if (base[0]  == '%') memmove(base,  base  + 1, strlen(base));
    if (idx[0]   == '%') memmove(idx,   idx   + 1, strlen(idx));

    /* sym(%rip) → [rip+sym]  (keep as RIP-relative bracket so lea/mov encoder sees it) */
    if (strcasecmp(base, "rip") == 0 && idx[0] == '\0') {
        if (disp[0])
            snprintf(out, outsz, "[rip+%s]", disp);
        else
            snprintf(out, outsz, "[rip]");
        return;
    }

    /* Build Intel bracket form */
    char intel[128] = "[";
    int  need_plus  = 0;

    if (base[0]) { strncat(intel, base,  sizeof(intel) - strlen(intel) - 1); need_plus = 1; }
    if (idx[0]) {
        if (need_plus) strncat(intel, "+", sizeof(intel) - strlen(intel) - 1);
        strncat(intel, idx, sizeof(intel) - strlen(intel) - 1);
        if (scale[0] && strcmp(scale, "1") != 0) {
            strncat(intel, "*", sizeof(intel) - strlen(intel) - 1);
            strncat(intel, scale, sizeof(intel) - strlen(intel) - 1);
        }
        need_plus = 1;
    }
    if (disp[0] && strcmp(disp, "0") != 0) {
        /* disp may be negative, e.g. "-8" — include sign as-is */
        if (need_plus && disp[0] != '-')
            strncat(intel, "+", sizeof(intel) - strlen(intel) - 1);
        strncat(intel, disp, sizeof(intel) - strlen(intel) - 1);
    }
    if (!base[0] && !idx[0]) strncat(intel, "0", sizeof(intel) - strlen(intel) - 1);
    strncat(intel, "]", sizeof(intel) - strlen(intel) - 1);
    snprintf(out, outsz, "%s", intel);
}

/* Return 1 if mnemonic is known to take two operands in AT&T order
 * (src, dst) that need to be swapped to Intel (dst, src). */
static int att_is_two_op(const char *m) {
    static const char *two_ops[] = {
        "mov","add","sub","and","or","xor","cmp","test",
        "lea","adc","sbb","imul","xchg","movsx","movzx",
        "movs","movb","movw","movl","movq",
        "cmpxchg","bsf","bsr","bt","btc","btr","bts",NULL
    };
    for (int i = 0; two_ops[i]; i++)
        if (strcasecmp(m, two_ops[i]) == 0) return 1;
    return 0;
}

/* Strip size suffix (q/l/w/b) from an AT&T mnemonic if present.
 * Only strips when the base mnemonic (without suffix) is known. */
static void strip_size_suffix(char *op) {
    size_t len = strlen(op);
    if (len < 2) return;
    char last = op[len - 1];
    if (last != 'q' && last != 'l' && last != 'w' && last != 'b') return;

    /* Temporarily null-terminate without suffix */
    op[len - 1] = '\0';

    static const char *known[] = {
        "mov","push","pop","add","sub","and","or","xor","cmp","test",
        "lea","call","ret","jmp","inc","dec","neg","not","mul","imul",
        "div","idiv","xchg","movsx","movzx","adc","sbb","rol","ror",
        "shl","sal","shr","sar","leave","enter","xor","cmpxchg",
        /* AT&T compound zero/sign-extend: movzb, movzw, movsb, movsw */
        "movzb","movzw","movsb","movsw","movzl","movsl",NULL
    };
    for (int i = 0; known[i]; i++) {
        if (strcasecmp(op, known[i]) == 0) {
            /* Map AT&T compound extend mnemonics to Intel equivalents */
            if (strncasecmp(op, "movzb", 5) == 0 || strncasecmp(op, "movzw", 5) == 0 ||
                strncasecmp(op, "movzl", 5) == 0)
                strcpy(op, "movzx");
            else if (strncasecmp(op, "movsb", 5) == 0 || strncasecmp(op, "movsw", 5) == 0 ||
                     strncasecmp(op, "movsl", 5) == 0)
                strcpy(op, "movsx");
            return; /* keep stripped */
        }
    }
    /* Not a known mnemonic without suffix — restore */
    op[len - 1] = last;
}

/* Normalise one operand token: strip % and $, convert mem syntax.
 * Returns pointer to a static buffer — caller copies immediately. */
static const char *norm_operand(const char *tok) {
    static char buf[128];
    if (!tok) return NULL;

    /* Strip leading whitespace */
    while (*tok == ' ' || *tok == '\t') tok++;

    if (tok[0] == '$') {
        /* Immediate */
        snprintf(buf, sizeof(buf), "%s", tok + 1);
        /* strip % from register-sized immediates that are actually symbols */
    } else if (tok[0] == '%') {
        /* Register */
        snprintf(buf, sizeof(buf), "%s", tok + 1);
    } else if (strchr(tok, '(')) {
        /* Memory operand */
        convert_att_mem(tok, buf, sizeof(buf));
    } else {
        /* Symbol, label, or plain number */
        snprintf(buf, sizeof(buf), "%s", tok);
    }
    return buf;
}

/* Split a raw operand string (after the mnemonic) around the first
 * top-level comma (respecting parentheses).  Writes into src/dst. */
static void split_operands(const char *ops, char *first, size_t fsz,
                            char *second, size_t ssz) {
    first[0]  = '\0';
    second[0] = '\0';
    if (!ops || !*ops) return;

    int depth = 0;
    const char *p = ops;
    const char *comma = NULL;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0) { comma = p; break; }
        p++;
    }
    if (!comma) {
        strncpy(first, ops, fsz - 1); first[fsz - 1] = '\0';
        return;
    }
    size_t n = (size_t)(comma - ops);
    if (n >= fsz) n = fsz - 1;
    memcpy(first, ops, n); first[n] = '\0';
    strncpy(second, comma + 1, ssz - 1); second[ssz - 1] = '\0';
    /* trim second */
    char *s = second;
    while (*s == ' ' || *s == '\t') memmove(s, s+1, strlen(s));
}

/* Top-level AT&T → Intel line normaliser.
 * Rewrites 'line' in-place (line must be writable and have capacity
 * MAX_LINE).  Returns 1 if the line should be assembled, 0 to skip. */
static int normalize_att_line(char *line) {
    /* Work on a copy for tokenising */
    char work[MAX_LINE];
    strncpy(work, line, MAX_LINE - 1);
    work[MAX_LINE - 1] = '\0';

    /* Strip inline comments (# or ; not inside strings) */
    for (char *c = work; *c; c++) {
        if (*c == '#' || *c == ';') { *c = '\0'; break; }
    }
    char *trimmed = trim(work);
    if (!trimmed || !*trimmed) { line[0] = '\0'; return 1; }

    /* ---- Directive handling ---------------------------------------- */

    /* .section with arbitrary flags: just map to section name */
    if (strncasecmp(trimmed, ".section", 8) == 0) {
        char *rest = trim(trimmed + 8);
        /* Grab first token (section name) */
        char secname[64];
        int i = 0;
        while (*rest && *rest != ',' && *rest != ' ' && *rest != '\t' &&
               i < (int)sizeof(secname) - 1)
            secname[i++] = *rest++;
        secname[i] = '\0';
        /* Map section names */
        if (strncmp(secname, ".text", 5) == 0)
            snprintf(line, MAX_LINE, ".text");
        else if (strncmp(secname, ".data", 5) == 0 ||
                 strncmp(secname, ".rodata", 7) == 0 ||
                 strncmp(secname, ".bss", 4) == 0)
            snprintf(line, MAX_LINE, ".data");
        else
            line[0] = '\0'; /* unknown section → ignore */
        return 1;
    }

    /* .rodata → .data */
    if (strcasecmp(trimmed, ".rodata") == 0) {
        snprintf(line, MAX_LINE, ".data"); return 1;
    }

    /* .string → .asciz */
    if (strncasecmp(trimmed, ".string", 7) == 0) {
        char *rest = trim(trimmed + 7);
        snprintf(line, MAX_LINE, ".asciz %s", rest);
        return 1;
    }

    /* .long → .dword */
    if (strncasecmp(trimmed, ".long", 5) == 0) {
        char *rest = trim(trimmed + 5);
        snprintf(line, MAX_LINE, ".dword %s", rest);
        return 1;
    }

    /* .short → .word */
    if (strncasecmp(trimmed, ".short", 6) == 0) {
        char *rest = trim(trimmed + 6);
        snprintf(line, MAX_LINE, ".word %s", rest);
        return 1;
    }

    /* .size, .type, .file, .ident, .cfi_* → ignore */
    if (strncasecmp(trimmed, ".size", 5)  == 0 ||
        strncasecmp(trimmed, ".type", 5)  == 0 ||
        strncasecmp(trimmed, ".file", 5)  == 0 ||
        strncasecmp(trimmed, ".ident", 6) == 0 ||
        strncasecmp(trimmed, ".cfi_", 5)  == 0 ||
        strncasecmp(trimmed, ".note", 5)  == 0 ||
        strncasecmp(trimmed, ".att_syntax", 11) == 0 ||
        strncasecmp(trimmed, ".intel_syntax", 13) == 0 ||
        strncasecmp(trimmed, ".p2align", 8) == 0) {
        line[0] = '\0'; return 1;
    }

    /* .p2align N → .align (1<<N)  (GAS power-of-2 alignment) */
    /* already ignored above; could implement if needed */

    /* Anything else starting with '.' → pass through unchanged */
    if (trimmed[0] == '.') {
        snprintf(line, MAX_LINE, "%s", trimmed);
        return 1;
    }

    /* ---- Label handling -------------------------------------------- */
    /* Labels end with ':'; may stand alone or precede an instruction */
    {
        char *colon = strchr(trimmed, ':');
        if (colon && (colon[1] == '\0' || colon[1] == ' ' || colon[1] == '\t')) {
            char *after = trim(colon + 1);
            if (after && *after) {
                /* "label: instruction" — keep both on one line.
                 * assemble_line's label handler will process the label
                 * then recursively assemble the remainder. */
                *colon = '\0';
                snprintf(line, MAX_LINE, "%s: %s", trimmed, after);
            } else {
                /* Label alone */
                *colon = '\0';
                snprintf(line, MAX_LINE, "%s:", trimmed);
            }
            return 1;
        }
    }

    /* ---- Instruction handling -------------------------------------- */
    /* Split into mnemonic and operands */
    char mnemonic[32] = {0};
    const char *ops_start = NULL;
    {
        const char *p = trimmed;
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && i < 31)
            mnemonic[i++] = *p++;
        mnemonic[i] = '\0';
        ops_start = (*p) ? trim((char*)p) : NULL;
    }

    /* Detect AT&T syntax: presence of % register prefix or $ immediate,
       or a size-suffixed mnemonic (movq, pushq, leaq …).
       If none of these markers are present the line is Intel syntax —
       pass it through unchanged so we don't corrupt it. */
    int is_att = 0;
    if (ops_start) {
        /* Scan for % or $ anywhere in the operands */
        for (const char *p = ops_start; *p; p++) {
            if (*p == '%' || *p == '$') { is_att = 1; break; }
        }
    }
    /* Also check for size-suffixed mnemonic: copy, strip, check if base known */
    if (!is_att) {
        char test_mn[32];
        strncpy(test_mn, mnemonic, sizeof(test_mn) - 1);
        test_mn[sizeof(test_mn) - 1] = '\0';
        size_t orig_len = strlen(test_mn);
        strip_size_suffix(test_mn);
        if (strlen(test_mn) < orig_len) is_att = 1; /* suffix was stripped */
    }

    if (!is_att) {
        /* Pure Intel syntax — put back the original trimmed line and return */
        snprintf(line, MAX_LINE, "%s", trimmed);
        return 1;
    }

    /* Strip size suffix (q/l/w/b) from AT&T mnemonic */
    strip_size_suffix(mnemonic);

    /* No operands */
    if (!ops_start || !*ops_start) {
        snprintf(line, MAX_LINE, "%s", mnemonic);
        return 1;
    }

    /* Split operands around first top-level comma */
    char first[64] = {0}, second[64] = {0};
    split_operands(ops_start, first, sizeof(first), second, sizeof(second));
    trim(first); trim(second);

    /* Normalise each operand */
    char op1[128] = {0}, op2[128] = {0};
    strncpy(op1, norm_operand(first),  sizeof(op1)  - 1);
    if (second[0])
        strncpy(op2, norm_operand(second), sizeof(op2) - 1);

    /* AT&T: two-operand instructions are src, dst → swap to dst, src */
    if (second[0] && att_is_two_op(mnemonic)) {
        /* op1=src, op2=dst → Intel: op2, op1 */
        snprintf(line, MAX_LINE, "%s %s, %s", mnemonic, op2, op1);
    } else if (second[0]) {
        snprintf(line, MAX_LINE, "%s %s, %s", mnemonic, op1, op2);
    } else {
        snprintf(line, MAX_LINE, "%s %s", mnemonic, op1);
    }
    return 1;
}

/* Strip Intel operand size hints (byte/word/dword/qword) from arg strings.
 * Handles two forms:
 *   embedded prefix: "byte [rax]"  -> "[rax]"   (in-place)
 *   separate token:  arg2="byte", arg3="[rax]"  -> arg2="[rax]", arg3=NULL */
static int is_size_hint_word(const char *s) {
    if (!s) return 0;
    return strcasecmp(s,"byte")==0 || strcasecmp(s,"word")==0 ||
           strcasecmp(s,"dword")==0 || strcasecmp(s,"qword")==0 ||
           strcasecmp(s,"tbyte")==0 || strcasecmp(s,"ptr")==0 ||
           strcasecmp(s,"xmmword")==0 || strcasecmp(s,"ymmword")==0;
}
static void strip_hint_prefix(char *s) {
    if (!s) return;
    char *sp = s;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (!*sp) return;
    char save = *sp; *sp = '\0';
    if (!is_size_hint_word(s)) { *sp = save; return; }
    char *rest = sp + 1;
    while (*rest == ' ' || *rest == '\t') rest++;
    memmove(s, rest, strlen(rest) + 1);
}
static void strip_size_hint_arg(char **a1, char **a2, char **a3) {
    strip_hint_prefix(*a1);
    strip_hint_prefix(*a2);
    strip_hint_prefix(*a3);
    if (is_size_hint_word(*a2) && *a3) { *a2 = *a3; *a3 = NULL; }
    if (is_size_hint_word(*a1) && *a2) { *a1 = *a2; *a2 = *a3; *a3 = NULL; }
}

void assemble_line(char *line) {
    /* Normalise AT&T/GAS syntax to Intel syntax before processing */
    normalize_att_line(line);
    if (!line[0]) return;

    line_number++;

    // Check if we're inside a false conditional block
    int skip = 0;
    for (int i = 0; i < if_depth; i++) {
        if (if_stack[i] == 0) {
            skip = 1;
            break;
        }
    }
    
    char *_al_save = NULL;
    char *op = strtok_r(line, " \t\n", &_al_save);
    if (!op || op[0] == ';' || op[0] == '#') return;
    
    // Always process conditional directives even when skipping
    if (strcasecmp(op, ".if") == 0 || strcasecmp(op, ".else") == 0 || 
        strcasecmp(op, ".endif") == 0 || strcasecmp(op, ".ifdef") == 0 || 
        strcasecmp(op, ".ifndef") == 0) {
        // Let these fall through to be processed
    } else if (skip) {
        return; // Skip this line
    }

    /* Split remaining text into up to 3 args, respecting brackets so that
       commas inside [...] don't split an operand like [rip+.LC0]. */
    char *rest = strtok_r(NULL, "", &_al_save);  /* everything after op */
    char *arg1 = NULL, *arg2 = NULL, *arg3 = NULL;
    static char _a1[MAX_LINE], _a2[MAX_LINE], _a3[MAX_LINE];
    _a1[0] = _a2[0] = _a3[0] = '\0';

    if (rest) {
        /* skip leading whitespace */
        while (*rest == ' ' || *rest == '\t') rest++;
        /* find first top-level comma */
        int depth = 0;
        char *p = rest, *comma1 = NULL, *comma2 = NULL;
        while (*p) {
            if (*p == '[' || *p == '(') depth++;
            else if (*p == ']' || *p == ')') depth--;
            else if (*p == ',' && depth == 0) {
                if (!comma1) comma1 = p;
                else if (!comma2) { comma2 = p; break; }
            }
            p++;
        }
        if (comma1) {
            size_t n1 = (size_t)(comma1 - rest);
            if (n1 >= MAX_LINE) n1 = MAX_LINE - 1;
            memcpy(_a1, rest, n1); _a1[n1] = '\0';
            /* trim trailing whitespace */
            char *e = _a1 + strlen(_a1);
            while (e > _a1 && (e[-1]==' '||e[-1]=='\t')) *--e='\0';
            arg1 = _a1;

            char *second = comma1 + 1;
            while (*second == ' ' || *second == '\t') second++;
            if (comma2) {
                size_t n2 = (size_t)(comma2 - second);
                if (n2 >= MAX_LINE) n2 = MAX_LINE - 1;
                memcpy(_a2, second, n2); _a2[n2] = '\0';
                e = _a2 + strlen(_a2);
                while (e > _a2 && (e[-1]==' '||e[-1]=='\t')) *--e='\0';
                arg2 = _a2;
                char *third = comma2 + 1;
                while (*third == ' ' || *third == '\t') third++;
                strncpy(_a3, third, MAX_LINE - 1); _a3[MAX_LINE-1] = '\0';
                e = _a3 + strlen(_a3);
                while (e > _a3 && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e='\0';
                if (_a3[0]) arg3 = _a3;
            } else {
                strncpy(_a2, second, MAX_LINE - 1); _a2[MAX_LINE-1] = '\0';
                e = _a2 + strlen(_a2);
                while (e > _a2 && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e='\0';
                if (_a2[0]) arg2 = _a2;
            }
        } else {
            /* no comma — try space-splitting (for directives like .equ, .fill, .space) */
            strncpy(_a1, rest, MAX_LINE - 1); _a1[MAX_LINE-1] = '\0';
            char *e = _a1 + strlen(_a1);
            while (e > _a1 && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e='\0';
            if (_a1[0]) {
                arg1 = _a1;
                /* find first space after arg1 */
                char *sp = _a1;
                while (*sp && *sp!=' ' && *sp!='\t') sp++;
                if (*sp) {
                    *sp++ = '\0';
                    while (*sp==' '||*sp=='\t') sp++;
                    if (*sp) {
                        strncpy(_a2, sp, MAX_LINE-1); _a2[MAX_LINE-1]='\0';
                        e = _a2+strlen(_a2);
                        while (e>_a2&&(e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e='\0';
                        if (_a2[0]) {
                            arg2 = _a2;
                            /* find second space for arg3 */
                            char *sp2 = _a2;
                            while (*sp2 && *sp2!=' ' && *sp2!='\t') sp2++;
                            if (*sp2) {
                                *sp2++ = '\0';
                                while (*sp2==' '||*sp2=='\t') sp2++;
                                if (*sp2) {
                                    strncpy(_a3, sp2, MAX_LINE-1); _a3[MAX_LINE-1]='\0';
                                    e = _a3+strlen(_a3);
                                    while (e>_a3&&(e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e='\0';
                                    if (_a3[0]) arg3 = _a3;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (strcasecmp(op, ".section") == 0 && arg1) {
        if (strcasecmp(arg1, ".text") == 0) current_section = 0;
        else if (strcasecmp(arg1, ".data") == 0) current_section = 1;
        return;
    }
    if (strcasecmp(op, ".text") == 0) { current_section = 0; return; }
    if (strcasecmp(op, ".data") == 0) { current_section = 1; return; }
    if (strcasecmp(op, ".bss") == 0) { current_section = 2; return; }
    if (strcasecmp(op, ".byte") == 0 && arg1) {
        int32_t v;
        if (parse_imm(arg1, &v)) emit_byte(v);
        return;
    }
    if (strcasecmp(op, ".word") == 0 && arg1) {
        int32_t v;
        if (parse_imm(arg1, &v)) emit_word(v);
        return;
    }
    if (strcasecmp(op, ".dword") == 0 && arg1) {
        int32_t v;
        if (parse_imm(arg1, &v)) emit_dword(v);
        return;
    }
    if (strcasecmp(op, ".ascii") == 0 && arg1) {
        char *s = arg1;
        if (*s == '"') s++;
        while (*s && *s != '"') {
            if (*s == '\\' && *(s+1)) {
                s++;
                switch (*s) {
                    case 'n': emit_byte('\n'); break;
                    case 't': emit_byte('\t'); break;
                    case 'r': emit_byte('\r'); break;
                    case '0': emit_byte('\0'); break;
                    case '\\': emit_byte('\\'); break;
                    case '"': emit_byte('"'); break;
                    default: emit_byte(*s); break;
                }
            } else {
                emit_byte(*s);
            }
            s++;
        }
        return;
    }
    if (strcasecmp(op, ".asciz") == 0 && arg1) {
        char *s = arg1;
        if (*s == '"') s++;
        while (*s && *s != '"') {
            if (*s == '\\' && *(s+1)) {
                s++;
                switch (*s) {
                    case 'n': emit_byte('\n'); break;
                    case 't': emit_byte('\t'); break;
                    case 'r': emit_byte('\r'); break;
                    case '0': emit_byte('\0'); break;
                    case '\\': emit_byte('\\'); break;
                    case '"': emit_byte('"'); break;
                    default: emit_byte(*s); break;
                }
            } else {
                emit_byte(*s);
            }
            s++;
        }
        emit_byte(0); // Null terminator
        return;
    }
    if (strcasecmp(op, ".fill") == 0 && arg1) {
        int32_t count, size, value;
        if (parse_imm(arg1, &count)) {
            size = arg2 ? (parse_imm(arg2, &size) ? size : 1) : 1;
            value = arg3 ? (parse_imm(arg3, &value) ? value : 0) : 0;
            for (int i = 0; i < count; i++) {
                for (int j = 0; j < size; j++) {
                    emit_byte(value & 0xff);
                }
            }
        }
        return;
    }
    if (strcasecmp(op, ".zero") == 0 && arg1) {
        int32_t count;
        if (parse_imm(arg1, &count)) {
            for (int i = 0; i < count; i++) emit_byte(0);
        }
        return;
    }
    if (strcasecmp(op, ".space") == 0 && arg1) {
        int32_t count, value = 0;
        if (parse_imm(arg1, &count)) {
            if (arg2) parse_imm(arg2, &value);
            for (int i = 0; i < count; i++) emit_byte(value);
        }
        return;
    }
    if (strcasecmp(op, ".quad") == 0 && arg1) {
        int32_t val;
        if (parse_imm(arg1, &val)) {
            emit_dword(val);
            emit_dword(0); // Upper 32 bits
        }
        return;
    }
    if (strcasecmp(op, ".global") == 0 || strcasecmp(op, ".globl") == 0) return;
    if (strcasecmp(op, ".extern") == 0) return;
    if (strcasecmp(op, ".rodata") == 0) { current_section = 1; return; }
    if (strcasecmp(op, ".string") == 0 && arg1) {
        /* Same as .asciz */
        char *s = arg1;
        if (*s == '"') s++;
        while (*s && *s != '"') {
            if (*s == '\\' && *(s+1)) {
                s++;
                switch (*s) {
                    case 'n': emit_byte('\n'); break;
                    case 't': emit_byte('\t'); break;
                    case 'r': emit_byte('\r'); break;
                    case '0': emit_byte('\0'); break;
                    case '\\': emit_byte('\\'); break;
                    case '"': emit_byte('"'); break;
                    default: emit_byte(*s); break;
                }
            } else { emit_byte(*s); }
            s++;
        }
        emit_byte(0);
        return;
    }
    if (strcasecmp(op, ".long") == 0 && arg1) {
        int32_t v; if (parse_imm(arg1, &v)) emit_dword(v); return;
    }
    if (strcasecmp(op, ".short") == 0 && arg1) {
        int32_t v; if (parse_imm(arg1, &v)) emit_word(v); return;
    }
    /* Ignore GAS metadata directives */
    if (strcasecmp(op, ".size") == 0 || strcasecmp(op, ".type") == 0 ||
        strcasecmp(op, ".file") == 0 || strcasecmp(op, ".ident") == 0 ||
        strncasecmp(op, ".cfi_", 5) == 0 || strncasecmp(op, ".note", 5) == 0 ||
        strcasecmp(op, ".p2align") == 0) return;
    if (strcasecmp(op, ".include") == 0 && arg1) {
        if (include_depth >= MAX_INCLUDE_DEPTH) return;
        include_depth++;
        char *filename = arg1;
        if (filename[0] == '"' || filename[0] == '<') {
            filename++;
            char *end = strchr(filename, filename[0] == '"' ? '"' : '>');
            if (end) *end = '\0';
        }
        FILE *inc = fopen(filename, "r");
        if (inc) {
            char inc_line[MAX_LINE];
            while (fgets(inc_line, MAX_LINE, inc)) {
                assemble_line(inc_line);
            }
            fclose(inc);
        }
        include_depth--;
        return;
    }
    if (strcasecmp(op, ".equ") == 0 && arg1 && arg2) {
        int idx = get_symbol(arg1);
        if (idx < 0) return;
        int32_t val;
        if (parse_imm(arg2, &val)) {
            symbols[idx].value = val;
            symbols[idx].defined = 1;
        }
        return;
    }
    if (strcasecmp(op, ".set") == 0 && arg1 && arg2) {
        int idx = get_symbol(arg1);
        if (idx < 0) return;
        int32_t val;
        if (parse_imm(arg2, &val)) {
            symbols[idx].value = val;
            symbols[idx].defined = 1;
        }
        return;
    }
    if (strcasecmp(op, ".align") == 0 && arg1) {
        int32_t align;
        if (parse_imm(arg1, &align)) {
            uint32_t mask = align - 1;
            uint32_t current = current_section == 0 ? code_size : data_size;
            uint32_t padding = (mask - (current & mask)) & mask;
            for (uint32_t i = 0; i < padding; i++) emit_byte(0);
        }
        return;
    }
    if (strcasecmp(op, ".org") == 0 && arg1) {
        int32_t org;
        if (parse_imm(arg1, &org)) {
            uint32_t current = current_section == 0 ? code_size : data_size;
            if (org > 0 && (uint32_t)org > current) {
                for (uint32_t i = 0; i < (uint32_t)org - current; i++) emit_byte(0);
            }
        }
        return;
    }
    if (strcasecmp(op, ".if") == 0 && arg1) {
        if (if_depth < MAX_INCLUDE_DEPTH) {
            int32_t val;
            if_stack[if_depth] = parse_imm(arg1, &val) ? val : 0;
            if_depth++;
        }
        return;
    }
    if (strcasecmp(op, ".else") == 0) {
        if (if_depth > 0) {
            if_stack[if_depth - 1] = !if_stack[if_depth - 1];
        }
        return;
    }
    if (strcasecmp(op, ".endif") == 0) {
        if (if_depth > 0) {
            if_depth--;
        }
        return;
    }
    if (strcasecmp(op, ".ifdef") == 0 && arg1) {
        if (if_depth < MAX_INCLUDE_DEPTH) {
            int idx = get_symbol(arg1);
            if_stack[if_depth] = (idx >= 0 && symbols[idx].defined) ? 1 : 0;
            if_depth++;
        }
        return;
    }
    if (strcasecmp(op, ".ifndef") == 0 && arg1) {
        if (if_depth < MAX_INCLUDE_DEPTH) {
            int idx = get_symbol(arg1);
            if_stack[if_depth] = (idx >= 0 && symbols[idx].defined) ? 0 : 1;
            if_depth++;
        }
        return;
    }
    
    if (op[strlen(op)-1] == ':') {
        op[strlen(op)-1] = 0;
        int idx = get_symbol(op);
        if (idx < 0) return;
        if (current_section == 0) {
            symbols[idx].value = code_size;
        } else {
            symbols[idx].value = data_size | 0x80000000u;
        }
        symbols[idx].defined = 1;
        /* If there is more on this line (e.g. "label: .byte 1"), process it */
        if (arg1) {
            /* Rebuild a line from arg1 onward and re-assemble it */
            char rest_line[MAX_LINE];
            if (arg2 && arg3)
                snprintf(rest_line, MAX_LINE, "%s %s %s", arg1, arg2, arg3);
            else if (arg2)
                snprintf(rest_line, MAX_LINE, "%s %s", arg1, arg2);
            else
                snprintf(rest_line, MAX_LINE, "%s", arg1);
            assemble_line(rest_line);
        }
        return;
    }
    
    int32_t imm;
    int r1, r2;

    /* Strip Intel size hints embedded in operand strings or as separate tokens */
    strip_size_hint_arg(&arg1, &arg2, &arg3);

    if (strcasecmp(op, "mov") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;

        if (r1 != -1 && parse_imm(arg2, &imm)) {
            /* MOV reg, imm */
            if (is_64bit_reg(arg1)) {
                emit_rex_w(needs_rex(arg1) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0xb8 + (r1 & 7));
                emit_qword(imm);
            } else {
                emit_byte(0xb8 + (r1 & 7));
                emit_dword(imm);
            }
        } else if (r1 != -1 && (r2 = parse_reg(arg2)) != -1) {
            /* MOV r/m, r  (0x89): reg=src(r2), r/m=dst(r1) */
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2))
                emit_rex(is_64bit_reg(arg1)||is_64bit_reg(arg2)?1:0,
                         needs_rex(arg2)?1:0, 0, needs_rex(arg1)?1:0);
            emit_byte(0x89);
            emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
        } else if (r1 != -1 && parse_mem_operand(arg2, &mem)) {
            /* MOV reg, [mem]  (0x8B) */
            if (is_64bit_reg(arg1) || needs_rex(arg1))
                emit_rex(is_64bit_reg(arg1)?1:0, needs_rex(arg1)?1:0, 0, 0);
            emit_byte(0x8b);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            /* MOV [mem], reg  (0x89) */
            if (is_64bit_reg(arg2) || needs_rex(arg2))
                emit_rex(is_64bit_reg(arg2)?1:0, needs_rex(arg2)?1:0, 0, 0);
            emit_byte(0x89);
            emit_mem_operand(&mem, r2);
        } else if (parse_mem_operand(arg1, &mem) && parse_imm(arg2, &imm)) {
            /* MOV [mem], imm  (C7 /0) — 32-bit immediate sign-extended */
            emit_byte(0xc7);
            emit_mem_operand(&mem, 0);
            emit_dword(imm);
        }
    }
    else if (strcasecmp(op, "add") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                if (imm >= -128 && imm < 128) {
                    emit_byte(0x83);
                    emit_byte(0xc0 + (r1 & 7));
                    emit_byte(imm);
                } else {
                    emit_byte(0x81);
                    emit_byte(0xc0 + (r1 & 7));
                    emit_dword(imm);
                }
            } else {
                if (imm >= -128 && imm < 128) {
                    emit_byte(0x83);
                    emit_byte(0xc0 + r1);
                    emit_byte(imm);
                } else {
                    emit_byte(0x81);
                    emit_byte(0xc0 + r1);
                    emit_dword(imm);
                }
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x01);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x01);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x03);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x01);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "sub") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                if (imm >= -128 && imm < 128) {
                    emit_byte(0x83);
                    emit_byte(0xe8 + (r1 & 7));
                    emit_byte(imm);
                } else {
                    emit_byte(0x81);
                    emit_byte(0xe8 + (r1 & 7));
                    emit_dword(imm);
                }
            } else {
                if (imm >= -128 && imm < 128) {
                    emit_byte(0x83);
                    emit_byte(0xe8 + r1);
                    emit_byte(imm);
                } else {
                    emit_byte(0x81);
                    emit_byte(0xe8 + r1);
                    emit_dword(imm);
                }
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x29);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x29);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x2b);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x29);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "and") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x81);
                emit_byte(0xe0 + (r1 & 7));
                emit_dword(imm);
            } else {
                emit_byte(0x81);
                emit_byte(0xe0 + r1);
                emit_dword(imm);
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x21);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x21);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x23);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x21);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "or") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x81);
                emit_byte(0xc8 + (r1 & 7));
                emit_dword(imm);
            } else {
                emit_byte(0x81);
                emit_byte(0xc8 + r1);
                emit_dword(imm);
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x09);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x09);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x0b);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x09);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "xor") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x81);
                emit_byte(0xf0 + (r1 & 7));
                emit_dword(imm);
            } else {
                emit_byte(0x81);
                emit_byte(0xf0 + r1);
                emit_dword(imm);
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x31);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x31);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x33);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x31);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "not") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            emit_byte(0xf7);
            emit_byte(0xd0 + (r1 & 7));
        } else {
            emit_byte(0xf7);
            emit_byte(0xd0 + r1);
        }
    }
    else if (strcasecmp(op, "neg") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            emit_byte(0xf7);
            emit_byte(0xd8 + (r1 & 7));
        } else {
            emit_byte(0xf7);
            emit_byte(0xd8 + r1);
        }
    }
    else if (strcasecmp(op, "shl") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0xc1);
                emit_byte(0xe0 + (r1 & 7));
            } else {
                emit_byte(0xc1);
                emit_byte(0xe0 + r1);
            }
            emit_byte(imm);
        } else if (strcmp(arg2, "cl") == 0) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0xd3);
            emit_byte(0xe0 + (r1 & 7));
        }
    }
    else if (strcasecmp(op, "shr") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0xc1);
                emit_byte(0xe8 + (r1 & 7));
            } else {
                emit_byte(0xc1);
                emit_byte(0xe8 + r1);
            }
            emit_byte(imm);
        } else if (strcmp(arg2, "cl") == 0) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0xd3);
            emit_byte(0xe8 + (r1 & 7));
        }
    }
    else if (strcasecmp(op, "sar") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0xc1);
                emit_byte(0xf8 + (r1 & 7));
            } else {
                emit_byte(0xc1);
                emit_byte(0xf8 + r1);
            }
            emit_byte(imm);
        } else if (strcmp(arg2, "cl") == 0) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0xd3);
            emit_byte(0xf8 + (r1 & 7));
        }
    }
    else if (strcasecmp(op, "rol") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0xc1);
            emit_byte(0xc0 + (r1 & 7));
            emit_byte(imm);
        }
    }
    else if (strcasecmp(op, "ror") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0xc1);
            emit_byte(0xc8 + (r1 & 7));
            emit_byte(imm);
        }
    }
    else if (strcasecmp(op, "lea") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x8d);
            emit_mem_operand(&mem, r1);
        }
    }
    else if (strcasecmp(op, "mul") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            emit_byte(0xf7);
            emit_byte(0xe0 + (r1 & 7));
        } else {
            emit_byte(0xf7);
            emit_byte(0xe0 + r1);
        }
    }
    else if (strcasecmp(op, "imul") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (arg2 && arg3 && (r2 = parse_reg(arg2)) != -1
                   && parse_imm(arg3, &imm)) {
            /* Three-operand: imul dst, src, imm  (6B /r imm8 or 69 /r imm32) */
            if (is_64bit_reg(arg1)||is_64bit_reg(arg2)||needs_rex(arg1)||needs_rex(arg2))
                emit_rex(is_64bit_reg(arg1)||is_64bit_reg(arg2)?1:0,
                         needs_rex(arg1)?1:0, 0, needs_rex(arg2)?1:0);
            if (imm >= -128 && imm <= 127) {
                emit_byte(0x6b);
                emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
                emit_byte((int8_t)imm);
            } else {
                emit_byte(0x69);
                emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
                emit_dword(imm);
            }
        } else if (arg2 && (r2 = parse_reg(arg2)) != -1) {
            /* Two-operand: imul dst, src  ->  0F AF /r
               REX.R extends dst (r1), REX.B extends src (r2) */
            if (is_64bit_reg(arg1)||is_64bit_reg(arg2)||needs_rex(arg1)||needs_rex(arg2))
                emit_rex(is_64bit_reg(arg1)||is_64bit_reg(arg2)?1:0,
                         needs_rex(arg1)?1:0, 0, needs_rex(arg2)?1:0);
            emit_byte(0x0f); emit_byte(0xaf);
            emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
        } else {
            /* One-operand: imul src  ->  F7 /5  (rdx:rax = rax * src) */
            if (is_64bit_reg(arg1) || needs_rex(arg1))
                emit_rex(is_64bit_reg(arg1)?1:0, 0, 0, needs_rex(arg1)?1:0);
            emit_byte(0xf7); emit_byte(0xe8 + (r1 & 7));
        }
    }
    else if (strcasecmp(op, "div") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            emit_byte(0xf7);
            emit_byte(0xf0 + (r1 & 7));
        } else {
            emit_byte(0xf7);
            emit_byte(0xf0 + r1);
        }
    }
    else if (strcasecmp(op, "idiv") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            emit_byte(0xf7);
            emit_byte(0xf8 + (r1 & 7));
        } else {
            emit_byte(0xf7);
            emit_byte(0xf8 + r1);
        }
    }
    else if (strcasecmp(op, "cmp") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_imm(arg2, &imm)) {
            if (is_64bit_reg(arg1)) {
                emit_rex_w(0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x81);
                emit_byte(0xf8 + (r1 & 7));
                emit_dword(imm);
            } else {
                emit_byte(0x81);
                emit_byte(0xf8 + r1);
                emit_dword(imm);
            }
        } else if ((r2 = parse_reg(arg2)) != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x39);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x39);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x3b);
            emit_mem_operand(&mem, r1);
        } else if (parse_mem_operand(arg1, &mem) && (r2 = parse_reg(arg2)) != -1) {
            if (needs_rex(arg2)) {
                emit_rex(0, 0, 0, needs_rex(arg2) ? 1 : 0);
            }
            emit_byte(0x39);
            emit_mem_operand(&mem, r2);
        }
    }
    else if (strcasecmp(op, "test") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        r2 = parse_reg(arg2);
        if (r2 != -1) {
            if (is_64bit_reg(arg1) || is_64bit_reg(arg2) || needs_rex(arg1) || needs_rex(arg2)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, needs_rex(arg2) ? 1 : 0, 0, needs_rex(arg1) ? 1 : 0);
                emit_byte(0x85);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            } else {
                emit_byte(0x85);
                emit_byte(0xc0 + ((r2 & 7) << 3) + (r1 & 7));
            }
        } else if (parse_mem_operand(arg2, &mem)) {
            if (is_64bit_reg(arg1) || needs_rex(arg1)) {
                emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
            }
            emit_byte(0x85);
            emit_mem_operand(&mem, r1);
        }
    }
    else if (strcasecmp(op, "jmp") == 0 && arg1) {
        emit_byte(0xe9);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "je") == 0 || strcasecmp(op, "jz") == 0) {
        emit_byte(0x0f); emit_byte(0x84);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jne") == 0 || strcasecmp(op, "jnz") == 0) {
        emit_byte(0x0f); emit_byte(0x85);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jg") == 0 || strcasecmp(op, "jnle") == 0) {
        emit_byte(0x0f); emit_byte(0x8f);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jl") == 0 || strcasecmp(op, "jnge") == 0) {
        emit_byte(0x0f); emit_byte(0x8c);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jge") == 0 || strcasecmp(op, "jnl") == 0) {
        emit_byte(0x0f); emit_byte(0x8d);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jle") == 0 || strcasecmp(op, "jng") == 0) {
        emit_byte(0x0f); emit_byte(0x8e);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "ja") == 0 || strcasecmp(op, "jnbe") == 0) {
        emit_byte(0x0f); emit_byte(0x87);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jb") == 0 || strcasecmp(op, "jnae") == 0) {
        emit_byte(0x0f); emit_byte(0x82);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jae") == 0 || strcasecmp(op, "jnb") == 0) {
        emit_byte(0x0f); emit_byte(0x83);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jbe") == 0 || strcasecmp(op, "jna") == 0) {
        emit_byte(0x0f); emit_byte(0x86);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "js") == 0) {
        emit_byte(0x0f); emit_byte(0x88);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "jns") == 0) {
        emit_byte(0x0f); emit_byte(0x89);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "loop") == 0 && arg1) {
        emit_byte(0xe2);
        add_reloc(code_size, arg1, 2);
        emit_byte(0);
    }
    else if (strcasecmp(op, "loope") == 0 || strcasecmp(op, "loopz") == 0) {
        emit_byte(0xe1);
        add_reloc(code_size, arg1, 2);
        emit_byte(0);
    }
    else if (strcasecmp(op, "loopne") == 0 || strcasecmp(op, "loopnz") == 0) {
        emit_byte(0xe0);
        add_reloc(code_size, arg1, 2);
        emit_byte(0);
    }
    else if (strcasecmp(op, "call") == 0 && arg1) {
        emit_byte(0xe8);
        add_reloc(code_size, arg1, 1);
        emit_dword(0);
    }
    else if (strcasecmp(op, "ret") == 0) {
        emit_byte(0xc3);
    }
    else if (strcasecmp(op, "ret") == 0 && arg1) {
        emit_byte(0xc2);
        if (parse_imm(arg1, &imm)) emit_word(imm);
    }
    else if (strcasecmp(op, "push") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (r1 != -1) {
            /* PUSH is 64-bit by default in 64-bit mode; REX only needed for r8-r15 */
            if (needs_rex(arg1))
                emit_rex(0, 0, 0, 1); /* REX.B only */
            emit_byte(0x50 + (r1 & 7));
        } else if (parse_imm(arg1, &imm)) {
            emit_byte(0x68);
            emit_dword(imm);
        }
    }
    else if (strcasecmp(op, "pop") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (r1 != -1) {
            /* POP is 64-bit by default in 64-bit mode; REX only needed for r8-r15 */
            if (needs_rex(arg1))
                emit_rex(0, 0, 0, 1); /* REX.B only */
            emit_byte(0x58 + (r1 & 7));
        }
    }
    else if (strcasecmp(op, "pusha") == 0) {
        emit_byte(0x60);
    }
    else if (strcasecmp(op, "popa") == 0) {
        emit_byte(0x61);
    }
    else if (strcasecmp(op, "pushf") == 0) {
        emit_byte(0x9c);
    }
    else if (strcasecmp(op, "popf") == 0) {
        emit_byte(0x9d);
    }
    else if (strcasecmp(op, "enter") == 0 && arg1 && arg2) {
        emit_byte(0xc8);
        if (parse_imm(arg1, &imm)) emit_word(imm);
        if (parse_imm(arg2, &imm)) emit_byte(imm);
    }
    else if (strcasecmp(op, "leave") == 0) {
        emit_byte(0xc9);
    }
    else if (strcasecmp(op, "int") == 0 && arg1) {
        if (parse_imm(arg1, &imm)) {
            emit_byte(0xcd);
            emit_byte(imm);
        }
    }
    else if (strcasecmp(op, "int3") == 0) {
        emit_byte(0xcc);
    }
    else if (strcasecmp(op, "into") == 0) {
        emit_byte(0xce);
    }
    else if (strcasecmp(op, "iret") == 0) {
        emit_byte(0xcf);
    }
    else if (strcasecmp(op, "clc") == 0) {
        emit_byte(0xf8);
    }
    else if (strcasecmp(op, "stc") == 0) {
        emit_byte(0xf9);
    }
    else if (strcasecmp(op, "cli") == 0) {
        emit_byte(0xfa);
    }
    else if (strcasecmp(op, "sti") == 0) {
        emit_byte(0xfb);
    }
    else if (strcasecmp(op, "cld") == 0) {
        emit_byte(0xfc);
    }
    else if (strcasecmp(op, "std") == 0) {
        emit_byte(0xfd);
    }
    else if (strcasecmp(op, "inc") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
        }
        emit_byte(0xff);
        emit_byte(0xc0 + (r1 & 7));
    }
    else if (strcasecmp(op, "dec") == 0 && arg1) {
        r1 = parse_reg(arg1);
        if (is_64bit_reg(arg1) || needs_rex(arg1)) {
            emit_rex(is_64bit_reg(arg1) ? 1 : 0, 0, 0, needs_rex(arg1) ? 1 : 0);
        }
        emit_byte(0xff);
        emit_byte(0xc8 + (r1 & 7));
    }
    else if (strcasecmp(op, "nop") == 0) {
        emit_byte(0x90);
    }
    else if (strcasecmp(op, "hlt") == 0) {
        emit_byte(0xf4);
    }
    else if (strcasecmp(op, "lea") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        MemOperand mem;
        if (parse_mem_operand(arg2, &mem)) {
            emit_byte(0x8d);
            emit_mem_operand(&mem, r1);
        } else if ((r2 = parse_reg(arg2)) != -1) {
            emit_byte(0x8d);
            emit_byte(0x00 + ((r1 & 7) << 3) + (r2 & 7));
        }
    }
    else if (strcasecmp(op, "xchg") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (strcmp(arg2, "eax") == 0) {
            emit_byte(0x90 + r1);
        }
    }
    else if (strcasecmp(op, "cbw") == 0) {
        emit_byte(0x98);
    }
    else if (strcasecmp(op, "cwd") == 0) {
        emit_byte(0x66); emit_byte(0x99);
    }
    else if (strcasecmp(op, "cdq") == 0) {
        emit_byte(0x66); emit_byte(0x99);
    }
    else if (strcasecmp(op, "cqo") == 0) {
        emit_byte(0x48); emit_byte(0x99);
    }
    else if (strcasecmp(op, "cdqe") == 0) {
        emit_byte(0x48); emit_byte(0x98);
    }
    else if (strcasecmp(op, "movsxd") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x63); emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "movsb") == 0) {
        emit_byte(0xa4);
    }
    else if (strcasecmp(op, "movsw") == 0) {
        emit_byte(0x66); emit_byte(0xa5);
    }
    else if (strcasecmp(op, "movsd") == 0) {
        emit_byte(0xa5);
    }
    else if (strcasecmp(op, "cmpsb") == 0) {
        emit_byte(0xa6);
    }
    else if (strcasecmp(op, "cmpsw") == 0) {
        emit_byte(0x66); emit_byte(0xa7);
    }
    else if (strcasecmp(op, "cmpsd") == 0) {
        emit_byte(0xa7);
    }
    else if (strcasecmp(op, "scasb") == 0) {
        emit_byte(0xae);
    }
    else if (strcasecmp(op, "scasw") == 0) {
        emit_byte(0x66); emit_byte(0xaf);
    }
    else if (strcasecmp(op, "scasd") == 0) {
        emit_byte(0xaf);
    }
    else if (strcasecmp(op, "lodsb") == 0) {
        emit_byte(0xac);
    }
    else if (strcasecmp(op, "lodsw") == 0) {
        emit_byte(0x66); emit_byte(0xad);
    }
    else if (strcasecmp(op, "lodsd") == 0) {
        emit_byte(0xad);
    }
    else if (strcasecmp(op, "stosb") == 0) {
        emit_byte(0xaa);
    }
    else if (strcasecmp(op, "stosw") == 0) {
        emit_byte(0x66); emit_byte(0xab);
    }
    else if (strcasecmp(op, "stosd") == 0) {
        emit_byte(0xab);
    }
    else if (strcasecmp(op, "rep") == 0 && arg1) {
        emit_byte(0xf3);
    }
    else if (strcasecmp(op, "repe") == 0 || strcasecmp(op, "repz") == 0) {
        emit_byte(0xf3);
    }
    else if (strcasecmp(op, "repne") == 0 || strcasecmp(op, "repnz") == 0) {
        emit_byte(0xf2);
    }
    else if (strcasecmp(op, "bswap") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f);
        emit_byte(0xc8 + r1);
    }
    else if (strcasecmp(op, "bsr") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xbd);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "bsf") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xbc);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "bt") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xa3);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "bts") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xab);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "btr") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xb3);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "btc") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f);
        emit_byte(0xbb);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "cmovz") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0x44);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "cmovnz") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0x45);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "cmovs") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0x48);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "cmovns") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0x49);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "setz") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x94);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setnz") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x95);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "sets") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x98);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setns") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x99);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setc") == 0 || strcasecmp(op, "setb") == 0) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x92);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setnc") == 0 || strcasecmp(op, "setae") == 0) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x93);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setg") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x9f);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setl") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x9c);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setge") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x9d);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "setle") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x9e);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "rcl") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            emit_byte(0xc1);
            emit_byte(0xd0 + r1);
            emit_byte(imm);
        }
    }
    else if (strcasecmp(op, "rcr") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        if (parse_imm(arg2, &imm)) {
            emit_byte(0xc1);
            emit_byte(0xd8 + r1);
            emit_byte(imm);
        }
    }
    else if (strcasecmp(op, "lahf") == 0) {
        emit_byte(0x9f);
    }
    else if (strcasecmp(op, "sahf") == 0) {
        emit_byte(0x9e);
    }
    else if (strcasecmp(op, "pushad") == 0) {
        emit_byte(0x60);
    }
    else if (strcasecmp(op, "popad") == 0) {
        emit_byte(0x61);
    }
    else if (strcasecmp(op, "bound") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        emit_byte(0x62);
        emit_byte(0x00 + ((r1 & 7) << 3));
    }
    else if (strcasecmp(op, "arpl") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x63);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "verr") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x00);
        emit_byte(0x20 + r1);
    }
    else if (strcasecmp(op, "verw") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0x00);
        emit_byte(0x28 + r1);
    }
    else if (strcasecmp(op, "syscall") == 0) {
        emit_byte(0x0f); emit_byte(0x05);
    }
    else if (strcasecmp(op, "sysenter") == 0) {
        emit_byte(0x0f); emit_byte(0x34);
    }
    else if (strcasecmp(op, "sysexit") == 0) {
        emit_byte(0x0f); emit_byte(0x35);
    }
    else if (strcasecmp(op, "sysret") == 0) {
        emit_byte(0x0f); emit_byte(0x07);
    }
    else if (strcasecmp(op, "rdtsc") == 0) {
        emit_byte(0x0f); emit_byte(0x31);
    }
    else if (strcasecmp(op, "rdmsr") == 0) {
        emit_byte(0x0f); emit_byte(0x32);
    }
    else if (strcasecmp(op, "wrmsr") == 0) {
        emit_byte(0x0f); emit_byte(0x30);
    }
    else if (strcasecmp(op, "cpuid") == 0) {
        emit_byte(0x0f); emit_byte(0xa2);
    }
    else if (strcasecmp(op, "pushfd") == 0) {
        emit_byte(0x9c);
    }
    else if (strcasecmp(op, "popfd") == 0) {
        emit_byte(0x9d);
    }
    else if (strcasecmp(op, "lidt") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x01); emit_byte(0x18);
    }
    else if (strcasecmp(op, "sidt") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x01); emit_byte(0x08);
    }
    else if (strcasecmp(op, "lgdt") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x01); emit_byte(0x10);
    }
    else if (strcasecmp(op, "sgdt") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x01); emit_byte(0x00);
    }
    else if (strcasecmp(op, "xadd") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0xc0);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "cmpxchg") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0xb0);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "lock") == 0) {
        emit_byte(0xf0);
    }
    else if (strcasecmp(op, "xlat") == 0) {
        emit_byte(0xd7);
    }
    else if (strcasecmp(op, "crc32") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0xf2); emit_byte(0x0f); emit_byte(0x38);
        emit_byte(0xf0 + r1);
    }
    else if (strcasecmp(op, "fadd") == 0) {
        emit_byte(0xd8); emit_byte(0xc0);
    }
    else if (strcasecmp(op, "fsub") == 0) {
        emit_byte(0xd8); emit_byte(0xe0);
    }
    else if (strcasecmp(op, "fmul") == 0) {
        emit_byte(0xd8); emit_byte(0xc8);
    }
    else if (strcasecmp(op, "fdiv") == 0) {
        emit_byte(0xd8); emit_byte(0xf0);
    }
    else if (strcasecmp(op, "faddp") == 0) {
        emit_byte(0xde); emit_byte(0xc1);
    }
    else if (strcasecmp(op, "fsubp") == 0) {
        emit_byte(0xde); emit_byte(0xe9);
    }
    else if (strcasecmp(op, "fmulp") == 0) {
        emit_byte(0xde); emit_byte(0xc9);
    }
    else if (strcasecmp(op, "fdivp") == 0) {
        emit_byte(0xde); emit_byte(0xf9);
    }
    else if (strcasecmp(op, "fld") == 0 && arg1) {
        if (strcmp(arg1, "dword") == 0) {
            emit_byte(0xd9); emit_byte(0x00);
        } else if (strcmp(arg1, "qword") == 0) {
            emit_byte(0xdd); emit_byte(0x00);
        } else if (strcmp(arg1, "tword") == 0) {
            emit_byte(0xdb); emit_byte(0x28);
        } else {
            emit_byte(0xd9); emit_byte(0xc0);
        }
    }
    else if (strcasecmp(op, "fst") == 0 && arg1) {
        if (strcmp(arg1, "dword") == 0) {
            emit_byte(0xd9); emit_byte(0x10);
        } else if (strcmp(arg1, "qword") == 0) {
            emit_byte(0xdd); emit_byte(0x10);
        } else if (strcmp(arg1, "tword") == 0) {
            emit_byte(0xdb); emit_byte(0x30);
        } else {
            emit_byte(0xdd); emit_byte(0xd0);
        }
    }
    else if (strcasecmp(op, "fstp") == 0 && arg1) {
        if (strcmp(arg1, "dword") == 0) {
            emit_byte(0xd9); emit_byte(0x18);
        } else if (strcmp(arg1, "qword") == 0) {
            emit_byte(0xdd); emit_byte(0x18);
        } else if (strcmp(arg1, "tword") == 0) {
            emit_byte(0xdb); emit_byte(0x38);
        } else {
            emit_byte(0xdd); emit_byte(0xd8);
        }
    }
    else if (strcasecmp(op, "fild") == 0 && arg1) {
        if (strcmp(arg1, "dword") == 0) {
            emit_byte(0xdb); emit_byte(0x00);
        } else if (strcmp(arg1, "qword") == 0) {
            emit_byte(0xdf); emit_byte(0x28);
        } else if (strcmp(arg1, "word") == 0) {
            emit_byte(0xdf); emit_byte(0x00);
        }
    }
    else if (strcasecmp(op, "fistp") == 0 && arg1) {
        if (strcmp(arg1, "dword") == 0) {
            emit_byte(0xdb); emit_byte(0x18);
        } else if (strcmp(arg1, "qword") == 0) {
            emit_byte(0xdf); emit_byte(0x3c);
        } else if (strcmp(arg1, "word") == 0) {
            emit_byte(0xdf); emit_byte(0x08);
        }
    }
    else if (strcasecmp(op, "fcom") == 0) {
        emit_byte(0xd8); emit_byte(0xd0);
    }
    else if (strcasecmp(op, "fcomp") == 0) {
        emit_byte(0xd8); emit_byte(0xd8);
    }
    else if (strcasecmp(op, "fcompp") == 0) {
        emit_byte(0xde); emit_byte(0xd9);
    }
    else if (strcasecmp(op, "ftst") == 0) {
        emit_byte(0xd9); emit_byte(0xe4);
    }
    else if (strcasecmp(op, "fxch") == 0) {
        emit_byte(0xd9); emit_byte(0xc8);
    }
    else if (strcasecmp(op, "fchs") == 0) {
        emit_byte(0xd9); emit_byte(0xe0);
    }
    else if (strcasecmp(op, "fabs") == 0) {
        emit_byte(0xd9); emit_byte(0xe1);
    }
    else if (strcasecmp(op, "fsqrt") == 0) {
        emit_byte(0xd9); emit_byte(0xfa);
    }
    else if (strcasecmp(op, "fsin") == 0) {
        emit_byte(0xd9); emit_byte(0xfe);
    }
    else if (strcasecmp(op, "fcos") == 0) {
        emit_byte(0xd9); emit_byte(0xff);
    }
    else if (strcasecmp(op, "fptan") == 0) {
        emit_byte(0xd9); emit_byte(0xf2);
    }
    else if (strcasecmp(op, "fpatan") == 0) {
        emit_byte(0xd9); emit_byte(0xf3);
    }
    else if (strcasecmp(op, "finit") == 0) {
        emit_byte(0x9b); emit_byte(0xdb); emit_byte(0xe3);
    }
    else if (strcasecmp(op, "fninit") == 0) {
        emit_byte(0xdb); emit_byte(0xe3);
    }
    else if (strcasecmp(op, "fldcw") == 0 && arg1) {
        emit_byte(0xd9); emit_byte(0x28);
    }
    else if (strcasecmp(op, "fstcw") == 0 && arg1) {
        emit_byte(0x9b); emit_byte(0xd9); emit_byte(0x38);
    }
    else if (strcasecmp(op, "fnstcw") == 0 && arg1) {
        emit_byte(0xd9); emit_byte(0x38);
    }
    else if (strcasecmp(op, "fldenv") == 0 && arg1) {
        emit_byte(0xd9); emit_byte(0x20);
    }
    else if (strcasecmp(op, "fnstenv") == 0 && arg1) {
        emit_byte(0xd9); emit_byte(0x30);
    }
    else if (strcasecmp(op, "fwait") == 0) {
        emit_byte(0x9b);
    }
    else if (strcasecmp(op, "fnop") == 0) {
        emit_byte(0xd9); emit_byte(0xd0);
    }
    else if (strcasecmp(op, "movsx") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        MemOperand mem2;
        if (r2 != -1) {
            /* reg <- reg */
            if (is_64bit_reg(arg1) || needs_rex(arg1) || needs_rex(arg2))
                emit_rex(is_64bit_reg(arg1)?1:0, needs_rex(arg1)?1:0, 0, needs_rex(arg2)?1:0);
            emit_byte(0x0f); emit_byte(0xbf);
            emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
        } else if (parse_mem_operand(arg2, &mem2)) {
            /* reg <- mem */
            if (is_64bit_reg(arg1) || needs_rex(arg1))
                emit_rex(is_64bit_reg(arg1)?1:0, needs_rex(arg1)?1:0, 0, 0);
            emit_byte(0x0f); emit_byte(0xbf);
            emit_mem_operand(&mem2, r1);
        }
    }
    else if (strcasecmp(op, "movzx") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        MemOperand mem2;
        if (r2 != -1) {
            /* reg <- reg */
            if (is_64bit_reg(arg1) || needs_rex(arg1) || needs_rex(arg2))
                emit_rex(is_64bit_reg(arg1)?1:0, needs_rex(arg1)?1:0, 0, needs_rex(arg2)?1:0);
            emit_byte(0x0f); emit_byte(0xb6);
            emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
        } else if (parse_mem_operand(arg2, &mem2)) {
            /* reg <- mem (zero-extend byte) */
            if (is_64bit_reg(arg1) || needs_rex(arg1))
                emit_rex(is_64bit_reg(arg1)?1:0, needs_rex(arg1)?1:0, 0, 0);
            emit_byte(0x0f); emit_byte(0xb6);
            emit_mem_operand(&mem2, r1);
        }
    }
    else if (strcasecmp(op, "cmpxchg8b") == 0 && arg1) {
        r1 = parse_reg(arg1);
        emit_byte(0x0f); emit_byte(0xc7);
        emit_byte(0xc0 + r1);
    }
    else if (strcasecmp(op, "prefetchnta") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x18); emit_byte(0x08);
    }
    else if (strcasecmp(op, "prefetcht0") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x18); emit_byte(0x01);
    }
    else if (strcasecmp(op, "prefetcht1") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x18); emit_byte(0x02);
    }
    else if (strcasecmp(op, "prefetcht2") == 0 && arg1) {
        emit_byte(0x0f); emit_byte(0x18); emit_byte(0x03);
    }
    else if (strcasecmp(op, "sfence") == 0) {
        emit_byte(0x0f); emit_byte(0xae); emit_byte(0xf8);
    }
    else if (strcasecmp(op, "mfence") == 0) {
        emit_byte(0x0f); emit_byte(0xae); emit_byte(0xf0);
    }
    else if (strcasecmp(op, "lfence") == 0) {
        emit_byte(0x0f); emit_byte(0xae); emit_byte(0xe8);
    }
    else if (strcasecmp(op, "maskmovq") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0xf7);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "pshufw") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0x70);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "pminub") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0xda);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
    else if (strcasecmp(op, "pmaxub") == 0 && arg1 && arg2) {
        r1 = parse_reg(arg1);
        r2 = parse_reg(arg2);
        emit_byte(0x0f); emit_byte(0xde);
        emit_byte(0xc0 + ((r1 & 7) << 3) + (r2 & 7));
    }
}

/* Virtual address layout.
 * We place text at file offset PAGE_SIZE so text and data are on separate
 * physical pages and the kernel does not strip the execute bit when mapping
 * the data segment (which would happen if both shared the same 4 KB page). */
#define ELF_LOAD_BASE  0x400000u
#define ELF_PAGE_SIZE  0x1000u          /* 4 KB */
#define ELF_TEXT_FOFF  ELF_PAGE_SIZE    /* text starts at file offset 0x1000 */

static uint32_t elf_text_va(void) {
    return ELF_LOAD_BASE + ELF_TEXT_FOFF;   /* 0x401000 */
}
static uint32_t elf_data_va(void) {
    /* data goes on the next page after text */
    uint32_t text_pages = ((code_size + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1));
    return elf_text_va() + text_pages;
}

/* Resolve stored symbol value to absolute VA.
   Data symbols are tagged with bit 31 set. */
static uint32_t symbol_va(uint32_t v) {
    if (v & 0x80000000u)
        return elf_data_va() + (v & ~0x80000000u);
    return elf_text_va() + v;
}

void resolve_relocs(void) {
    for (int i = 0; i < reloc_count; i++) {
        int sym_idx = get_symbol(relocs[i].symbol);
        int section = relocs[i].type >> 8;
        int type    = relocs[i].type & 0xff;
        uint32_t off = relocs[i].offset;

        if (sym_idx < 0 || !symbols[sym_idx].defined) {
            /* Unresolved external symbol in text section:
             * Replace the call/jmp opcode + 4-byte placeholder with
             * a safe "exit(127)" sequence so execution doesn't jump
             * into garbage.  The 5 bytes are: nop(1) + int3(1) + nop(3)
             * — but we need something that terminates cleanly.
             * Instead: patch the preceding opcode byte (off-1) to 0x90
             * (nop) and the 4-byte field to a syscall exit sequence.
             * Since we only have 5 bytes (opcode + disp32), write:
             *   b8 3c 00 00 00  = mov eax, 60  (SYS_exit)  -- 5 bytes
             * then the next byte after will be hit, but we print a warning.
             */
            if (section == 0 && off >= 1 && off + 4 <= code_size) {
                fprintf(stderr, "warning: unresolved symbol '%s' — "
                        "replacing call/jmp with exit(127)\n",
                        relocs[i].symbol);
                /* Overwrite the opcode byte before the disp32 */
                code[off - 1] = 0xb8;             /* mov eax, imm32 */
                uint32_t exit127 = 127;
                memcpy(code + off, &exit127, 4);   /* imm32 = 127    */
                /* The 'ret' after this will still execute, which is fine */
            }
            continue;
        }

        uint32_t sym_va = symbol_va(symbols[sym_idx].value);

        if (section == 0) {
            if (off + 4 > code_size) continue;
            if (type == 2) {
                /* 8-bit relative */
                uint32_t next_va = elf_text_va() + off + 1;
                int32_t rel = (int32_t)(sym_va - next_va);
                code[off] = (uint8_t)(rel & 0xff);
            } else {
                /* 32-bit relative: displacement from end of the 4-byte field */
                uint32_t next_va = elf_text_va() + off + 4;
                int32_t rel = (int32_t)(sym_va - next_va);
                memcpy(code + off, &rel, 4);
            }
        } else if (section == 1) {
            if (off + 4 > data_size) continue;
            memcpy(data + off, &sym_va, 4);
        }
    }
}

void write_elf(const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    /* If the user defined 'main' but not '_start', prepend a minimal _start
     * stub that calls main() then exits via syscall so that 'ret' from main
     * doesn't fault trying to return to a garbage address.
     *
     * The stub (10 bytes):
     *   e8 XX XX XX XX   call main  (rel32 patched below)
     *   48 31 ff         xor  rdi, rdi   ; exit code 0
     *   b8 3c 00 00 00   mov  eax, 60    ; SYS_exit
     *   0f 05            syscall
     *
     * Total: 12 bytes.  We prepend this to the code section and shift all
     * text-symbol values by STUB_SIZE.
     */
    int has_main  = 0, has_start = 0;
    {
        int mi = get_symbol("main");
        int si = get_symbol("_start");
        if (mi >= 0 && symbols[mi].defined) has_main  = 1;
        if (si >= 0 && symbols[si].defined) has_start = 1;
    }

    static const uint8_t STUB[] = {
        0xe8, 0x00, 0x00, 0x00, 0x00, /* call main  (rel32 placeholder) */
        0x48, 0x89, 0xc7,              /* mov rdi, rax  (exit code = main's return) */
        0xb8, 0x3c, 0x00, 0x00, 0x00, /* mov eax, 60  (SYS_exit)        */
        0x0f, 0x05                     /* syscall                        */
    };
    const uint32_t STUB_SIZE = (uint32_t)sizeof(STUB);

    int inject_stub = (has_main && !has_start);

    /* If we inject a stub, shift all text symbol values by STUB_SIZE */
    if (inject_stub) {
        for (int i = 0; i < sym_count; i++) {
            if (symbols[i].defined && !(symbols[i].value & 0x80000000u)) {
                symbols[i].value += STUB_SIZE;
            }
        }
        /* Also shift all text-section reloc offsets */
        for (int i = 0; i < reloc_count; i++) {
            if ((relocs[i].type >> 8) == 0)
                relocs[i].offset += STUB_SIZE;
        }
        /* Add stub bytes to the front of code by reallocating */
        uint32_t new_size = code_size + STUB_SIZE;
        if (new_size > code_capacity) {
            while (code_capacity < new_size) code_capacity *= 2;
            uint8_t *tmp = realloc(code, code_capacity);
            if (!tmp) { fclose(f); return; }
            code = tmp;
        }
        memmove(code + STUB_SIZE, code, code_size);
        memcpy(code, STUB, STUB_SIZE);
        code_size = new_size;

        /* Patch the call displacement in the stub:
         * call at byte 1, next_ip = text_va + 5, target = symbol_va(main) */
        int mi = get_symbol("main");
        if (mi >= 0 && symbols[mi].defined) {
            uint32_t next_ip = elf_text_va() + 5;   /* after the call */
            int32_t  rel     = (int32_t)(symbol_va(symbols[mi].value) - next_ip);
            memcpy(code + 1, &rel, 4);
        }
    }

    /* Re-run reloc resolution now that code has been shifted */
    resolve_relocs();

    uint32_t ehdr_size = 64;
    uint32_t phdr_size = 56;
    uint32_t shdr_size = 64;

    /* Page-aligned layout:
     *   file[0x0000 .. 0x0FFF]: ELF header + program headers (rest is padding)
     *   file[0x1000 .. 0x1FFF]: .text  (one page, executable)
     *   file[0x2000 ..       ]: .data  (one page, writable)
     *   section headers and shstrtab follow data
     *
     * This keeps text and data on separate 4 KB pages so the kernel never
     * strips execute permission from the text when mapping data as R|W. */
    uint32_t text_foff = ELF_PAGE_SIZE;   /* 0x1000 */
    uint32_t text_pages = ((code_size + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1));
    uint32_t data_foff  = text_foff + text_pages;
    uint32_t data_pages = ((data_size + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1));
    uint32_t sh_foff    = data_foff + (data_size > 0 ? data_pages : 0);

    uint32_t text_size = code_size;
    uint32_t ds = data_size;

    /* Set entry point */
    uint64_t entry = 0;
    if (inject_stub) {
        /* _start stub is at the very start of the text section */
        entry = elf_text_va();
    } else {
        int ei = get_symbol("_start");
        if (ei < 0 || !symbols[ei].defined) ei = get_symbol("main");
        if (ei >= 0 && symbols[ei].defined)
            entry = symbol_va(symbols[ei].value);
    }
    
    // ELF64 Header
    uint8_t ehdr[64] = {0};
    ehdr[0] = 0x7F; ehdr[1] = 'E'; ehdr[2] = 'L'; ehdr[3] = 'F';
    ehdr[4] = 2; // 64-bit
    ehdr[5] = 1; // little endian
    ehdr[6] = 1; // ELF version
    ehdr[7] = 0; // OS/ABI = System V
    *(uint16_t*)(ehdr + 16) = 2;          // e_type: ET_EXEC
    *(uint16_t*)(ehdr + 18) = 62;         // e_machine: EM_X86_64
    *(uint32_t*)(ehdr + 20) = 1;          // e_version
    *(uint64_t*)(ehdr + 24) = entry;      // e_entry
    *(uint64_t*)(ehdr + 32) = ehdr_size;  // e_phoff
    *(uint64_t*)(ehdr + 40) = sh_foff;    // e_shoff
    *(uint32_t*)(ehdr + 48) = 0;          // e_flags
    *(uint16_t*)(ehdr + 52) = ehdr_size;  // e_ehsize
    *(uint16_t*)(ehdr + 54) = phdr_size;  // e_phentsize
    *(uint16_t*)(ehdr + 56) = 2;          // e_phnum
    *(uint16_t*)(ehdr + 58) = shdr_size;  // e_shentsize
    *(uint16_t*)(ehdr + 60) = 4;          // e_shnum
    *(uint16_t*)(ehdr + 62) = 3;          // e_shstrndx

    /* Pad from end of headers to text_foff with zeros, then write header */
    fwrite(ehdr, 1, ehdr_size, f);

    // Program header for .text
    uint8_t phdr1[56] = {0};
    *(uint32_t*)(phdr1 + 0)  = 1;                  // PT_LOAD
    *(uint32_t*)(phdr1 + 4)  = 5;                  // PF_R|PF_X
    *(uint64_t*)(phdr1 + 8)  = text_foff;          // p_offset
    *(uint64_t*)(phdr1 + 16) = elf_text_va();      // p_vaddr
    *(uint64_t*)(phdr1 + 24) = elf_text_va();      // p_paddr
    *(uint64_t*)(phdr1 + 32) = text_size;          // p_filesz
    *(uint64_t*)(phdr1 + 40) = text_size;          // p_memsz
    *(uint64_t*)(phdr1 + 48) = ELF_PAGE_SIZE;      // p_align
    fwrite(phdr1, 1, phdr_size, f);

    // Program header for .data
    uint8_t phdr2[56] = {0};
    *(uint32_t*)(phdr2 + 0)  = 1;                  // PT_LOAD
    *(uint32_t*)(phdr2 + 4)  = 6;                  // PF_R|PF_W
    *(uint64_t*)(phdr2 + 8)  = data_foff;          // p_offset
    *(uint64_t*)(phdr2 + 16) = elf_data_va();      // p_vaddr
    *(uint64_t*)(phdr2 + 24) = elf_data_va();      // p_paddr
    *(uint64_t*)(phdr2 + 32) = ds;                 // p_filesz
    *(uint64_t*)(phdr2 + 40) = ds;                 // p_memsz
    *(uint64_t*)(phdr2 + 48) = ELF_PAGE_SIZE;      // p_align
    fwrite(phdr2, 1, phdr_size, f);

    // Pad from end of phdrs to text_foff
    {
        uint32_t written = ehdr_size + phdr_size * 2;
        for (uint32_t i = written; i < text_foff; i++) fputc(0, f);
    }

    // Write .text section
    fwrite(code, 1, code_size, f);
    // Pad to next page boundary
    for (uint32_t i = code_size; i < text_pages; i++) fputc(0, f);

    // Write .data section
    if (ds > 0) {
        fwrite(data, 1, ds, f);
        for (uint32_t i = ds; i < data_pages; i++) fputc(0, f);
    }

    // Section header for null
    uint8_t shdr0[64] = {0};
    fwrite(shdr0, 1, shdr_size, f);

    // Section header for .text
    uint8_t shdr1[64] = {0};
    *(uint32_t*)(shdr1 + 0)  = 1;              // sh_name
    *(uint32_t*)(shdr1 + 4)  = 1;              // sh_type: SHT_PROGBITS
    *(uint64_t*)(shdr1 + 8)  = 6;              // sh_flags: SHF_ALLOC|SHF_EXECINSTR
    *(uint64_t*)(shdr1 + 16) = elf_text_va();  // sh_addr
    *(uint64_t*)(shdr1 + 24) = text_foff;      // sh_offset
    *(uint64_t*)(shdr1 + 32) = text_size;      // sh_size
    *(uint64_t*)(shdr1 + 48) = 16;             // sh_addralign
    fwrite(shdr1, 1, shdr_size, f);

    // Section header for .data
    uint8_t shdr2[64] = {0};
    *(uint32_t*)(shdr2 + 0)  = 7;              // sh_name
    *(uint32_t*)(shdr2 + 4)  = 1;              // sh_type: SHT_PROGBITS
    *(uint64_t*)(shdr2 + 8)  = 3;              // sh_flags: SHF_ALLOC|SHF_WRITE
    *(uint64_t*)(shdr2 + 16) = elf_data_va();  // sh_addr
    *(uint64_t*)(shdr2 + 24) = data_foff;      // sh_offset
    *(uint64_t*)(shdr2 + 32) = ds;             // sh_size
    *(uint64_t*)(shdr2 + 48) = 8;              // sh_addralign
    fwrite(shdr2, 1, shdr_size, f);

    // Section header for .shstrtab
    uint64_t shstrtab_foff = sh_foff + (uint64_t)(shdr_size * 4);
    uint8_t shdr3[64] = {0};
    *(uint32_t*)(shdr3 + 0)  = 13;             // sh_name
    *(uint32_t*)(shdr3 + 4)  = 3;              // sh_type: SHT_STRTAB
    *(uint64_t*)(shdr3 + 24) = shstrtab_foff;  // sh_offset
    *(uint64_t*)(shdr3 + 32) = 23;             // sh_size
    *(uint64_t*)(shdr3 + 48) = 1;              // sh_addralign
    fwrite(shdr3, 1, shdr_size, f);

    // Section string table
    const char *shstrtab = "\0.text\0.data\0.shstrtab";
    fwrite(shstrtab, 1, 23, f);

    fclose(f);
}

void write_obj(const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    
    uint32_t ehdr_size = 64;
    uint32_t shdr_size = 64;
    
    uint32_t text_off = ehdr_size;
    uint32_t data_off = text_off + ((code_size + 15) & ~15);
    uint32_t sh_off = data_off + ((data_size + 15) & ~15);
    
    // ELF64 Header for relocatable object
    uint8_t ehdr[64] = {0};
    ehdr[0] = 0x7F; ehdr[1] = 'E'; ehdr[2] = 'L'; ehdr[3] = 'F';
    ehdr[4] = 2; // 64-bit
    ehdr[5] = 1; // little endian
    ehdr[6] = 1; // ELF version
    ehdr[7] = 0; // OS/ABI = System V
    *(uint16_t*)(ehdr + 16) = 1;   // e_type: ET_REL
    *(uint16_t*)(ehdr + 18) = 62;  // e_machine: EM_X86_64
    *(uint32_t*)(ehdr + 20) = 1;   // e_version
    
    *(uint64_t*)(ehdr + 24) = 0;       // e_entry
    *(uint64_t*)(ehdr + 32) = 0;       // e_phoff
    *(uint64_t*)(ehdr + 40) = sh_off;  // e_shoff
    *(uint32_t*)(ehdr + 48) = 0;       // e_flags
    *(uint16_t*)(ehdr + 52) = ehdr_size; // e_ehsize
    *(uint16_t*)(ehdr + 54) = 0;       // e_phentsize
    *(uint16_t*)(ehdr + 56) = 0;       // e_phnum
    *(uint16_t*)(ehdr + 58) = shdr_size; // e_shentsize
    *(uint16_t*)(ehdr + 60) = 3;       // e_shnum (null + .text + .data)
    *(uint16_t*)(ehdr + 62) = 0;       // e_shstrndx (no strtab in obj)
    
    fwrite(ehdr, 1, ehdr_size, f);
    
    // Write code
    fwrite(code, 1, code_size, f);
    uint32_t padding = ((code_size + 15) & ~15) - code_size;
    for (uint32_t i = 0; i < padding; i++) fputc(0, f);
    
    // Write data
    if (data_size > 0) {
        fwrite(data, 1, data_size, f);
        padding = ((data_size + 15) & ~15) - data_size;
        for (uint32_t i = 0; i < padding; i++) fputc(0, f);
    }
    
    // Section headers
    uint8_t shdr0[64] = {0};
    fwrite(shdr0, 1, shdr_size, f);
    
    uint8_t shdr1[64] = {0};
    *(uint32_t*)(shdr1 + 0) = 1;
    *(uint32_t*)(shdr1 + 4) = 1;
    *(uint64_t*)(shdr1 + 8) = 0;
    *(uint64_t*)(shdr1 + 16) = 0;
    *(uint64_t*)(shdr1 + 24) = text_off;
    *(uint64_t*)(shdr1 + 32) = code_size;
    *(uint32_t*)(shdr1 + 40) = 0;
    *(uint32_t*)(shdr1 + 44) = 0;
    fwrite(shdr1, 1, shdr_size, f);
    
    uint8_t shdr2[64] = {0};
    *(uint32_t*)(shdr2 + 0) = 7;
    *(uint32_t*)(shdr2 + 4) = 1;
    *(uint64_t*)(shdr2 + 8) = 0;
    *(uint64_t*)(shdr2 + 16) = 0;
    *(uint64_t*)(shdr2 + 24) = data_off;
    *(uint64_t*)(shdr2 + 32) = data_size;
    *(uint32_t*)(shdr2 + 40) = 0;
    *(uint32_t*)(shdr2 + 44) = 0;
    fwrite(shdr2, 1, shdr_size, f);
    
    // Section string table
    const char *shstrtab = "\0.text\0.data\0.shstrtab";
    fwrite(shstrtab, 1, 23, f);
    
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input.asm> <output> [obj|exe]\n", argv[0]);
        return 1;
    }
    
    code = malloc(code_capacity);
    data = malloc(data_capacity);
    
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        printf("Cannot open %s\n", argv[1]);
        free(code);
        free(data);
        return 1;
    }
    
    char line[MAX_LINE];
    while (fgets(line, MAX_LINE, f)) {
        assemble_line(line);
    }
    fclose(f);

    if (argc > 3 && strcmp(argv[3], "obj") == 0) {
        resolve_relocs();   /* obj has no stub injection */
        write_obj(argv[2]);
    } else {
        write_elf(argv[2]); /* write_elf calls resolve_relocs internally */
    }
    
    free(code);
    free(data);
    return 0;
}