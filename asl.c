/*
 * asl.c — single-file x86-64 assembler + ELF linker, GCC-pipeline compatible
 *
 * Usage (assembler):  asl -o foo.o foo.s
 * Usage (linker):     asl -o foo    foo.o bar.o [--entry sym]
 * Usage (combined):   asl -o foo    foo.s bar.s
 *
 * Build:  cc -O2 -o asl asl.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>

/* ───────── ELF64 structs ───────── */
#define ET_REL 1
#define ET_EXEC 2
#define EM_X86_64 62
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STV_DEFAULT 0
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_8 14
#define R_X86_64_PC8 15
#define ELF_ST_BIND(i) ((i)>>4)
#define ELF_ST_TYPE(i) ((i)&0xf)
#define ELF_ST_INFO(b,t) (((b)<<4)|((t)&0xf))
#define ELF_R_SYM(i) ((uint32_t)((i)>>32))
#define ELF_R_TYPE(i) ((uint32_t)(i))
#define ELF_R_INFO(s,t) (((uint64_t)(s)<<32)|(uint32_t)(t))

typedef struct{uint8_t e_ident[16];uint16_t e_type,e_machine;
  uint32_t e_version;uint64_t e_entry,e_phoff,e_shoff;
  uint32_t e_flags;uint16_t e_ehsize,e_phentsize,e_phnum,
  e_shentsize,e_shnum,e_shstrndx;}Elf64_Ehdr;
typedef struct{uint32_t sh_name,sh_type;uint64_t sh_flags,sh_addr,
  sh_offset,sh_size;uint32_t sh_link,sh_info;
  uint64_t sh_addralign,sh_entsize;}Elf64_Shdr;
typedef struct{uint32_t st_name;uint8_t st_info,st_other;
  uint16_t st_shndx;uint64_t st_value,st_size;}Elf64_Sym;
typedef struct{uint64_t r_offset,r_info;int64_t r_addend;}Elf64_Rela;
typedef struct{uint32_t p_type,p_flags;uint64_t p_offset,p_vaddr,
  p_paddr,p_filesz,p_memsz,p_align;}Elf64_Phdr;

/* ───────── error ───────── */
static const char *g_file="<input>";
static int g_line=0;
static void die(const char*fmt,...){
  va_list ap;va_start(ap,fmt);
  fprintf(stderr,"%s:%d: error: ",g_file,g_line);
  vfprintf(stderr,fmt,ap);fputc('\n',stderr);exit(1);}
static void warn(const char*fmt,...){
  va_list ap;va_start(ap,fmt);
  fprintf(stderr,"%s:%d: warning: ",g_file,g_line);
  vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);}

/* ───────── byte buffer ───────── */
typedef struct{uint32_t n,cap;uint8_t*d;}ByteBuf;
static void bb_push(ByteBuf*b,uint8_t v){
  if(b->n>=b->cap){b->cap=b->cap?2*b->cap:64;
    b->d=realloc(b->d,b->cap);}
  b->d[b->n++]=v;}
static void bb_write(ByteBuf*b,const void*data,uint32_t len){
  for(uint32_t i=0;i<len;i++)bb_push(b,((uint8_t*)data)[i]);}
static void bb_align(ByteBuf*b,uint32_t align){
  while(b->n&(align-1))bb_push(b,0);}

/* ───────── relocation ───────── */
typedef struct{uint64_t offset;int64_t addend;uint32_t symidx,type;}Reloc;
typedef struct{Reloc*d;uint32_t n,cap;}RelocBuf;

/* ───────── section ───────── */
typedef struct{
  char name[64];ByteBuf data;RelocBuf relocs;
  uint64_t flags;uint32_t type,align;int is_bss;
}Section;
#define MAX_SECS 32
static Section secs[MAX_SECS];
static uint32_t nsecs=0,cur_sec=0;

static uint32_t sec_find(const char*n){
  for(uint32_t i=0;i<nsecs;i++)if(!strcmp(secs[i].name,n))return i;
  return (uint32_t)-1;}
static uint32_t sec_get(const char*n,uint64_t fl,uint32_t ty,int bss){
  uint32_t i=sec_find(n);if(i!=(uint32_t)-1)return i;
  if(nsecs>=MAX_SECS)die("too many sections");
  i=nsecs++;Section*s=&secs[i];memset(s,0,sizeof*s);
  strncpy(s->name,n,63);s->flags=fl;s->type=ty;s->is_bss=bss;s->align=1;
  return i;}
static Section*cur_section(void){return&secs[cur_sec];}

/* ───────── symbol table ───────── */
typedef struct{
  char name[128];uint64_t value;uint32_t secidx;
  uint8_t bind,type;int defined;}Sym;
#define MAX_SYMS 8192
static Sym syms[MAX_SYMS];
static uint32_t nsyms=0;
static uint32_t sym_find(const char*n){
  for(uint32_t i=0;i<nsyms;i++)if(!strcmp(syms[i].name,n))return i;
  return(uint32_t)-1;}
static uint32_t sym_get(const char*n){
  uint32_t i=sym_find(n);if(i!=(uint32_t)-1)return i;
  if(nsyms>=MAX_SYMS)die("too many symbols");
  i=nsyms++;strncpy(syms[i].name,n,127);
  syms[i].value=0;syms[i].secidx=0;
  syms[i].bind=STB_LOCAL;syms[i].type=STT_NOTYPE;syms[i].defined=0;
  return i;}

/* ───────── lexer ───────── */
typedef enum{T_EOF,T_EOL,T_IDENT,T_INT,T_STR,
  T_COMMA,T_COLON,T_LBRACK,T_RBRACK,T_PLUS,T_MINUS,T_STAR,T_DOLLAR}TokKind;
typedef struct{TokKind kind;char s[256];int64_t i;}Tok;
static char*src_ptr;
static Tok tok;

/* forward declarations for local numeric label support */
static uint32_t local_label_seq=0;
static uint32_t local_label_last[10];

static void lex_next(void){
  while(*src_ptr==' '||*src_ptr=='\t'||*src_ptr=='\r')src_ptr++;
  char c=*src_ptr;
  if(!c||c=='\n'){tok.kind=(c?T_EOL:T_EOF);
    if(c){src_ptr++;g_line++;}return;}
  if(c=='#'||c==';'){while(*src_ptr&&*src_ptr!='\n')src_ptr++;
    tok.kind=T_EOL;if(*src_ptr){src_ptr++;g_line++;}return;}
  if(c=='/'&&src_ptr[1]=='/'){while(*src_ptr&&*src_ptr!='\n')src_ptr++;
    tok.kind=T_EOL;if(*src_ptr){src_ptr++;g_line++;}return;}
  if(c=='"'){src_ptr++;int j=0;
    while(*src_ptr&&*src_ptr!='"'){char ch=*src_ptr++;
      if(ch=='\\'){ch=*src_ptr++;
        switch(ch){case'n':ch='\n';break;case't':ch='\t';break;
          case'r':ch='\r';break;case'0':ch='\0';break;
          case'\\':ch='\\';break;case'"':ch='"';break;}}
      if(j<255)tok.s[j++]=ch;}
    if(*src_ptr=='"')src_ptr++;
    tok.s[j]=0;tok.kind=T_STR;return;}
  if(isdigit(c)||(c=='-'&&isdigit(src_ptr[1]))||
     (c=='0'&&(src_ptr[1]=='x'||src_ptr[1]=='X'))){
    char*end;tok.i=strtoll(src_ptr,&end,0);
    tok.kind=T_INT;src_ptr=end;return;}
  if(isalpha(c)||c=='_'||c=='.'||c=='%'||c=='@'){
    int j=0;
    while(isalnum(*src_ptr)||*src_ptr=='_'||*src_ptr=='.'||
          *src_ptr=='%'||*src_ptr=='@'){
      if(j<255)tok.s[j++]=*src_ptr;src_ptr++;}
    tok.s[j]=0;tok.kind=T_IDENT;return;}
  src_ptr++;
  switch(c){
    case',':tok.kind=T_COMMA;break;case':':tok.kind=T_COLON;break;
    case'[':tok.kind=T_LBRACK;break;case']':tok.kind=T_RBRACK;break;
    case'(':tok.kind=T_LBRACK;break;case')':tok.kind=T_RBRACK;break;
    case'+':tok.kind=T_PLUS;break;case'-':tok.kind=T_MINUS;break;
    case'*':tok.kind=T_STAR;break;case'$':tok.kind=T_DOLLAR;break;
    default:tok.kind=T_IDENT;tok.s[0]=c;tok.s[1]=0;break;}}

static void skip_eol(void){while(tok.kind==T_EOL)lex_next();}
static int at_eol(void){return tok.kind==T_EOL||tok.kind==T_EOF;}

/* ───────── register table ───────── */
typedef struct{const char*name;uint8_t idx,size;}RegInfo;
static const RegInfo regs[]={
  {"rax",0,8},{"rcx",1,8},{"rdx",2,8},{"rbx",3,8},
  {"rsp",4,8},{"rbp",5,8},{"rsi",6,8},{"rdi",7,8},
  {"r8",8,8},{"r9",9,8},{"r10",10,8},{"r11",11,8},
  {"r12",12,8},{"r13",13,8},{"r14",14,8},{"r15",15,8},
  {"eax",0,4},{"ecx",1,4},{"edx",2,4},{"ebx",3,4},
  {"esp",4,4},{"ebp",5,4},{"esi",6,4},{"edi",7,4},
  {"r8d",8,4},{"r9d",9,4},{"r10d",10,4},{"r11d",11,4},
  {"r12d",12,4},{"r13d",13,4},{"r14d",14,4},{"r15d",15,4},
  {"ax",0,2},{"cx",1,2},{"dx",2,2},{"bx",3,2},
  {"sp",4,2},{"bp",5,2},{"si",6,2},{"di",7,2},
  {"al",0,1},{"cl",1,1},{"dl",2,1},{"bl",3,1},
  {"spl",4,1},{"bpl",5,1},{"sil",6,1},{"dil",7,1},
  {"r8b",8,1},{"r9b",9,1},{"r10b",10,1},{"r11b",11,1},
  {"r12b",12,1},{"r13b",13,1},{"r14b",14,1},{"r15b",15,1},
  {"rip",16,8},{NULL,0,0}};
static const RegInfo*reg_find(const char*name){
  const char*n=(name[0]=='%')?name+1:name;
  for(int i=0;regs[i].name;i++)
    if(!strcasecmp(regs[i].name,n))return&regs[i];
  return NULL;}

/* ───────── operand ───────── */
typedef enum{OP_NONE,OP_REG,OP_IMM,OP_MEM,OP_LABEL}OpKind;
typedef struct{
  OpKind kind;uint8_t reg,regsize;
  uint8_t base,idx,scale;int64_t disp,imm;
  char label[128];int64_t addend;uint8_t size;int att;
}Operand;

/* resolve Nf/Nb local label reference */
static void resolve_local_ref(char*name){
  size_t ln=strlen(name);
  if(ln==2&&name[0]>='0'&&name[0]<='9'&&(name[1]=='f'||name[1]=='b')){
    int d=name[0]-'0';
    if(name[1]=='b'){
      if(local_label_last[d]!=0xffffffff)
        snprintf(name,128,".Lloc_%d_%u",d,local_label_last[d]);
    }else{
      snprintf(name,128,".Lloc_%d_%u",d,local_label_seq);
    }
  }
}

static Operand parse_operand(void){
  Operand op;memset(&op,0,sizeof op);
  op.base=0xff;op.idx=0xff;op.scale=1;

  /* size override */
  if(tok.kind==T_IDENT){
    uint8_t sz=0;
    if(!strcasecmp(tok.s,"byte"))sz=1;
    else if(!strcasecmp(tok.s,"word"))sz=2;
    else if(!strcasecmp(tok.s,"dword"))sz=4;
    else if(!strcasecmp(tok.s,"qword"))sz=8;
    if(sz){op.size=sz;lex_next();
      if(tok.kind==T_IDENT&&!strcasecmp(tok.s,"ptr"))lex_next();}}

  /* AT&T immediate */
  if(tok.kind==T_DOLLAR){op.att=1;lex_next();
    if(tok.kind==T_INT){op.kind=OP_IMM;op.imm=tok.i;lex_next();return op;}
    if(tok.kind==T_MINUS){lex_next();op.kind=OP_IMM;op.imm=-tok.i;lex_next();return op;}
    if(tok.kind==T_IDENT){
      op.kind=OP_LABEL;strncpy(op.label,tok.s,127);
      resolve_local_ref(op.label);lex_next();
      if(tok.kind==T_PLUS){lex_next();op.addend=tok.i;lex_next();}
      return op;}}

  /* segment override: %fs:N or %gs:N */
  if(tok.kind==T_IDENT&&(
     !strcasecmp(tok.s,"%fs")||!strcasecmp(tok.s,"fs")||
     !strcasecmp(tok.s,"%gs")||!strcasecmp(tok.s,"gs"))){
    op.att=1;lex_next();
    if(tok.kind==T_COLON)lex_next();
    if(tok.kind==T_INT){op.kind=OP_MEM;op.disp=tok.i;lex_next();}
    else if(tok.kind==T_LBRACK){lex_next();
      if(tok.kind==T_IDENT){const RegInfo*r=reg_find(tok.s);
        if(r){op.base=r->idx;lex_next();}}
      if(tok.kind==T_RBRACK)lex_next();op.kind=OP_MEM;}
    return op;}

  /* memory: [...] or disp(...) */
  if(tok.kind==T_LBRACK){
    lex_next();op.kind=OP_MEM;int neg=0;
    if(tok.kind==T_MINUS){neg=1;lex_next();}
    if(tok.kind==T_COMMA){
      /* (,index,scale) form */
      op.att=1;lex_next();
      if(tok.kind==T_IDENT){const RegInfo*ri=reg_find(tok.s);
        if(ri){op.idx=ri->idx;lex_next();}
        if(tok.kind==T_COMMA){lex_next();
          if(tok.kind==T_INT){op.scale=(uint8_t)tok.i;lex_next();}}}
    }else if(tok.kind==T_IDENT){
      if(tok.s[0]=='%')op.att=1;
      const RegInfo*r=reg_find(tok.s);
      if(r){op.base=r->idx;lex_next();
        /* AT&T (base,idx,scale) */
        if(tok.kind==T_COMMA){lex_next();
          if(tok.kind==T_IDENT){const RegInfo*r2=reg_find(tok.s);
            if(r2){op.idx=r2->idx;lex_next();}
            if(tok.kind==T_COMMA){lex_next();
              if(tok.kind==T_INT){op.scale=(uint8_t)tok.i;lex_next();}}}
        /* Intel [base+idx*scale+disp] */
        }else if(tok.kind==T_PLUS||tok.kind==T_MINUS){
          int s2=(tok.kind==T_MINUS)?-1:1;lex_next();
          const RegInfo*r2=reg_find(tok.kind==T_IDENT?tok.s:"");
          if(r2){op.idx=r2->idx;lex_next();
            if(tok.kind==T_STAR){lex_next();op.scale=(uint8_t)tok.i;lex_next();}
            if(tok.kind==T_PLUS||tok.kind==T_MINUS){
              int s3=(tok.kind==T_MINUS)?-1:1;lex_next();
              op.disp=s3*tok.i;lex_next();}
          }else if(tok.kind==T_INT){op.disp=s2*tok.i;lex_next();}
          else if(tok.kind==T_IDENT){
            strncpy(op.label,tok.s,127);resolve_local_ref(op.label);lex_next();
            if(tok.kind==T_PLUS){lex_next();op.disp=tok.i;lex_next();}}
        }
      }else{
        strncpy(op.label,tok.s,127);resolve_local_ref(op.label);
        op.base=0xff;lex_next();
        if(tok.kind==T_PLUS){lex_next();op.disp=tok.i;lex_next();}}
    }else if(tok.kind==T_INT){op.disp=(neg?-1:1)*tok.i;lex_next();}
    if(tok.kind==T_RBRACK)lex_next();
    return op;}

  /* register */
  if(tok.kind==T_IDENT){
    if(tok.s[0]=='%')op.att=1;
    const RegInfo*r=reg_find(tok.s);
    if(r){op.kind=OP_REG;op.reg=r->idx;op.regsize=r->size;lex_next();return op;}
    /* label */
    op.kind=OP_LABEL;strncpy(op.label,tok.s,127);
    resolve_local_ref(op.label);lex_next();
    if(tok.kind==T_PLUS){lex_next();op.addend=tok.i;lex_next();}
    if(tok.kind==T_MINUS){lex_next();op.addend=-tok.i;lex_next();}
    return op;}

  /* bare integer, possibly disp(base) */
  if(tok.kind==T_INT){
    int64_t disp=tok.i;lex_next();
    if(tok.kind==T_LBRACK){
      lex_next();op.kind=OP_MEM;op.disp=disp;
      if(tok.kind==T_IDENT){
        if(tok.s[0]=='%')op.att=1;
        const RegInfo*rb=reg_find(tok.s);
        if(rb){op.base=rb->idx;lex_next();}
        if(tok.kind==T_COMMA){lex_next();
          if(tok.kind==T_IDENT){const RegInfo*ri=reg_find(tok.s);
            if(ri){op.idx=ri->idx;lex_next();}
            if(tok.kind==T_COMMA){lex_next();
              if(tok.kind==T_INT){op.scale=(uint8_t)tok.i;lex_next();}}}
        }}
      if(tok.kind==T_RBRACK)lex_next();
      return op;}
    op.kind=OP_IMM;op.imm=disp;return op;}

  return op;}

/* ───────── emitters ───────── */
static void emit8(uint8_t v){bb_push(&cur_section()->data,v);}
static void emit16(uint16_t v){emit8(v&0xff);emit8(v>>8);}
static void emit32(uint32_t v){emit16(v&0xffff);emit16(v>>16);}
static void emit64(uint64_t v){emit32((uint32_t)v);emit32((uint32_t)(v>>32));}
static uint64_t cur_off(void){return cur_section()->data.n;}

static void add_reloc(uint64_t off,uint32_t si,uint32_t type,int64_t addend){
  Section*s=cur_section();
  Reloc r={off,addend,si,type};
  if(s->relocs.n>=s->relocs.cap){
    s->relocs.cap=s->relocs.cap?2*s->relocs.cap:8;
    s->relocs.d=realloc(s->relocs.d,s->relocs.cap*sizeof(Reloc));}
  s->relocs.d[s->relocs.n++]=r;}

static uint8_t rex(int W,int R,int X,int B){
  return 0x40|(W<<3)|(R<<2)|(X<<1)|B;}
static void emit_rex(int W,uint8_t reg,uint8_t idx,uint8_t rm){
  int R=(reg>7)?1:0,X=(idx!=0xff&&idx>7)?1:0,B=(rm>7)?1:0;
  if(W||R||X||B)emit8(rex(W,R,X,B));}

static void emit_modrm_sib(uint8_t reg_f,const Operand*m){
  uint8_t base=m->base,idx=m->idx;
  /* disp32-only: mod=0,rm=4,SIB=0x25 */
  if(base==0xff&&idx==0xff){
    emit8((0<<6)|((reg_f&7)<<3)|4);emit8(0x25);
    if(m->label[0]){uint32_t si=sym_get(m->label);
      add_reloc(cur_off(),si,R_X86_64_32S,m->disp);emit32(0);}
    else emit32((uint32_t)m->disp);return;}
  int has_sib=(idx!=0xff||(base!=0xff&&(base&7)==4));
  int disp32=(m->disp>0x7f||m->disp<-0x80||m->label[0]);
  int disp8=(m->disp!=0&&!disp32&&!m->label[0]);
  uint8_t mod=0;
  if(disp8)mod=1;else if(disp32||m->label[0])mod=2;
  uint8_t rm_f=has_sib?4:(base&7);
  /* [rbp/r13]+0 needs disp8=0 to disambiguate */
  if(mod==0&&(base&7)==5){mod=1;disp8=1;disp32=0;}
  emit8((mod<<6)|((reg_f&7)<<3)|rm_f);
  if(has_sib){
    uint8_t ss=0;
    if(m->scale==2)ss=1;else if(m->scale==4)ss=2;else if(m->scale==8)ss=3;
    uint8_t si2=(idx==0xff)?4:(idx&7);
    uint8_t sb=(base==0xff)?5:(base&7);
    emit8((ss<<6)|(si2<<3)|sb);}
  if(m->label[0]){uint32_t si=sym_get(m->label);
    add_reloc(cur_off(),si,R_X86_64_32S,m->disp);emit32(0);}
  else if(disp8)emit8((uint8_t)(int8_t)m->disp);
  else if(disp32||mod==2)emit32((uint32_t)m->disp);}

/* ───────── encoders ───────── */
static void enc_alu(uint8_t opc_mr,const Operand*dst,const Operand*src,int sz){
  int W=(sz==8)?1:0;uint8_t opc_rm=opc_mr+2;
  if(src->kind==OP_REG&&dst->kind==OP_REG){
    emit_rex(W,src->reg,0xff,dst->reg);emit8(opc_mr);
    emit8(0xC0|((src->reg&7)<<3)|(dst->reg&7));
  }else if(dst->kind==OP_REG&&src->kind==OP_MEM){
    emit_rex(W,dst->reg,src->idx!=0xff?src->idx:0,src->base!=0xff?src->base:0);
    emit8(opc_rm);emit_modrm_sib(dst->reg,src);
  }else if(dst->kind==OP_MEM&&src->kind==OP_REG){
    emit_rex(W,src->reg,dst->idx!=0xff?dst->idx:0,dst->base!=0xff?dst->base:0);
    emit8(opc_mr);emit_modrm_sib(src->reg,dst);}}

static void enc_alu_imm(uint8_t slash,const Operand*dst,int64_t imm,int sz){
  int W=(sz==8)?1:0;int imm8=(imm>=-128&&imm<=127);
  if(dst->kind==OP_REG){
    if(sz==2)emit8(0x66);
    emit_rex(W,0,0xff,dst->reg);
    if(imm8&&sz!=1){emit8(0x83);emit8(0xC0|(slash<<3)|(dst->reg&7));emit8((uint8_t)imm);}
    else if(sz==1){emit8(0x80);emit8(0xC0|(slash<<3)|(dst->reg&7));emit8((uint8_t)imm);}
    else if(sz==2){emit8(0x81);emit8(0xC0|(slash<<3)|(dst->reg&7));emit16((uint16_t)imm);}
    else{emit8(0x81);emit8(0xC0|(slash<<3)|(dst->reg&7));emit32((uint32_t)imm);}
  }else if(dst->kind==OP_MEM){
    if(sz==2)emit8(0x66);
    emit_rex(W,0,dst->idx!=0xff?dst->idx:0,dst->base!=0xff?dst->base:0);
    if(imm8&&sz!=1){emit8(0x83);emit_modrm_sib(slash,dst);emit8((uint8_t)imm);}
    else if(sz==1){emit8(0x80);emit_modrm_sib(slash,dst);emit8((uint8_t)imm);}
    else if(sz==2){emit8(0x81);emit_modrm_sib(slash,dst);emit16((uint16_t)imm);}
    else{emit8(0x81);emit_modrm_sib(slash,dst);emit32((uint32_t)imm);}}}

static void enc_mov(const Operand*dst,const Operand*src,int sz){
  int W=(sz==8)?1:0;
  if(dst->kind==OP_REG&&src->kind==OP_IMM){
    emit_rex(W,0,0xff,dst->reg);
    emit8(0xB8|(dst->reg&7));
    if(W)emit64((uint64_t)src->imm);else emit32((uint32_t)src->imm);return;}
  if(dst->kind==OP_REG&&src->kind==OP_LABEL){
    emit_rex(1,0,0xff,dst->reg);emit8(0xB8|(dst->reg&7));
    uint32_t si=sym_get(src->label);
    add_reloc(cur_off(),si,R_X86_64_64,src->addend);emit64(0);return;}
  if(dst->kind==OP_REG&&src->kind==OP_REG){
    emit_rex(W,src->reg,0xff,dst->reg);
    emit8(sz==1?0x88:0x89);
    emit8(0xC0|((src->reg&7)<<3)|(dst->reg&7));return;}
  if(dst->kind==OP_REG&&src->kind==OP_MEM){
    if(sz==2)emit8(0x66);
    emit_rex(W,dst->reg,src->idx!=0xff?src->idx:0,src->base!=0xff?src->base:0);
    emit8(sz==1?0x8A:0x8B);emit_modrm_sib(dst->reg,src);return;}
  if(dst->kind==OP_MEM&&src->kind==OP_REG){
    if(sz==2)emit8(0x66);
    emit_rex(W,src->reg,dst->idx!=0xff?dst->idx:0,dst->base!=0xff?dst->base:0);
    emit8(sz==1?0x88:0x89);emit_modrm_sib(src->reg,dst);return;}
  if(dst->kind==OP_MEM&&src->kind==OP_IMM){
    if(sz==2)emit8(0x66);
    emit_rex(W,0,dst->idx!=0xff?dst->idx:0,dst->base!=0xff?dst->base:0);
    emit8(sz==1?0xC6:0xC7);emit_modrm_sib(0,dst);
    if(sz==1)emit8((uint8_t)src->imm);
    else if(sz==2)emit16((uint16_t)src->imm);
    else emit32((uint32_t)src->imm);return;}
  die("mov: unsupported operand combination");}

static void enc_lea(const Operand*dst,const Operand*src){
  if(dst->kind!=OP_REG||src->kind!=OP_MEM)die("lea: reg,mem required");
  int W=(dst->regsize==4)?0:1;
  emit_rex(W,dst->reg,src->idx!=0xff?src->idx:0,src->base!=0xff?src->base:0);
  emit8(0x8D);emit_modrm_sib(dst->reg,src);}

static void enc_push(const Operand*op){
  if(op->kind==OP_REG){
    if(op->reg>7)emit8(0x41);emit8(0x50|(op->reg&7));
  }else if(op->kind==OP_IMM){
    if(op->imm>=-128&&op->imm<=127){emit8(0x6A);emit8((uint8_t)op->imm);}
    else{emit8(0x68);emit32((uint32_t)op->imm);}
  }else die("push: unsupported operand");}

static void enc_pop(const Operand*op){
  if(op->kind!=OP_REG)die("pop: register required");
  if(op->reg>7)emit8(0x41);emit8(0x58|(op->reg&7));}

static void enc_call(const Operand*op){
  if(op->kind==OP_REG){
    if(op->reg>7)emit8(0x41);emit8(0xFF);emit8(0xD0|(op->reg&7));return;}
  if(op->kind==OP_LABEL){
    emit8(0xE8);uint32_t si=sym_get(op->label);
    add_reloc(cur_off(),si,R_X86_64_PC32,-4+op->addend);emit32(0);return;}
  die("call: unsupported operand");}

static void enc_jmp(const Operand*op){
  if(op->kind==OP_REG){
    if(op->reg>7)emit8(0x41);emit8(0xFF);emit8(0xE0|(op->reg&7));return;}
  if(op->kind==OP_LABEL){
    uint32_t si=sym_get(op->label);
    /* short form for backward refs */
    if(syms[si].defined&&syms[si].secidx==cur_sec+1){
      int64_t off=(int64_t)syms[si].value-(int64_t)(cur_off()+2);
      if(off>=-128&&off<=127){emit8(0xEB);emit8((uint8_t)(int8_t)off);return;}}
    emit8(0xE9);add_reloc(cur_off(),si,R_X86_64_PC32,-4+op->addend);emit32(0);return;}
  die("jmp: unsupported operand");}

static const struct{const char*name;uint8_t opc;}jcc_tab[]={
  {"je",0x84},{"jz",0x84},{"jne",0x85},{"jnz",0x85},
  {"jl",0x8C},{"jnge",0x8C},{"jge",0x8D},{"jnl",0x8D},
  {"jle",0x8E},{"jng",0x8E},{"jg",0x8F},{"jnle",0x8F},
  {"jb",0x82},{"jnae",0x82},{"jae",0x83},{"jnb",0x83},
  {"jbe",0x86},{"jna",0x86},{"ja",0x87},{"jnbe",0x87},
  {"js",0x88},{"jns",0x89},{"jp",0x8A},{"jnp",0x8B},
  {"jo",0x80},{"jno",0x81},{NULL,0}};

static int enc_jcc(const char*name,const Operand*op){
  for(int i=0;jcc_tab[i].name;i++){
    if(!strcasecmp(jcc_tab[i].name,name)){
      if(op->kind!=OP_LABEL)die("%s: label required",name);
      uint32_t si=sym_get(op->label);
      /* short form for backward refs */
      if(syms[si].defined&&syms[si].secidx==cur_sec+1){
        int64_t off=(int64_t)syms[si].value-(int64_t)(cur_off()+2);
        if(off>=-128&&off<=127){
          emit8(jcc_tab[i].opc-0x10);emit8((uint8_t)(int8_t)off);return 1;}}
      emit8(0x0F);emit8(jcc_tab[i].opc);
      add_reloc(cur_off(),si,R_X86_64_PC32,-4+op->addend);emit32(0);return 1;}}
  return 0;}

static void enc_unary(uint8_t slash,const Operand*op,int sz){
  int W=(sz==8)?1:0;
  if(op->kind==OP_REG){
    emit_rex(W,0,0xff,op->reg);
    emit8(W?0xF7:0xF6);emit8(0xC0|(slash<<3)|(op->reg&7));
  }else if(op->kind==OP_MEM){
    emit_rex(W,0,op->idx!=0xff?op->idx:0,op->base!=0xff?op->base:0);
    emit8(W?0xF7:0xF6);emit_modrm_sib(slash,op);}}

static void enc_shift(uint8_t slash,const Operand*dst,const Operand*cnt,int sz){
  int W=(sz==8)?1:0;
  if(dst->kind==OP_REG){
    emit_rex(W,0,0xff,dst->reg);
    if(cnt->kind==OP_IMM&&cnt->imm==1){emit8(W?0xD1:0xD0);emit8(0xC0|(slash<<3)|(dst->reg&7));}
    else if(cnt->kind==OP_IMM){emit8(W?0xC1:0xC0);emit8(0xC0|(slash<<3)|(dst->reg&7));emit8((uint8_t)cnt->imm);}
    else{emit8(W?0xD3:0xD2);emit8(0xC0|(slash<<3)|(dst->reg&7));}}
  else if(dst->kind==OP_MEM){
    emit_rex(W,0,dst->idx!=0xff?dst->idx:0,dst->base!=0xff?dst->base:0);
    if(cnt->kind==OP_IMM&&cnt->imm==1){emit8(W?0xD1:0xD0);emit_modrm_sib(slash,dst);}
    else if(cnt->kind==OP_IMM){emit8(W?0xC1:0xC0);emit_modrm_sib(slash,dst);emit8((uint8_t)cnt->imm);}
    else{emit8(W?0xD3:0xD2);emit_modrm_sib(slash,dst);}}}

static void enc_imul2(const Operand*dst,const Operand*src,int sz){
  int W=(sz==8)?1:0;
  if(dst->kind==OP_REG&&src->kind==OP_REG){
    emit_rex(W,dst->reg,0xff,src->reg);
    emit8(0x0F);emit8(0xAF);emit8(0xC0|((dst->reg&7)<<3)|(src->reg&7));
  }else if(dst->kind==OP_REG&&src->kind==OP_IMM){
    emit_rex(W,dst->reg,0xff,dst->reg);
    if(src->imm>=-128&&src->imm<=127){
      emit8(0x6B);emit8(0xC0|((dst->reg&7)<<3)|(dst->reg&7));emit8((uint8_t)src->imm);
    }else{emit8(0x69);emit8(0xC0|((dst->reg&7)<<3)|(dst->reg&7));emit32((uint32_t)src->imm);}
  }else die("imul: unsupported form");}

/* ───────── directive parser ───────── */
static void switch_section(const char*name,uint64_t flags,uint32_t type,int bss){
  uint32_t i=sec_get(name,flags,type,bss);cur_sec=i;}

static void parse_directive(const char*dir){
  if(!strcmp(dir,".text")){switch_section(".text",SHF_ALLOC|SHF_EXECINSTR,SHT_PROGBITS,0);return;}
  if(!strcmp(dir,".data")){switch_section(".data",SHF_ALLOC|SHF_WRITE,SHT_PROGBITS,0);return;}
  if(!strcmp(dir,".bss")){switch_section(".bss",SHF_ALLOC|SHF_WRITE,SHT_NOBITS,1);return;}
  if(!strcmp(dir,".rodata")){switch_section(".rodata",SHF_ALLOC,SHT_PROGBITS,0);return;}
  if(!strcmp(dir,".section")){
    char name[64]="";uint64_t flags=SHF_ALLOC;uint32_t type=SHT_PROGBITS;
    if(tok.kind==T_IDENT){strncpy(name,tok.s,63);lex_next();}
    if(tok.kind==T_COMMA){lex_next();
      if(tok.kind==T_STR){
        for(char*p=tok.s;*p;p++){
          if(*p=='x')flags|=SHF_EXECINSTR;
          else if(*p=='w')flags|=SHF_WRITE;
          else if(*p=='a')flags|=SHF_ALLOC;}
        lex_next();}
      if(tok.kind==T_COMMA){lex_next();
        if(tok.kind==T_IDENT){
          if(strstr(tok.s,"nobits"))type=SHT_NOBITS;lex_next();}}}
    int is_bss=(type==SHT_NOBITS);
    if(!strncmp(name,".note",5)||!strncmp(name,".comment",8)){
      /* skip metadata content until next section */}
    switch_section(name,flags,type,is_bss);return;}
  if(!strcmp(dir,".globl")||!strcmp(dir,".global")){
    if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);syms[si].bind=STB_GLOBAL;lex_next();}return;}
  if(!strcmp(dir,".extern")||!strcmp(dir,".extrn")){
    if(tok.kind==T_IDENT){sym_get(tok.s);lex_next();}return;}
  if(!strcmp(dir,".weak")){
    if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);syms[si].bind=STB_WEAK;lex_next();}return;}
  if(!strcmp(dir,".type")){
    if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);lex_next();
      if(tok.kind==T_COMMA)lex_next();
      if(tok.kind==T_IDENT){
        if(strstr(tok.s,"function")||strstr(tok.s,"STT_FUNC"))syms[si].type=STT_FUNC;
        else if(strstr(tok.s,"object"))syms[si].type=STT_OBJECT;
        lex_next();}}return;}
  if(!strcmp(dir,".set")||!strcmp(dir,".equ")){
    if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);lex_next();
      if(tok.kind==T_COMMA)lex_next();
      if(tok.kind==T_INT){syms[si].value=(uint64_t)tok.i;syms[si].defined=1;
        syms[si].secidx=SHN_ABS;lex_next();}}return;}
  if(!strcmp(dir,".p2align")){
    if(tok.kind==T_INT){uint32_t a=(uint32_t)(1<<tok.i);lex_next();
      bb_align(&cur_section()->data,a);
      if(a>cur_section()->align)cur_section()->align=a;}return;}
  if(!strcmp(dir,".align")||!strcmp(dir,".balign")){
    if(tok.kind==T_INT){uint32_t a=(uint32_t)tok.i;lex_next();
      bb_align(&cur_section()->data,a);
      if(a>cur_section()->align)cur_section()->align=a;}return;}
  if(!strcmp(dir,".byte")){
    do{if(tok.kind==T_COMMA)lex_next();
       if(tok.kind==T_INT){emit8((uint8_t)tok.i);lex_next();}
       else if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);
         add_reloc(cur_off(),si,R_X86_64_8,0);emit8(0);lex_next();}
    }while(tok.kind==T_COMMA);return;}
  if(!strcmp(dir,".word")||!strcmp(dir,".short")||!strcmp(dir,".2byte")){
    do{if(tok.kind==T_COMMA)lex_next();
       if(tok.kind==T_INT){emit16((uint16_t)tok.i);lex_next();}
    }while(tok.kind==T_COMMA);return;}
  if(!strcmp(dir,".long")||!strcmp(dir,".int")||!strcmp(dir,".4byte")){
    do{if(tok.kind==T_COMMA)lex_next();
       if(tok.kind==T_INT){emit32((uint32_t)tok.i);lex_next();}
       else if(tok.kind==T_IDENT){
         char la[128];strncpy(la,tok.s,127);resolve_local_ref(la);lex_next();
         if(tok.kind==T_MINUS){lex_next();
           /* label difference: consume and emit 0 placeholder */
           if(tok.kind==T_IDENT)lex_next();emit32(0);
         }else{uint32_t si=sym_get(la);
           add_reloc(cur_off(),si,R_X86_64_32,0);emit32(0);}}
    }while(tok.kind==T_COMMA);return;}
  if(!strcmp(dir,".quad")||!strcmp(dir,".8byte")){
    do{if(tok.kind==T_COMMA)lex_next();
       if(tok.kind==T_INT){emit64((uint64_t)tok.i);lex_next();}
       else if(tok.kind==T_IDENT){
         char la[128];strncpy(la,tok.s,127);resolve_local_ref(la);lex_next();
         uint32_t si=sym_get(la);
         add_reloc(cur_off(),si,R_X86_64_64,0);emit64(0);}
    }while(tok.kind==T_COMMA);return;}
  if(!strcmp(dir,".ascii")){
    if(tok.kind==T_STR){bb_write(&cur_section()->data,(uint8_t*)tok.s,strlen(tok.s));lex_next();}return;}
  if(!strcmp(dir,".asciz")||!strcmp(dir,".string")){
    if(tok.kind==T_STR){bb_write(&cur_section()->data,(uint8_t*)tok.s,strlen(tok.s)+1);lex_next();}return;}
  if(!strcmp(dir,".space")||!strcmp(dir,".zero")||!strcmp(dir,".skip")){
    if(tok.kind==T_INT){uint64_t n=(uint64_t)tok.i;lex_next();
      for(uint64_t i=0;i<n;i++)emit8(0);}return;}
  if(!strcmp(dir,".comm")){
    if(tok.kind==T_IDENT){uint32_t si=sym_get(tok.s);lex_next();
      int64_t sz=0;if(tok.kind==T_COMMA){lex_next();sz=tok.i;lex_next();}
      if(tok.kind==T_COMMA){lex_next();lex_next();}
      syms[si].value=(uint64_t)sz;syms[si].defined=1;
      syms[si].secidx=SHN_COMMON;syms[si].bind=STB_GLOBAL;}return;}
  /* silently ignore: .cfi_* .file .loc .ident .size etc. */
  while(!at_eol())lex_next();}

/* ───────── instruction parser ───────── */
static void parse_instruction(const char*mne){
  /* AT&T size suffix: only strip if mnemonic is not in the no-strip list */
  static const char*no_strip[]={
    "syscall","retf","pushf","popf","pusha","popa","lahf","sahf",
    "call","mul","div","sub","shl","shr","sar","rol","ror","rcl","rcr",
    "imul","idiv","neg","not","inc","dec","nop","hlt","ret","leave",
    "cdq","cqo","xchg","lea","test","cmp","and","xor","or","add",
    "jnl","jnle","jnge","jge","jle","jne","jbe","jnbe","jnae",
    "jnb","jnp","jnz","jno","jns","jnc","jng","jnal",
    "jl","jg","je","jz","js","jp","jo","ja","jb","jae","jmp",
    "endbr64","endbr32","ud2","pause","lfence","mfence","sfence",
    "movsb","movsw","movsd","stosb","stosw","stosd","stosq",
    "scasb","lodsb","cmpsb","rep","repe","repz","repne","repnz",NULL};
  int sz=8;char mn[64];strncpy(mn,mne,63);mn[63]=0;
  size_t mlen=strlen(mn);
  int skip=0;
  for(int i=0;no_strip[i];i++)if(!strcasecmp(mn,no_strip[i])){skip=1;break;}
  if(!skip&&mlen>2){
    char last=mn[mlen-1];
    if(last=='q'){sz=8;mn[mlen-1]=0;}
    else if(last=='l'){sz=4;mn[mlen-1]=0;}
    else if(last=='w'){sz=2;mn[mlen-1]=0;}
    else if(last=='b'){sz=1;mn[mlen-1]=0;}}

  Operand ops[3];int nops=0;
  while(!at_eol()&&nops<3){
    if(tok.kind==T_COMMA)lex_next();
    ops[nops++]=parse_operand();
    if(tok.kind==T_COMMA)lex_next();}

  /* AT&T operand reversal */
  int is_att=0;
  for(int i=0;i<nops;i++)if(ops[i].att){is_att=1;break;}
  if(is_att&&nops==2){Operand tmp=ops[0];ops[0]=ops[1];ops[1]=tmp;}

  /* zero-operand */
  if(!strcasecmp(mn,"nop")){emit8(0x90);return;}
  if(!strcasecmp(mn,"ret")){emit8(0xC3);return;}
  if(!strcasecmp(mn,"leave")){emit8(0xC9);return;}
  if(!strcasecmp(mn,"hlt")){emit8(0xF4);return;}
  if(!strcasecmp(mn,"syscall")){emit8(0x0F);emit8(0x05);return;}
  if(!strcasecmp(mn,"cdq")){emit8(0x99);return;}
  if(!strcasecmp(mn,"cqo")){emit8(0x48);emit8(0x99);return;}
  if(!strcasecmp(mn,"endbr64")){emit8(0xF3);emit8(0x0F);emit8(0x1E);emit8(0xFA);return;}
  if(!strcasecmp(mn,"endbr32")){emit8(0xF3);emit8(0x0F);emit8(0x1E);emit8(0xFB);return;}
  if(!strcasecmp(mn,"ud2")){emit8(0x0F);emit8(0x0B);return;}
  if(!strcasecmp(mn,"pause")){emit8(0xF3);emit8(0x90);return;}
  if(!strcasecmp(mn,"lfence")){emit8(0x0F);emit8(0xAE);emit8(0xE8);return;}
  if(!strcasecmp(mn,"mfence")){emit8(0x0F);emit8(0xAE);emit8(0xF0);return;}
  if(!strcasecmp(mn,"sfence")){emit8(0x0F);emit8(0xAE);emit8(0xF8);return;}
  if(!strcasecmp(mn,"stosb")){emit8(0xAA);return;}
  if(!strcasecmp(mn,"stosw")){emit8(0x66);emit8(0xAB);return;}
  if(!strcasecmp(mn,"stosd")){emit8(0xAB);return;}
  if(!strcasecmp(mn,"stosq")){emit8(0x48);emit8(0xAB);return;}
  if(!strcasecmp(mn,"movsb")){emit8(0xA4);return;}
  if(!strcasecmp(mn,"movsw")){emit8(0x66);emit8(0xA5);return;}
  if(!strcasecmp(mn,"movsd")){emit8(0xA5);return;}
  if(!strcasecmp(mn,"cmpsb")){emit8(0xA6);return;}
  if(!strcasecmp(mn,"scasb")){emit8(0xAE);return;}
  if(!strcasecmp(mn,"lodsb")){emit8(0xAC);return;}
  if(!strcasecmp(mn,"rep")||!strcasecmp(mn,"repe")||!strcasecmp(mn,"repz")){emit8(0xF3);return;}
  if(!strcasecmp(mn,"repne")||!strcasecmp(mn,"repnz")){emit8(0xF2);return;}

  /* one-operand */
  if(!strcasecmp(mn,"imul")&&nops==1){enc_unary(5,&ops[0],sz);return;}
  if(!strcasecmp(mn,"push")){enc_push(&ops[0]);return;}
  if(!strcasecmp(mn,"pop")){enc_pop(&ops[0]);return;}
  if(!strcasecmp(mn,"call")){enc_call(&ops[0]);return;}
  if(!strcasecmp(mn,"jmp")){enc_jmp(&ops[0]);return;}
  if(!strcasecmp(mn,"neg")){enc_unary(3,&ops[0],sz);return;}
  if(!strcasecmp(mn,"not")){enc_unary(2,&ops[0],sz);return;}
  if(!strcasecmp(mn,"inc")){
    int W=(sz==8)?1:0;
    if(ops[0].kind==OP_REG){emit_rex(W,0,0xff,ops[0].reg);
      emit8(W?0xFF:0xFE);emit8(0xC0|(0<<3)|(ops[0].reg&7));}
    else enc_unary(0,&ops[0],sz);return;}
  if(!strcasecmp(mn,"dec")){
    int W=(sz==8)?1:0;
    if(ops[0].kind==OP_REG){emit_rex(W,0,0xff,ops[0].reg);
      emit8(W?0xFF:0xFE);emit8(0xC0|(1<<3)|(ops[0].reg&7));}
    else enc_unary(1,&ops[0],sz);return;}
  if(!strcasecmp(mn,"idiv")){enc_unary(7,&ops[0],sz);return;}
  if(!strcasecmp(mn,"div")){enc_unary(6,&ops[0],sz);return;}
  if(!strcasecmp(mn,"mul")){enc_unary(4,&ops[0],sz);return;}

  /* conditional jumps */
  if(enc_jcc(mn,&ops[0]))return;

  /* two-operand */
  if(nops<2){warn("expected 2 operands for %s",mn);return;}
  if(ops[0].size)sz=ops[0].size;
  else if(ops[0].kind==OP_REG&&ops[0].regsize)sz=ops[0].regsize;
  else if(ops[1].size)sz=ops[1].size;
  else if(ops[1].kind==OP_REG&&ops[1].regsize)sz=ops[1].regsize;

  if(!strcasecmp(mn,"mov")||!strcasecmp(mn,"movabs")){enc_mov(&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"lea")){enc_lea(&ops[0],&ops[1]);return;}
  if(!strcasecmp(mn,"xchg")){
    int W=(sz==8)?1:0;
    if(ops[0].kind==OP_REG&&ops[1].kind==OP_REG){
      emit_rex(W,ops[0].reg,0xff,ops[1].reg);
      emit8(W?0x87:0x86);emit8(0xC0|((ops[0].reg&7)<<3)|(ops[1].reg&7));}
    return;}
  if(!strcasecmp(mn,"imul")){
    if(nops==3)enc_imul2(&ops[0],&ops[2],sz);
    else enc_imul2(&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"shl")||!strcasecmp(mn,"sal")){enc_shift(4,&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"shr")){enc_shift(5,&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"sar")){enc_shift(7,&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"rol")){enc_shift(0,&ops[0],&ops[1],sz);return;}
  if(!strcasecmp(mn,"ror")){enc_shift(1,&ops[0],&ops[1],sz);return;}

  /* movsxd / movslq / movsl */
  if(!strcasecmp(mn,"movsxd")||!strcasecmp(mn,"movslq")||!strcasecmp(mn,"movsl")){
    if(nops>=2&&ops[0].kind==OP_REG){
      uint8_t src_r=(ops[1].kind==OP_REG)?ops[1].reg:
                    (ops[1].base!=0xff?ops[1].base:0);
      emit8(rex(1,ops[0].reg>7?1:0,0,src_r>7?1:0));emit8(0x63);
      if(ops[1].kind==OP_REG)emit8(0xC0|((ops[0].reg&7)<<3)|(ops[1].reg&7));
      else emit_modrm_sib(ops[0].reg,&ops[1]);}return;}
  /* movzx family */
  if(!strcasecmp(mn,"movzx")||!strcasecmp(mn,"movzbq")||!strcasecmp(mn,"movzwq")||
     !strcasecmp(mn,"movzbl")||!strcasecmp(mn,"movzwl")){
    if(nops>=2&&ops[0].kind==OP_REG){
      int src_sz=(!strcasecmp(mn,"movzwq")||!strcasecmp(mn,"movzwl"))?2:1;
      int dst_sz=(!strcasecmp(mn,"movzbl")||!strcasecmp(mn,"movzwl"))?4:8;
      int W=(dst_sz==8)?1:0;
      emit_rex(W,ops[0].reg,0,ops[1].kind==OP_REG?ops[1].reg:0);
      emit8(0x0F);emit8(src_sz==1?0xB6:0xB7);
      if(ops[1].kind==OP_REG)emit8(0xC0|((ops[0].reg&7)<<3)|(ops[1].reg&7));
      else emit_modrm_sib(ops[0].reg,&ops[1]);}return;}
  /* movsx family */
  if(!strcasecmp(mn,"movsx")||!strcasecmp(mn,"movsbq")||!strcasecmp(mn,"movswq")||
     !strcasecmp(mn,"movsbl")||!strcasecmp(mn,"movswl")){
    if(nops>=2&&ops[0].kind==OP_REG){
      int src_sz=(!strcasecmp(mn,"movswq")||!strcasecmp(mn,"movswl"))?2:1;
      int dst_sz=(!strcasecmp(mn,"movsbl")||!strcasecmp(mn,"movswl"))?4:8;
      int W=(dst_sz==8)?1:0;
      emit_rex(W,ops[0].reg,0,ops[1].kind==OP_REG?ops[1].reg:0);
      emit8(0x0F);emit8(src_sz==1?0xBE:0xBF);
      if(ops[1].kind==OP_REG)emit8(0xC0|((ops[0].reg&7)<<3)|(ops[1].reg&7));
      else emit_modrm_sib(ops[0].reg,&ops[1]);}return;}

  /* setcc */
  {static const struct{const char*n;uint8_t o;}setcc[]={
    {"sete",0x94},{"setz",0x94},{"setne",0x95},{"setnz",0x95},
    {"setl",0x9C},{"setge",0x9D},{"setle",0x9E},{"setg",0x9F},
    {"setb",0x92},{"setae",0x93},{"setbe",0x96},{"seta",0x97},
    {"sets",0x98},{"setns",0x99},{NULL,0}};
   for(int i=0;setcc[i].n;i++)if(!strcasecmp(mn,setcc[i].n)){
     if(nops>=1&&ops[0].kind==OP_REG){
       if(ops[0].reg>7)emit8(0x41);
       emit8(0x0F);emit8(setcc[i].o);emit8(0xC0|(ops[0].reg&7));}return;}}
  /* cmovcc */
  {static const struct{const char*n;uint8_t o;}cmov[]={
    {"cmove",0x44},{"cmovz",0x44},{"cmovne",0x45},{"cmovnz",0x45},
    {"cmovl",0x4C},{"cmovge",0x4D},{"cmovle",0x4E},{"cmovg",0x4F},
    {"cmovb",0x42},{"cmovae",0x43},{"cmovbe",0x46},{"cmova",0x47},
    {"cmovs",0x48},{"cmovns",0x49},{NULL,0}};
   for(int i=0;cmov[i].n;i++)if(!strcasecmp(mn,cmov[i].n)){
     if(nops>=2&&ops[0].kind==OP_REG){
       int W=(sz==8)?1:0;
       emit_rex(W,ops[0].reg,0,ops[1].kind==OP_REG?ops[1].reg:0);
       emit8(0x0F);emit8(cmov[i].o);
       if(ops[1].kind==OP_REG)emit8(0xC0|((ops[0].reg&7)<<3)|(ops[1].reg&7));
       else emit_modrm_sib(ops[0].reg,&ops[1]);}return;}}

  /* ALU: add or adc sbb and sub xor cmp test */
  {static const struct{const char*mn;uint8_t slash,opc_rr;}alus[]={
    {"add",0,0x01},{"or",1,0x09},{"adc",2,0x11},{"sbb",3,0x19},
    {"and",4,0x21},{"sub",5,0x29},{"xor",6,0x31},{"cmp",7,0x39},
    {"test",0,0x85},{NULL,0,0}};
   for(int i=0;alus[i].mn;i++){
     if(!strcasecmp(mn,alus[i].mn)){
       if(!strcasecmp(mn,"test")){
         int W=(sz==8)?1:0;
         if(ops[1].kind==OP_IMM){enc_alu_imm(0,&ops[0],ops[1].imm,sz);return;}
         if(ops[0].kind==OP_REG&&ops[1].kind==OP_REG){
           emit_rex(W,ops[1].reg,0xff,ops[0].reg);
           emit8(W?0x85:0x84);emit8(0xC0|((ops[1].reg&7)<<3)|(ops[0].reg&7));return;}
         return;}
       if(ops[1].kind==OP_IMM){enc_alu_imm(alus[i].slash,&ops[0],ops[1].imm,sz);return;}
       enc_alu(alus[i].opc_rr,&ops[0],&ops[1],sz);return;}}}

  warn("unknown instruction: %s",mn);}

/* ───────── branch relaxation ───────── */
/* We sort relocs by descending offset so we process later instructions first.
   This ensures that shrinking a later instruction doesn't invalidate the
   already-committed short form of an earlier instruction. */
static int reloc_cmp_desc(const void*a,const void*b){
  const Reloc*ra=a,*rb=b;
  return (rb->offset>ra->offset)?1:(rb->offset<ra->offset)?-1:0;}

static void relax_section(uint32_t sec_idx){
  Section*s=&secs[sec_idx];
  if(!s->relocs.n||s->is_bss)return;
  int changed=1;
  while(changed){
    changed=0;
    /* sort descending so we process from end of section backwards */
    qsort(s->relocs.d,s->relocs.n,sizeof(Reloc),reloc_cmp_desc);
    for(uint32_t ri=0;ri<s->relocs.n;ri++){
      Reloc*r=&s->relocs.d[ri];
      if(r->type!=R_X86_64_PC32)continue;
      Sym*sym=&syms[r->symidx];
      if(!sym->defined||sym->secidx!=(sec_idx+1))continue;
      uint64_t roff=r->offset;uint8_t*p=s->data.d;
      int is_jcc6=0,is_jmp5=0;
      if(roff>=2&&p[roff-2]==0x0F&&(p[roff-1]&0xF0)==0x80)is_jcc6=1;
      else if(roff>=1&&p[roff-1]==0xE9)is_jmp5=1;
      if(!is_jcc6&&!is_jmp5)continue;
      uint64_t istart=roff-(is_jcc6?2:1);
      uint64_t ilen=is_jcc6?6:5;
      int64_t disp=(int64_t)sym->value-(int64_t)(istart+2);
      if(disp<-128||disp>127)continue;
      /* Only relax backward branches (target is before or at instruction). 
         Forward branch relaxation is complex and handled by linker. */
      if(sym->value >= istart+ilen) continue;
      uint8_t short_opc=is_jcc6?(uint8_t)(p[roff-1]-0x10):0xEB;
      uint8_t new2[2]={short_opc,(uint8_t)(int8_t)disp};
      uint32_t shrink=(uint32_t)(ilen-2);
      memmove(p+istart+2,p+istart+ilen,s->data.n-(uint32_t)istart-shrink-2);
      memcpy(p+istart,new2,2);s->data.n-=shrink;
      /* adjust symbols and relocs that come AFTER this instruction */
      for(uint32_t si=0;si<nsyms;si++)
        if(syms[si].defined&&syms[si].secidx==(sec_idx+1)&&syms[si].value>istart)
          syms[si].value-=shrink;
      for(uint32_t rj=0;rj<s->relocs.n;rj++)
        if(s->relocs.d[rj].offset>istart)s->relocs.d[rj].offset-=shrink;
      s->relocs.d[ri]=s->relocs.d[--s->relocs.n];
      ri--;changed=1;}}}
static void relax_all(void){for(uint32_t i=0;i<nsecs;i++)relax_section(i);}

/* ───────── main assembler pass ───────── */
static void assemble(char*source){
  sec_get(".text",SHF_ALLOC|SHF_EXECINSTR,SHT_PROGBITS,0);
  sec_get(".data",SHF_ALLOC|SHF_WRITE,SHT_PROGBITS,0);
  sec_get(".bss",SHF_ALLOC|SHF_WRITE,SHT_NOBITS,1);
  cur_sec=0;
  memset(local_label_last,0xff,sizeof local_label_last);
  local_label_seq=0;
  src_ptr=source;g_line=1;lex_next();
  while(tok.kind!=T_EOF){
    skip_eol();if(tok.kind==T_EOF)break;
    if(tok.kind!=T_IDENT){lex_next();continue;}
    char word[256];strncpy(word,tok.s,255);lex_next();
    /* label */
    if(tok.kind==T_COLON){lex_next();
      char lname[128];
      if(word[0]>='0'&&word[0]<='9'&&word[1]=='\0'){
        int d=word[0]-'0';
        snprintf(lname,127,".Lloc_%d_%u",d,local_label_seq);
        local_label_last[d]=local_label_seq++;
        strncpy(word,lname,255);}
      uint32_t si=sym_get(word);
      syms[si].value=cur_section()->data.n;
      syms[si].secidx=cur_sec+1;syms[si].defined=1;
      continue;}
    if(word[0]=='.'){parse_directive(word);continue;}
    parse_instruction(word);}}

/* ───────── string table ───────── */
typedef struct{uint8_t*d;uint32_t n,cap;}StrTab;
static uint32_t strtab_add(StrTab*st,const char*s){
  uint32_t off=st->n;size_t len=strlen(s)+1;
  while(st->n+len>st->cap){st->cap=st->cap?2*st->cap:256;
    st->d=realloc(st->d,st->cap);}
  memcpy(st->d+st->n,s,len);st->n+=len;return off;}
static void strtab_init(StrTab*st){st->d=NULL;st->n=st->cap=0;strtab_add(st,"");}

/* ───────── ELF object writer ───────── */
static void write_object(FILE*f){
  StrTab shstr;strtab_init(&shstr);
  uint32_t elf_sec_idx[MAX_SECS],elf_rela_idx[MAX_SECS];
  memset(elf_sec_idx,0,sizeof elf_sec_idx);
  memset(elf_rela_idx,0,sizeof elf_rela_idx);
  uint32_t shnum=1;
  for(uint32_t i=0;i<nsecs;i++)elf_sec_idx[i]=shnum++;
  for(uint32_t i=0;i<nsecs;i++)if(secs[i].relocs.n>0)elf_rela_idx[i]=shnum++;
  uint32_t symtab_idx=shnum++,strtab_idx=shnum++,shstrndx=shnum++;

  StrTab symstr;strtab_init(&symstr);
  uint32_t sym_elf_idx[MAX_SYMS];
  memset(sym_elf_idx,0xff,sizeof sym_elf_idx);
  uint32_t nsym_elf=1,first_global=1;
  for(uint32_t i=0;i<nsyms;i++)
    if(syms[i].bind==STB_LOCAL)sym_elf_idx[i]=nsym_elf++;
  first_global=nsym_elf;
  for(uint32_t i=0;i<nsyms;i++)
    if(syms[i].bind!=STB_LOCAL)sym_elf_idx[i]=nsym_elf++;

  Elf64_Sym*esyms=calloc(nsym_elf,sizeof(Elf64_Sym));
  for(uint32_t i=0;i<nsyms;i++){
    Elf64_Sym*es=&esyms[sym_elf_idx[i]];
    es->st_name=strtab_add(&symstr,syms[i].name);
    es->st_info=ELF_ST_INFO(syms[i].bind,syms[i].type);
    es->st_other=STV_DEFAULT;
    if(!syms[i].defined){es->st_shndx=SHN_UNDEF;es->st_value=0;}
    else if(syms[i].secidx==SHN_ABS){es->st_shndx=SHN_ABS;es->st_value=syms[i].value;}
    else if(syms[i].secidx==SHN_COMMON){es->st_shndx=SHN_COMMON;es->st_value=syms[i].value;}
    else{uint32_t si=syms[i].secidx-1;
      es->st_shndx=(uint16_t)elf_sec_idx[si];es->st_value=syms[i].value;}}

  uint32_t shstr_off[MAX_SECS*2+8],shstr_rela[MAX_SECS];
  char rela_name[128];
  for(uint32_t i=0;i<nsecs;i++){
    shstr_off[i]=strtab_add(&shstr,secs[i].name);
    if(secs[i].relocs.n>0){
      snprintf(rela_name,127,".rela%s",secs[i].name);
      shstr_rela[i]=strtab_add(&shstr,rela_name);}}
  uint32_t shstr_symtab=strtab_add(&shstr,".symtab");
  uint32_t shstr_strtab=strtab_add(&shstr,".strtab");
  uint32_t shstr_shstrtab=strtab_add(&shstr,".shstrtab");

  uint64_t off=sizeof(Elf64_Ehdr);
  uint64_t sec_foff[MAX_SECS];
  for(uint32_t i=0;i<nsecs;i++){
    if(secs[i].is_bss){sec_foff[i]=0;continue;}
    if(secs[i].align>1)off=(off+secs[i].align-1)&~(uint64_t)(secs[i].align-1);
    sec_foff[i]=off;off+=secs[i].data.n;}
  uint64_t rela_foff[MAX_SECS];memset(rela_foff,0,sizeof rela_foff);
  for(uint32_t i=0;i<nsecs;i++){
    if(!secs[i].relocs.n)continue;
    off=(off+7)&~7ULL;rela_foff[i]=off;off+=secs[i].relocs.n*sizeof(Elf64_Rela);}
  off=(off+7)&~7ULL;uint64_t symtab_foff=off;off+=nsym_elf*sizeof(Elf64_Sym);
  uint64_t strtab_foff=off;off+=symstr.n;
  uint64_t shstrtab_foff=off;off+=shstr.n;
  off=(off+7)&~7ULL;uint64_t shdr_off=off;

  Elf64_Ehdr eh;memset(&eh,0,sizeof eh);
  eh.e_ident[0]=0x7f;eh.e_ident[1]='E';eh.e_ident[2]='L';eh.e_ident[3]='F';
  eh.e_ident[4]=2;eh.e_ident[5]=1;eh.e_ident[6]=1;
  eh.e_type=ET_REL;eh.e_machine=EM_X86_64;eh.e_version=1;
  eh.e_shoff=shdr_off;eh.e_ehsize=sizeof(Elf64_Ehdr);
  eh.e_shentsize=sizeof(Elf64_Shdr);eh.e_shnum=shnum;eh.e_shstrndx=shstrndx;
  fwrite(&eh,sizeof eh,1,f);

  for(uint32_t i=0;i<nsecs;i++){
    if(secs[i].is_bss||!secs[i].data.n)continue;
    fseek(f,(long)sec_foff[i],SEEK_SET);
    fwrite(secs[i].data.d,1,secs[i].data.n,f);}
  for(uint32_t i=0;i<nsecs;i++){
    if(!secs[i].relocs.n)continue;
    fseek(f,(long)rela_foff[i],SEEK_SET);
    for(uint32_t j=0;j<secs[i].relocs.n;j++){
      Reloc*r=&secs[i].relocs.d[j];
      Elf64_Rela er;er.r_offset=r->offset;
      er.r_info=ELF_R_INFO(sym_elf_idx[r->symidx],r->type);
      er.r_addend=r->addend;fwrite(&er,sizeof er,1,f);}}
  fseek(f,(long)symtab_foff,SEEK_SET);fwrite(esyms,sizeof(Elf64_Sym),nsym_elf,f);
  fseek(f,(long)strtab_foff,SEEK_SET);fwrite(symstr.d,1,symstr.n,f);
  fseek(f,(long)shstrtab_foff,SEEK_SET);fwrite(shstr.d,1,shstr.n,f);

  fseek(f,(long)shdr_off,SEEK_SET);
  Elf64_Shdr sh;memset(&sh,0,sizeof sh);fwrite(&sh,sizeof sh,1,f);
  for(uint32_t i=0;i<nsecs;i++){
    memset(&sh,0,sizeof sh);sh.sh_name=shstr_off[i];
    sh.sh_type=secs[i].type;sh.sh_flags=secs[i].flags;
    sh.sh_offset=secs[i].is_bss?0:sec_foff[i];
    sh.sh_size=secs[i].data.n;
    sh.sh_addralign=secs[i].align?secs[i].align:1;fwrite(&sh,sizeof sh,1,f);}
  for(uint32_t i=0;i<nsecs;i++){
    if(!secs[i].relocs.n)continue;
    memset(&sh,0,sizeof sh);sh.sh_name=shstr_rela[i];
    sh.sh_type=SHT_RELA;sh.sh_offset=rela_foff[i];
    sh.sh_size=secs[i].relocs.n*sizeof(Elf64_Rela);
    sh.sh_link=symtab_idx;sh.sh_info=elf_sec_idx[i];
    sh.sh_addralign=8;sh.sh_entsize=sizeof(Elf64_Rela);fwrite(&sh,sizeof sh,1,f);}
  memset(&sh,0,sizeof sh);sh.sh_name=shstr_symtab;sh.sh_type=SHT_SYMTAB;
  sh.sh_offset=symtab_foff;sh.sh_size=nsym_elf*sizeof(Elf64_Sym);
  sh.sh_link=strtab_idx;sh.sh_info=first_global;
  sh.sh_addralign=8;sh.sh_entsize=sizeof(Elf64_Sym);fwrite(&sh,sizeof sh,1,f);
  memset(&sh,0,sizeof sh);sh.sh_name=shstr_strtab;sh.sh_type=SHT_STRTAB;
  sh.sh_offset=strtab_foff;sh.sh_size=symstr.n;sh.sh_addralign=1;fwrite(&sh,sizeof sh,1,f);
  memset(&sh,0,sizeof sh);sh.sh_name=shstr_shstrtab;sh.sh_type=SHT_STRTAB;
  sh.sh_offset=shstrtab_foff;sh.sh_size=shstr.n;sh.sh_addralign=1;fwrite(&sh,sizeof sh,1,f);
  free(esyms);}

/* ───────── linker ───────── */
typedef struct{
  Elf64_Shdr*shdrs;uint32_t nshdr;
  uint8_t*data;size_t size;const char*shstr;uint64_t*vma;
}ObjFile;
#define MAX_OBJS 64
#define MAX_LSYMS 65536
typedef struct{char name[128];uint64_t vma;int defined;uint8_t bind,type;}LSym;
static ObjFile lobjs[MAX_OBJS];static uint32_t nlobjs=0;
typedef struct{char name[64];uint8_t*data;uint64_t size,vma,flags;uint32_t type,align;}OutSec;
#define MAX_OUT_SECS 16
static OutSec outsecs[MAX_OUT_SECS];static uint32_t noutsecs=0;
static LSym lsyms[MAX_LSYMS];static uint32_t nlsyms=0;

static uint32_t lsym_find(const char*n){
  for(uint32_t i=0;i<nlsyms;i++)if(!strcmp(lsyms[i].name,n))return i;
  return(uint32_t)-1;}
static uint32_t lsym_get(const char*n){
  uint32_t i=lsym_find(n);if(i!=(uint32_t)-1)return i;
  if(nlsyms>=MAX_LSYMS)die("too many linker symbols");
  i=nlsyms++;strncpy(lsyms[i].name,n,127);
  lsyms[i].vma=0;lsyms[i].defined=0;lsyms[i].bind=STB_GLOBAL;return i;}
static uint32_t outsec_find(const char*n){
  for(uint32_t i=0;i<noutsecs;i++)if(!strcmp(outsecs[i].name,n))return i;
  return(uint32_t)-1;}
static uint32_t outsec_get(const char*n,uint64_t fl,uint32_t ty){
  uint32_t i=outsec_find(n);if(i!=(uint32_t)-1)return i;
  if(noutsecs>=MAX_OUT_SECS)die("too many output sections");
  i=noutsecs++;memset(&outsecs[i],0,sizeof outsecs[i]);
  strncpy(outsecs[i].name,n,63);outsecs[i].flags=fl;outsecs[i].type=ty;outsecs[i].align=16;
  return i;}

static ObjFile*load_obj(const char*path){
  if(nlobjs>=MAX_OBJS)die("too many object files");
  FILE*f=fopen(path,"rb");if(!f)die("cannot open %s",path);
  fseek(f,0,SEEK_END);size_t sz=(size_t)ftell(f);rewind(f);
  uint8_t*d=malloc(sz);if(fread(d,1,sz,f)!=sz){}fclose(f);
  ObjFile*o=&lobjs[nlobjs++];o->data=d;o->size=sz;
  Elf64_Ehdr*eh=(Elf64_Ehdr*)d;
  if(memcmp(eh->e_ident,"\x7f""ELF",4))die("%s: not ELF",path);
  if(eh->e_type!=ET_REL)die("%s: not relocatable",path);
  o->shdrs=(Elf64_Shdr*)(d+eh->e_shoff);o->nshdr=eh->e_shnum;
  o->shstr=(char*)(d+o->shdrs[eh->e_shstrndx].sh_offset);
  o->vma=calloc(o->nshdr,sizeof(uint64_t));return o;}

static void link_objs(const char*outpath,const char*entry_sym,int n_in,char**inputs){
  for(int i=0;i<n_in;i++)load_obj(inputs[i]);

  /* pass 1: assign input sections to output sections, compute sizes */
  for(uint32_t oi=0;oi<nlobjs;oi++){
    ObjFile*o=&lobjs[oi];
    for(uint32_t si=1;si<o->nshdr;si++){
      Elf64_Shdr*sh=&o->shdrs[si];
      if(sh->sh_type==SHT_NULL||sh->sh_type==SHT_STRTAB||
         sh->sh_type==SHT_SYMTAB||sh->sh_type==SHT_RELA)continue;
      const char*name=o->shstr+sh->sh_name;
      const char*grp=name;
      if(!strncmp(name,".text",5))grp=".text";
      else if(!strncmp(name,".rodata",7))grp=".rodata";
      else if(!strncmp(name,".data",5))grp=".data";
      else if(!strncmp(name,".bss",4))grp=".bss";
      else continue;
      uint32_t osi=outsec_get(grp,sh->sh_flags,sh->sh_type);
      uint64_t al=sh->sh_addralign?sh->sh_addralign:1;
      if(al>outsecs[osi].align)outsecs[osi].align=al;
      outsecs[osi].size=(outsecs[osi].size+al-1)&~(al-1);
      o->vma[si]=outsecs[osi].size;
      outsecs[osi].size+=sh->sh_size;}}

  /* assign VMAs */
  uint64_t page=0x1000;
  uint64_t load_base=0x400000;
  uint64_t hdr_size=sizeof(Elf64_Ehdr)+sizeof(Elf64_Phdr)*2;
  uint64_t vma=load_base+hdr_size,file_off=hdr_size;

  uint32_t text_idx=outsec_find(".text");
  uint32_t rodata_idx=outsec_find(".rodata");
  uint32_t data_idx=outsec_find(".data");
  uint32_t bss_idx=outsec_find(".bss");
  uint64_t text_vma=0,text_foff=0,text_size=0;
  uint64_t data_vma=0,data_foff=0,data_size=0,bss_vma=0,bss_size=0;

  if(text_idx!=(uint32_t)-1){
    vma=(vma+outsecs[text_idx].align-1)&~(outsecs[text_idx].align-1);
    file_off=(file_off+outsecs[text_idx].align-1)&~(outsecs[text_idx].align-1);
    text_vma=vma;text_foff=file_off;text_size=outsecs[text_idx].size;
    outsecs[text_idx].vma=text_vma;vma+=text_size;file_off+=text_size;
    if(rodata_idx!=(uint32_t)-1){
      vma=(vma+outsecs[rodata_idx].align-1)&~(outsecs[rodata_idx].align-1);
      file_off=(file_off+outsecs[rodata_idx].align-1)&~(outsecs[rodata_idx].align-1);
      outsecs[rodata_idx].vma=vma;vma+=outsecs[rodata_idx].size;file_off+=outsecs[rodata_idx].size;}}
  vma=(vma+page-1)&~(page-1);file_off=(file_off+page-1)&~(page-1);
  if(data_idx!=(uint32_t)-1){
    data_vma=vma;data_foff=file_off;data_size=outsecs[data_idx].size;
    outsecs[data_idx].vma=data_vma;vma+=data_size;file_off+=data_size;}
  if(bss_idx!=(uint32_t)-1){
    vma=(vma+outsecs[bss_idx].align-1)&~(outsecs[bss_idx].align-1);
    bss_vma=vma;bss_size=outsecs[bss_idx].size;outsecs[bss_idx].vma=bss_vma;vma+=bss_size;}

  /* fix up VMAs */
  for(uint32_t oi=0;oi<nlobjs;oi++){
    ObjFile*o=&lobjs[oi];
    for(uint32_t si=1;si<o->nshdr;si++){
      Elf64_Shdr*sh=&o->shdrs[si];if(sh->sh_type==SHT_NULL)continue;
      const char*name=o->shstr+sh->sh_name;const char*grp=name;
      if(!strncmp(name,".text",5))grp=".text";
      else if(!strncmp(name,".rodata",7))grp=".rodata";
      else if(!strncmp(name,".data",5))grp=".data";
      else if(!strncmp(name,".bss",4))grp=".bss";
      else continue;
      uint32_t osi=outsec_find(grp);if(osi==(uint32_t)-1)continue;
      o->vma[si]+=outsecs[osi].vma;}}

  /* pass 2: collect symbols */
  for(uint32_t oi=0;oi<nlobjs;oi++){
    ObjFile*o=&lobjs[oi];
    Elf64_Shdr*symsh=NULL;const char*symstr=NULL;
    for(uint32_t si=0;si<o->nshdr;si++)
      if(o->shdrs[si].sh_type==SHT_SYMTAB){
        symsh=&o->shdrs[si];
        symstr=(char*)(o->data+o->shdrs[symsh->sh_link].sh_offset);break;}
    if(!symsh)continue;
    uint32_t nsym=(uint32_t)(symsh->sh_size/sizeof(Elf64_Sym));
    Elf64_Sym*esyms=(Elf64_Sym*)(o->data+symsh->sh_offset);
    for(uint32_t si=1;si<nsym;si++){
      Elf64_Sym*es=&esyms[si];const char*name=symstr+es->st_name;
      if(!name||!*name)continue;
      if(ELF_ST_BIND(es->st_info)==STB_LOCAL)continue;
      uint32_t li=lsym_get(name);if(es->st_shndx==SHN_UNDEF)continue;
      if(lsyms[li].defined&&ELF_ST_BIND(es->st_info)!=STB_WEAK)continue;
      lsyms[li].defined=1;lsyms[li].bind=ELF_ST_BIND(es->st_info);
      lsyms[li].type=ELF_ST_TYPE(es->st_info);
      if(es->st_shndx==SHN_ABS){lsyms[li].vma=es->st_value;continue;}
      lsyms[li].vma=o->vma[es->st_shndx]+es->st_value;}}

  /* synthetic symbols */
  {uint32_t i=lsym_get("__bss_start");if(!lsyms[i].defined){lsyms[i].vma=bss_vma;lsyms[i].defined=1;}}
  {uint32_t i=lsym_get("_end");if(!lsyms[i].defined){lsyms[i].vma=bss_vma+bss_size;lsyms[i].defined=1;}}

  /* entry point */
  uint64_t entry_vma=0;
  {const char*try[]={entry_sym?entry_sym:"_start","main",NULL};
   for(int t=0;try[t];t++){uint32_t ei=lsym_find(try[t]);
     if(ei!=(uint32_t)-1&&lsyms[ei].defined){entry_vma=lsyms[ei].vma;break;}}}

  /* allocate output buffers */
  uint8_t*text_buf=calloc(1,text_size+1);
  uint8_t*data_buf=calloc(1,data_size+1);

  /* pass 3: copy section data */
  for(uint32_t oi=0;oi<nlobjs;oi++){
    ObjFile*o=&lobjs[oi];
    for(uint32_t si=1;si<o->nshdr;si++){
      Elf64_Shdr*sh=&o->shdrs[si];
      if(!sh->sh_size||sh->sh_type==SHT_NOBITS)continue;
      const char*name=o->shstr+sh->sh_name;
      uint8_t*dst=NULL;uint64_t base=0;
      if(!strncmp(name,".text",5)||!strncmp(name,".rodata",7)){dst=text_buf;base=text_vma;}
      else if(!strncmp(name,".data",5)){dst=data_buf;base=data_vma;}
      else continue;
      memcpy(dst+o->vma[si]-base,o->data+sh->sh_offset,sh->sh_size);}}

  /* pass 4: apply relocations */
  for(uint32_t oi=0;oi<nlobjs;oi++){
    ObjFile*o=&lobjs[oi];
    Elf64_Shdr*symsh=NULL;const char*symstr=NULL;
    for(uint32_t si=0;si<o->nshdr;si++)
      if(o->shdrs[si].sh_type==SHT_SYMTAB){
        symsh=&o->shdrs[si];
        symstr=(char*)(o->data+o->shdrs[symsh->sh_link].sh_offset);break;}
    if(!symsh)continue;
    Elf64_Sym*esyms=(Elf64_Sym*)(o->data+symsh->sh_offset);
    for(uint32_t si=0;si<o->nshdr;si++){
      Elf64_Shdr*rsh=&o->shdrs[si];
      if(rsh->sh_type!=SHT_RELA)continue;
      uint32_t tsi=(uint32_t)rsh->sh_info;
      if(tsi>=o->nshdr)continue;
      Elf64_Shdr*tsh=&o->shdrs[tsi];
      const char*tname=o->shstr+tsh->sh_name;
      uint8_t*dst_buf=NULL;uint64_t dst_base=0;
      if(!strncmp(tname,".text",5)||!strncmp(tname,".rodata",7)){dst_buf=text_buf;dst_base=text_vma;}
      else if(!strncmp(tname,".data",5)){dst_buf=data_buf;dst_base=data_vma;}
      else continue;
      uint64_t sec_vma=o->vma[tsi];
      uint32_t nrela=(uint32_t)(rsh->sh_size/sizeof(Elf64_Rela));
      Elf64_Rela*relas=(Elf64_Rela*)(o->data+rsh->sh_offset);
      for(uint32_t ri=0;ri<nrela;ri++){
        Elf64_Rela*r=&relas[ri];
        uint32_t sym_idx=ELF_R_SYM(r->r_info);
        uint32_t rtype=ELF_R_TYPE(r->r_info);
        Elf64_Sym*rsym=&esyms[sym_idx];
        uint64_t sym_vma=0;
        if(rsym->st_shndx==SHN_UNDEF){
          const char*sname=symstr+rsym->st_name;
          uint32_t li=lsym_find(sname);
          if(li==(uint32_t)-1||!lsyms[li].defined)die("undefined: %s",sname);
          sym_vma=lsyms[li].vma;
        }else if(rsym->st_shndx==SHN_ABS){sym_vma=rsym->st_value;}
        else{sym_vma=o->vma[rsym->st_shndx]+rsym->st_value;}
        uint64_t P=sec_vma+r->r_offset;uint64_t S=sym_vma;int64_t A=r->r_addend;
        uint8_t*patch=dst_buf+(P-dst_base);
        switch(rtype){
          case R_X86_64_64:{uint64_t v=S+A;memcpy(patch,&v,8);break;}
          case R_X86_64_32:case R_X86_64_32S:{uint32_t v=(uint32_t)(S+A);memcpy(patch,&v,4);break;}
          case R_X86_64_PC32:case R_X86_64_PLT32:{int32_t v=(int32_t)(S+A-P);memcpy(patch,&v,4);break;}
          case R_X86_64_PC16:{int16_t v=(int16_t)(S+A-P);memcpy(patch,&v,2);break;}
          case R_X86_64_PC8:{int8_t v=(int8_t)(S+A-P);memcpy(patch,&v,1);break;}
          case R_X86_64_16:{uint16_t v=(uint16_t)(S+A);memcpy(patch,&v,2);break;}
          case R_X86_64_8:{uint8_t v=(uint8_t)(S+A);memcpy(patch,&v,1);break;}
          default:warn("unhandled reloc type %u",rtype);break;}}}}

  /* write ELF executable */
  FILE*out=fopen(outpath,"wb");if(!out)die("cannot open %s",outpath);
  Elf64_Ehdr eh;memset(&eh,0,sizeof eh);
  eh.e_ident[0]=0x7f;eh.e_ident[1]='E';eh.e_ident[2]='L';eh.e_ident[3]='F';
  eh.e_ident[4]=2;eh.e_ident[5]=1;eh.e_ident[6]=1;
  eh.e_type=ET_EXEC;eh.e_machine=EM_X86_64;eh.e_version=1;
  eh.e_entry=entry_vma;eh.e_phoff=sizeof(Elf64_Ehdr);
  eh.e_ehsize=sizeof(Elf64_Ehdr);eh.e_phentsize=sizeof(Elf64_Phdr);eh.e_phnum=2;
  eh.e_shentsize=sizeof(Elf64_Shdr);eh.e_shnum=0;fwrite(&eh,sizeof eh,1,out);

  Elf64_Phdr ph;memset(&ph,0,sizeof ph);
  ph.p_type=PT_LOAD;ph.p_flags=PF_R|PF_X;ph.p_offset=0;
  ph.p_vaddr=load_base;ph.p_paddr=load_base;
  ph.p_filesz=text_foff+text_size;ph.p_memsz=ph.p_filesz;
  if(rodata_idx!=(uint32_t)-1){
    ph.p_filesz=(outsecs[rodata_idx].vma-load_base)+outsecs[rodata_idx].size;
    ph.p_memsz=ph.p_filesz;}
  ph.p_align=page;fwrite(&ph,sizeof ph,1,out);
  memset(&ph,0,sizeof ph);
  ph.p_type=PT_LOAD;ph.p_flags=PF_R|PF_W;
  ph.p_offset=data_foff;ph.p_vaddr=data_vma;ph.p_paddr=data_vma;
  ph.p_filesz=data_size;ph.p_memsz=data_size+bss_size;ph.p_align=page;
  fwrite(&ph,sizeof ph,1,out);

  {long cur=ftell(out);while(cur<(long)text_foff){fputc(0,out);cur++;}}
  fwrite(text_buf,1,text_size,out);
  if(rodata_idx!=(uint32_t)-1){
    long cur=ftell(out);uint64_t roff=outsecs[rodata_idx].vma-text_vma;
    while(cur<(long)(text_foff+roff)){fputc(0,out);cur++;}
    for(uint32_t oi=0;oi<nlobjs;oi++){
      ObjFile*o=&lobjs[oi];
      for(uint32_t si=1;si<o->nshdr;si++){
        if(o->shdrs[si].sh_type==SHT_NOBITS)continue;
        const char*n=o->shstr+o->shdrs[si].sh_name;
        if(!strncmp(n,".rodata",7)){
          uint64_t woff=o->vma[si]-text_vma;
          fseek(out,(long)(text_foff+woff),SEEK_SET);
          fwrite(o->data+o->shdrs[si].sh_offset,1,o->shdrs[si].sh_size,out);}}}}
  {long cur=ftell(out);while(cur<(long)data_foff){fputc(0,out);cur++;}}
  fwrite(data_buf,1,data_size,out);
  fclose(out);free(text_buf);free(data_buf);
  chmod(outpath,0755);}

/* ───────── main ───────── */
static char*read_file(const char*path){
  FILE*f=fopen(path,"rb");if(!f){perror(path);exit(1);}
  fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);
  char*buf=malloc(sz+2);if(fread(buf,1,sz,f)){}
  buf[sz]='\n';buf[sz+1]=0;fclose(f);return buf;}

int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"Usage: asl [-o out] [--entry sym] input...\n");return 1;}
  const char*outfile="a.out";char*entry_sym=NULL;
  char*inputs[64];int ninputs=0;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"-o")&&i+1<argc){outfile=argv[++i];continue;}
    if((!strcmp(argv[i],"--entry")||!strcmp(argv[i],"-e"))&&i+1<argc){entry_sym=argv[++i];continue;}
    if(!strncmp(argv[i],"-l",2)||!strcmp(argv[i],"-static")||
       !strcmp(argv[i],"-nostdlib")||!strcmp(argv[i],"-nostartfiles"))continue;
    if(argv[i][0]=='-')continue;
    inputs[ninputs++]=argv[i];}
  if(!ninputs){fprintf(stderr,"no input files\n");return 1;}
  const char*ext=strrchr(inputs[0],'.');
  int mode_link=(ext&&(!strcmp(ext,".o")||!strcmp(ext,".a")))||ninputs>1;
  if(!mode_link&&ext&&!strcmp(ext,".s")){
    const char*oext=strrchr(outfile,'.');
    mode_link=oext&&strcmp(oext,".o")!=0;}
  if(!mode_link){
    /* assemble only */
    for(int i=0;i<ninputs;i++){
      nsyms=0;nsecs=0;cur_sec=0;nlobjs=0;noutsecs=0;nlsyms=0;
      const char*inp=inputs[i];g_file=inp;
      char*src=read_file(inp);assemble(src);free(src);
      char derived[256];const char*o_out=outfile;
      if(ninputs>1){strncpy(derived,inp,250);
        char*dot=strrchr(derived,'.');if(dot)*dot=0;
        strncat(derived,".o",255);o_out=derived;}
      relax_all();
      FILE*of=fopen(o_out,"wb");if(!of){perror(o_out);return 1;}
      write_object(of);fclose(of);}
  }else{
    /* assemble .s files to temp .o, then link */
    char*link_in[64];int nlink=0;
    char tmps[64][256];int ntmp=0;
    for(int i=0;i<ninputs;i++){
      const char*e2=strrchr(inputs[i],'.');
      if(e2&&!strcmp(e2,".s")){
        nsyms=0;nsecs=0;cur_sec=0;nlobjs=0;noutsecs=0;nlsyms=0;
        g_file=inputs[i];char*src=read_file(inputs[i]);
        assemble(src);free(src);relax_all();
        snprintf(tmps[ntmp],255,"/tmp/asl_%d_%d.o",ntmp,(int)getpid());
        FILE*tf=fopen(tmps[ntmp],"wb");if(!tf){perror(tmps[ntmp]);return 1;}
        write_object(tf);fclose(tf);
        link_in[nlink++]=tmps[ntmp++];
        nsyms=0;nsecs=0;cur_sec=0;nlobjs=0;noutsecs=0;nlsyms=0;
      }else link_in[nlink++]=inputs[i];}
    link_objs(outfile,entry_sym,nlink,link_in);
    for(int i=0;i<ntmp;i++)remove(tmps[i]);}
  return 0;}
/* debug hook - remove later */
