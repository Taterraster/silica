#define _GNU_SOURCE
/*
 * codegen.c – Silica → x86-64 Linux AT&T assembly
 *
 * No libc. All I/O via sys_write (1), memory via sys_brk (12), exit via sys_exit (60).
 *
 * Variable storage (stack, rbp-relative):
 *   int/long/uint  → 8 bytes (qword)
 *   char/byte      → 8 bytes (qword, low byte used)
 *   bool           → 8 bytes (0 or 1)
 *   float          → 8 bytes (IEEE 754 double, stored via XMM)
 *   string         → 16 bytes: [ptr:8][len:8]
 *
 * Printing integers:
 *   We emit an inline itoa routine (int_to_str) in .text that converts
 *   rax → ASCII digits on the stack, then calls sys_write.
 *   This is pure assembly — zero libc.
 */

#include "codegen.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── string literal pool ── */
#define MAX_STRS 512
typedef struct { char *label; char *value; size_t len; } StrLit;
static StrLit str_pool[MAX_STRS];
static int    str_pool_n = 0;

static const char *pool_add(const char *s, int *counter) {
    char lbl[32];
    snprintf(lbl, sizeof(lbl), ".Lstr%d", (*counter)++);
    str_pool[str_pool_n].label = strdup(lbl);
    str_pool[str_pool_n].value = strdup(s);
    str_pool[str_pool_n].len   = strlen(s);
    return str_pool[str_pool_n++].label;
}

/* ── variable table ── */
#define MAX_VARS 256
typedef struct {
    char    *name;
    VarType  type;
    int      offset;       /* rbp - offset (points to first byte of var) */
    int      is_const;     /* 1 = cannot be reassigned */
    char    *struct_name;  /* for TYPE_STRUCT vars: name of the struct type */
    int      size;         /* total bytes occupied on stack */
} Var;

typedef struct {
    FILE *out;
    int   str_counter;
    int   lbl_counter;   /* for unique local labels */
    int   using_io;      /* import std.io;   */
    int   using_math;    /* import std.math; */
    int   using_fs;      /* import std.fs;   */
    int   using_mem;     /* import std.mem;  */
    int   using_time;    /* import std.time; */
    int   using_net;     /* import std.net;  */
    int   using_proc;    /* import std.proc; */
    int   using_str;     /* import std.str;  */
    int   using_env;     /* import std.env;  */
    int   had_error;     /* set to 1 on import gate violation → compilation fails */
    int   exit_code;
    int   stack_used;
    int   need_itoa;     /* do we emit the int_to_str helper? */
    Var   vars[MAX_VARS];
    int   nvar;
    /* user function registry for call resolution */
    FuncDecl  **funcs;
    int         nfuncs;
    /* struct type registry */
    StructDecl **structs;
    int          nstructs;
    /* enum registry */
    EnumDecl   **enums;
    int          nenums;
    /* current function's return label and type */
    char       ret_label[64];
    VarType    cur_rettype;
    /* loop label stack for break/continue */
    char       loop_top[32][64];   /* label to jump to for continue */
    char       loop_end[32][64];   /* label to jump to for break    */
    int        loop_depth;
} CG;

static int alloc_var(CG *cg, const char *name, VarType t, int is_const) {
    int size = (t == TYPE_STRING) ? 16 : 8;
    cg->stack_used += size;
    cg->vars[cg->nvar].name        = strdup(name);
    cg->vars[cg->nvar].type        = t;
    cg->vars[cg->nvar].offset      = cg->stack_used;
    cg->vars[cg->nvar].is_const    = is_const;
    cg->vars[cg->nvar].struct_name = NULL;
    cg->vars[cg->nvar].size        = size;
    return cg->nvar++;
}

/* Allocate a struct variable — N fields * 8 bytes, returns index of var */
static int alloc_struct_var(CG *cg, const char *name, const char *struct_type_name, int total_bytes) {
    int size = (total_bytes + 7) & ~7; /* round up to 8 */
    if (size < 8) size = 8;
    cg->stack_used += size;
    cg->vars[cg->nvar].name        = strdup(name);
    cg->vars[cg->nvar].type        = TYPE_STRUCT;
    cg->vars[cg->nvar].offset      = cg->stack_used;
    cg->vars[cg->nvar].is_const    = 0;
    cg->vars[cg->nvar].struct_name = struct_type_name ? strdup(struct_type_name) : NULL;
    cg->vars[cg->nvar].size        = size;
    return cg->nvar++;
}

static Var *find_var(CG *cg, const char *name) {
    for (int i = 0; i < cg->nvar; i++)
        if (strcmp(cg->vars[i].name, name) == 0)
            return &cg->vars[i];
    return NULL;
}

static FuncDecl *find_userfunc(CG *cg, const char *name) {
    /* exact match by name only (for contexts where arity isn't known) */
    for (int i = 0; i < cg->nfuncs; i++)
        if (strcmp(cg->funcs[i]->name, name) == 0)
            return cg->funcs[i];
    return NULL;
}

static StructDecl *find_struct(CG *cg, const char *name) {
    for (int i = 0; i < cg->nstructs; i++)
        if (strcmp(cg->structs[i]->name, name) == 0)
            return cg->structs[i];
    return NULL;
}

static StructField *find_field(StructDecl *sd, const char *name) {
    for (int i = 0; i < sd->nfields; i++)
        if (strcmp(sd->fields[i].name, name) == 0)
            return &sd->fields[i];
    return NULL;
}

/* Infer the broad VarType of an expression (for overload resolution) */
static VarType infer_expr_type(CG *cg, Expr *e) {
    if (!e) return TYPE_INT;
    switch (e->kind) {
        case EXPR_INT_LIT:    return TYPE_INT;
        case EXPR_FLOAT_LIT:  return TYPE_FLOAT;
        case EXPR_BOOL_LIT:   return TYPE_BOOL;
        case EXPR_CHAR_LIT:   return TYPE_CHAR;
        case EXPR_STRING_LIT: return TYPE_STRING;
        case EXPR_IDENT: {
            Var *v = find_var(cg, e->sval);
            return v ? v->type : TYPE_INT;
        }
        default: return TYPE_INT;
    }
}

/* Type char for mangling: int/long/uint/bool→i, float→f, string→s, char/byte→c */
static char type_char(VarType t) {
    switch (t) {
        case TYPE_INT: case TYPE_LONG: case TYPE_UINT: case TYPE_BOOL: return 'i';
        case TYPE_FLOAT:  return 'f';
        case TYPE_STRING: return 's';
        case TYPE_CHAR: case TYPE_BYTE: return 'c';
        default: return 'v';
    }
}

/* Find overloaded function by name + argument count, then by type signature */
static FuncDecl *find_userfunc_overload(CG *cg, const char *name, int argc, Expr **args) {
    /* Pass 1: name + arity + type signature match */
    if (args) {
        for (int i = 0; i < cg->nfuncs; i++) {
            FuncDecl *fd = cg->funcs[i];
            if (strcmp(fd->name, name) != 0 || fd->nparams != argc) continue;
            int match = 1;
            for (int j = 0; j < argc; j++) {
                VarType at = infer_expr_type(cg, args[j]);
                char ac = type_char(at);
                char fc = type_char(fd->params[j].type);
                if (ac != fc) { match = 0; break; }
            }
            if (match) return fd;
        }
    }
    /* Pass 2: name + arity only */
    for (int i = 0; i < cg->nfuncs; i++)
        if (strcmp(cg->funcs[i]->name, name) == 0 && cg->funcs[i]->nparams == argc)
            return cg->funcs[i];
    /* Pass 3: name only */
    return find_userfunc(cg, name);
}

/* Type char for mangling — duplicate removed, definition is above */
/* Build mangled label: no overloads = plain, overloaded = type-sig suffix */
static void func_label(CG *cg, FuncDecl *fd, char *out, int n) {
    int ov = 0;
    for (int i = 0; i < cg->nfuncs; i++)
        if (cg->funcs[i] != fd && strcmp(cg->funcs[i]->name, fd->name) == 0) { ov=1; break; }
    if (!ov) { snprintf(out, n, "__silica_user_%s", fd->name); return; }
    char sig[64]; int m = fd->nparams < 63 ? fd->nparams : 63;
    if (m == 0) { sig[0]='v'; sig[1]=0; }
    else { for (int i=0;i<m;i++) sig[i]=type_char(fd->params[i].type); sig[m]=0; }
    snprintf(out, n, "__silica_user_%s_%s", fd->name, sig);
}

static void flatten(Expr *e, char *buf, int n) {
    if (e->kind == EXPR_IDENT)
        strncpy(buf, e->sval, n - 1);
    else if (e->kind == EXPR_FIELD) {
        char tmp[256] = "";
        flatten(e->object, tmp, sizeof(tmp));
        snprintf(buf, n, "%s.%s", tmp, e->field);
    }
}

/* ── emit sys_write(1, label, len) ── */
static void emit_write_label(FILE *out, const char *label, size_t len) {
    fprintf(out,
        "    movq $1, %%rax\n"
        "    movq $1, %%rdi\n"
        "    leaq %s(%%rip), %%rsi\n"
        "    movq $%zu, %%rdx\n"
        "    syscall\n",
        label, len);
}

/* ── emit sys_write for a string variable (ptr+len on stack) ── */
static void emit_write_strvar(FILE *out, int offset) {
    fprintf(out,
        "    movq $1, %%rax\n"
        "    movq $1, %%rdi\n"
        "    movq -%d(%%rbp), %%rsi\n"
        "    movq -%d(%%rbp), %%rdx\n"
        "    syscall\n",
        offset, offset - 8);
}

/*
 * emit_print_int:
 *   Converts the integer in %rax to decimal ASCII on the stack
 *   and calls sys_write. Handles negative numbers.
 *   Uses registers: rax, rbx, rcx, rdx, rsi, rdi + 24 bytes of stack scratch.
 */
static void emit_print_int(CG *cg, int newline) {
    int lbl = cg->lbl_counter++;
    cg->need_itoa = 1;
    fprintf(cg->out,
        "    /* print int in %%rax */\n"
        "    callq __silica_print_int%s\n",
        newline ? "_nl" : "");
    (void)lbl;
}

/*
 * emit_print_char_val:
 *   Writes the char byte in %al to stdout.
 */
static void emit_print_char_reg(FILE *out, int newline) {
    /* store char + optional newline to stack scratch, then write */
    fprintf(out,
        "    /* print char in %%al */\n"
        "    subq $16, %%rsp\n"
        "    movb %%al, (%%rsp)\n");
    if (newline)
        fprintf(out, "    movb $10, 1(%%rsp)\n");
    fprintf(out,
        "    movq $1, %%rax\n"
        "    movq $1, %%rdi\n"
        "    movq %%rsp, %%rsi\n"
        "    movq $%d, %%rdx\n"
        "    syscall\n"
        "    addq $16, %%rsp\n",
        newline ? 2 : 1);
}

/* emit_print_float: convert double in xmm0 to string via __silica_print_float */
static void emit_print_float(CG *cg, int newline) {
    cg->need_itoa = 1; /* reuse flag; float helper also emitted */
    fprintf(cg->out,
        "    callq __silica_print_float%s\n",
        newline ? "_nl" : "");
}

/* forward */
static void emit_expr(CG *cg, Expr *e);
static void emit_call(CG *cg, Expr *e);
static void emit_float_expr(CG *cg, Expr *e);
static void emit_store_var(CG *cg, Var *v);
static void emit_init_expr(CG *cg, VarType dtype, Expr *src);

/*
 * emit_int_expr:
 *   Evaluates an integer expression tree and leaves result in %rax.
 *   Uses the x86-64 red zone for sub-expression temporaries.
 *   Scratch register: rbx (saved/restored around recursive calls via push/pop).
 */
static void emit_int_expr(CG *cg, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:
            fprintf(cg->out, "    movq $%ld, %%rax\n", e->ival);
            break;
        case EXPR_BOOL_LIT:
            fprintf(cg->out, "    movq $%ld, %%rax\n", e->ival ? 1L : 0L);
            break;
        case EXPR_IDENT: {
            Var *v = find_var(cg, e->sval);
            if (v) {
                switch (v->type) {
                    case TYPE_INT: case TYPE_LONG: case TYPE_UINT:
                        fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset); break;
                    case TYPE_BYTE:
                        fprintf(cg->out, "    movzbq -%d(%%rbp), %%rax\n", v->offset); break;
                    case TYPE_CHAR:
                        fprintf(cg->out, "    movzbq -%d(%%rbp), %%rax\n", v->offset); break;
                    default:
                        fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset); break;
                }
            } else {
                /* Check if it's an enum member constant */
                int found_enum = 0;
                /* Walk enum registry */
                for (int ei = 0; ei < cg->nenums && !found_enum; ei++) {
                    EnumDecl *ed = cg->enums[ei];
                    for (int mi = 0; mi < ed->nmembers; mi++) {
                        if (strcmp(ed->member_names[mi], e->sval) == 0) {
                            fprintf(cg->out, "    movq $%ld, %%rax\n", ed->member_values[mi]);
                            found_enum = 1;
                            break;
                        }
                    }
                }
                if (!found_enum) {
                    fprintf(stderr, "[codegen] unknown variable '%s'\n", e->sval);
                    fprintf(cg->out, "    xorq %%rax, %%rax\n");
                }
            }
            break;
        }
        case EXPR_FIELD: {
            /* Two cases:
               1. myStr.length — returns the length of a string variable
               2. myStruct.field — returns the value of a struct field */
            if (e->object && e->object->kind == EXPR_IDENT) {
                Var *v = find_var(cg, e->object->sval);
                if (v && v->type == TYPE_STRING && strcmp(e->field, "length") == 0) {
                    fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset - 8);
                } else if (v && v->type == TYPE_STRUCT && v->struct_name) {
                    StructDecl *sd = find_struct(cg, v->struct_name);
                    if (sd) {
                        StructField *sf = find_field(sd, e->field);
                        if (sf) {
                            /* struct is at rbp-(v->offset) to rbp-(v->offset - total_size)
                               field is at rbp-(v->offset - sf->offset) */
                            fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n",
                                    v->offset - sf->offset);
                        } else {
                            fprintf(stderr, "[codegen] unknown struct field '%s.%s'\n",
                                    v->struct_name, e->field);
                            fprintf(cg->out, "    xorq %%rax, %%rax\n");
                        }
                    } else {
                        fprintf(cg->out, "    xorq %%rax, %%rax\n");
                    }
                } else {
                    fprintf(cg->out, "    xorq %%rax, %%rax\n");
                }
            } else {
                fprintf(cg->out, "    xorq %%rax, %%rax\n");
            }
            break;
        }
        /* &x — address of variable */
        case EXPR_ADDROF: {
            if (e->rhs && e->rhs->kind == EXPR_IDENT) {
                Var *v = find_var(cg, e->rhs->sval);
                if (v) {
                    fprintf(cg->out, "    leaq -%d(%%rbp), %%rax\n", v->offset);
                } else {
                    fprintf(stderr, "[codegen] &: unknown variable '%s'\n", e->rhs->sval);
                    fprintf(cg->out, "    xorq %%rax, %%rax\n");
                }
            } else {
                fprintf(cg->out, "    xorq %%rax, %%rax\n");
            }
            break;
        }
        /* *p — dereference pointer */
        case EXPR_DEREF: {
            emit_int_expr(cg, e->rhs);   /* rax = pointer value */
            fprintf(cg->out, "    movq (%%rax), %%rax\n");
            break;
        }
        /* p->field — pointer to struct field access */
        case EXPR_PTR_FIELD: {
            /* lhs evaluates to a pointer to a struct; ptr_field is the field name.
               We need to know which struct type it points to. Look up via the var. */
            emit_int_expr(cg, e->lhs);  /* rax = struct pointer */
            /* Try to find the struct type from the lhs variable */
            const char *stype = NULL;
            if (e->lhs->kind == EXPR_IDENT) {
                Var *v = find_var(cg, e->lhs->sval);
                if (v) stype = v->struct_name;
            }
            if (stype) {
                StructDecl *sd = find_struct(cg, stype);
                if (sd && e->ptr_field) {
                    StructField *sf = find_field(sd, e->ptr_field);
                    if (sf) {
                        fprintf(cg->out, "    movq %d(%%rax), %%rax\n", sf->offset);
                    } else {
                        fprintf(stderr, "[codegen] unknown field '%s' in '%s'\n",
                                e->ptr_field, stype);
                        fprintf(cg->out, "    xorq %%rax, %%rax\n");
                    }
                } else {
                    /* unknown struct type — just load at offset 0 */
                    fprintf(cg->out, "    movq (%%rax), %%rax\n");
                }
            } else {
                fprintf(cg->out, "    movq (%%rax), %%rax\n");
            }
            break;
        }
        case EXPR_UNARY_NEG:
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out, "    negq %%rax\n");
            break;
        case EXPR_CALL:
            /* math.* functions return their result in rax — usable in expressions */
            emit_call(cg, e);
            break;
        case EXPR_BINOP: {
            /* eval lhs into rax, push; eval rhs into rax; pop lhs into rbx */
            emit_int_expr(cg, e->lhs);
            fprintf(cg->out, "    pushq %%rax\n");
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out, "    movq %%rax, %%rcx\n");   /* rcx = rhs */
            fprintf(cg->out, "    popq  %%rax\n");          /* rax = lhs */
            switch (e->op) {
                case '+': fprintf(cg->out, "    addq %%rcx, %%rax\n"); break;
                case '-': fprintf(cg->out, "    subq %%rcx, %%rax\n"); break;
                case '*': fprintf(cg->out, "    imulq %%rcx, %%rax\n"); break;
                case '/':
                    fprintf(cg->out,
                        "    cqto\n"
                        "    idivq %%rcx\n");
                    break;
                case '%':
                    fprintf(cg->out,
                        "    cqto\n"
                        "    idivq %%rcx\n"
                        "    movq %%rdx, %%rax\n");
                    break;
            }
            break;
        }
        case EXPR_COMPARE: {
            emit_int_expr(cg, e->lhs);
            fprintf(cg->out, "    pushq %%rax\n");
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out, "    movq %%rax, %%rcx\n");
            fprintf(cg->out, "    popq  %%rax\n");
            fprintf(cg->out, "    cmpq %%rcx, %%rax\n");
            const char *setcc = "sete";
            switch (e->op) {
                case '=': setcc = "sete";  break;
                case '!': setcc = "setne"; break;
                case '<': setcc = "setl";  break;
                case '>': setcc = "setg";  break;
                case 'L': setcc = "setle"; break;
                case 'G': setcc = "setge"; break;
            }
            fprintf(cg->out, "    %-6s %%al\n", setcc);
            fprintf(cg->out, "    movzbq %%al, %%rax\n");
            break;
        }
        case EXPR_LOGICAL_AND: {
            /* short-circuit: eval lhs; if 0 → result 0, else eval rhs */
            int lbl = cg->lbl_counter++;
            emit_int_expr(cg, e->lhs);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jz .Land_false%d\n",
                lbl);
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jz .Land_false%d\n"
                "    movq $1, %%rax\n"
                "    jmp .Land_end%d\n"
                ".Land_false%d:\n"
                "    xorq %%rax, %%rax\n"
                ".Land_end%d:\n",
                lbl, lbl, lbl, lbl);
            break;
        }
        case EXPR_LOGICAL_OR: {
            /* short-circuit: eval lhs; if != 0 → result 1, else eval rhs */
            int lbl = cg->lbl_counter++;
            emit_int_expr(cg, e->lhs);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jnz .Lor_true%d\n",
                lbl);
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jnz .Lor_true%d\n"
                "    xorq %%rax, %%rax\n"
                "    jmp .Lor_end%d\n"
                ".Lor_true%d:\n"
                "    movq $1, %%rax\n"
                ".Lor_end%d:\n",
                lbl, lbl, lbl, lbl);
            break;
        }
        case EXPR_LOGICAL_NOT:
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    sete %%al\n"
                "    movzbq %%al, %%rax\n");
            break;
        case EXPR_CAST: {
            VarType target = (VarType)e->cast_type;
            if (target == TYPE_FLOAT) {
                /* (float)intExpr — convert int to double in xmm0.
                   Since emit_int_expr convention returns rax, we stash the
                   float bits into rax so callers that need rax can use it.
                   But emit_init_expr(TYPE_FLOAT) will call emit_float_expr
                   directly and get xmm0, so this path is only hit when
                   EXPR_CAST appears in a context that calls emit_int_expr. */
                emit_int_expr(cg, e->rhs);
                fprintf(cg->out,
                    "    cvtsi2sdq %%rax, %%xmm0\n"
                    "    movsd %%xmm0, -8(%%rsp)\n"      /* movsd for xmm→mem */
                    "    movq -8(%%rsp), %%rax\n");       /* rax = float bits (raw) */
            } else {
                /* (int/long/…) cast from float → truncate; otherwise passthrough */
                Var *_rv = (e->rhs->kind == EXPR_IDENT) ? find_var(cg, e->rhs->sval) : NULL;
                int rhs_is_float = (e->rhs->kind == EXPR_FLOAT_LIT) ||
                                   (_rv && _rv->type == TYPE_FLOAT);
                if (rhs_is_float) {
                    emit_float_expr(cg, e->rhs);
                    fprintf(cg->out, "    cvttsd2siq %%xmm0, %%rax\n");
                } else {
                    emit_int_expr(cg, e->rhs);
                }
            }
            break;
        }
        case EXPR_INDEX: {
            /* arr[i]: lhs is array var (ptr stored at rbp-offset),
               rhs is index. elem size = 8 for all types. */
            emit_int_expr(cg, e->rhs);              /* rax = index */
            fprintf(cg->out, "    pushq %%rax\n");
            /* get array base pointer */
            if (e->lhs->kind == EXPR_IDENT) {
                Var *v = find_var(cg, e->lhs->sval);
                if (v) fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset);
                else { fprintf(cg->out, "    xorq %%rax, %%rax\n"); }
            } else {
                emit_int_expr(cg, e->lhs);
            }
            fprintf(cg->out,
                "    popq %%rcx\n"          /* rcx = index */
                "    movq (%%rax,%%rcx,8), %%rax\n"); /* rax = arr[i] */
            break;
        }
        default:
            fprintf(cg->out, "    xorq %%rax, %%rax\n");
    }
}

/*
 * is_numeric_expr: returns 1 if the expression yields an integer result in rax
 */
static int is_numeric_expr(Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
        case EXPR_INT_LIT:    return 1;
        case EXPR_IDENT:      return 1;
        case EXPR_BINOP:      return 1;
        case EXPR_UNARY_NEG:  return 1;
        case EXPR_CALL:       return 1;
        case EXPR_COMPARE:    return 1;
        case EXPR_LOGICAL_AND: return 1;
        case EXPR_LOGICAL_OR:  return 1;
        case EXPR_LOGICAL_NOT: return 1;
        case EXPR_CAST:        return 1;
        case EXPR_INDEX:       return 1;
        case EXPR_FIELD:       return 1;  /* struct field or string.length */
        case EXPR_ADDROF:      return 1;  /* address is an integer */
        case EXPR_DEREF:       return 1;  /* dereference yields integer */
        case EXPR_PTR_FIELD:   return 1;  /* pointer field access yields integer */
        default:              return 0;
    }
}

/* ── resolve namespace: always try std.X, io.X, mem.X, math.X transparently ── */
static void resolve_name(CG *cg, const char *raw, char *out, int n) {
    /* Already fully qualified */
    if (strncmp(raw, "std.", 4) == 0) { strncpy(out, raw, n-1); goto gate; }
    /* Short forms always work regardless of 'using std' */
    if (strncmp(raw, "io.",   3) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "mem.",  4) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "math.", 5) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "fs.",   3) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "str.",  4) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "time.", 5) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "net.",  4) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "proc.", 5) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    if (strncmp(raw, "env.",  4) == 0) { snprintf(out, n, "std.%s", raw); goto gate; }
    strncpy(out, raw, n-1);
    return;

gate:;
    /* ── import gate: every stdlib module requires an explicit import ── */
    if (strncmp(out, "std.io.", 7) == 0 && !cg->using_io) {
        fprintf(stderr, "[error] use of std.io requires: import std.io;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.math.", 9) == 0 && !cg->using_math) {
        fprintf(stderr, "[error] use of std.math requires: import std.math;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.fs.", 7) == 0 && !cg->using_fs) {
        fprintf(stderr, "[error] use of std.fs requires: import std.fs;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.mem.", 8) == 0 && !cg->using_mem) {
        fprintf(stderr, "[error] use of std.mem requires: import std.mem;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.time.", 9) == 0 && !cg->using_time) {
        fprintf(stderr, "[error] use of std.time requires: import std.time;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.net.", 8) == 0 && !cg->using_net) {
        fprintf(stderr, "[error] use of std.net requires: import std.net;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.proc.", 9) == 0 && !cg->using_proc) {
        fprintf(stderr, "[error] use of std.proc requires: import std.proc;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.str.", 8) == 0 && !cg->using_str) {
        fprintf(stderr, "[error] use of std.str requires: import std.str;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
    if (strncmp(out, "std.env.", 8) == 0 && !cg->using_env) {
        fprintf(stderr, "[error] use of std.env requires: import std.env;\n");
        cg->had_error = 1; out[0] = '\0'; return;
    }
}

/* ── emit a call expression ── */
static void emit_call(CG *cg, Expr *e) {
    char raw[256] = "", name[256] = "";
    flatten(e->callee, raw, sizeof(raw));
    resolve_name(cg, raw, name, sizeof(name));

    fprintf(cg->out, "    /* call: %s */\n", name);

    /* ── io.print / io.println ── */
    if (strcmp(name, "std.io.print") == 0 || strcmp(name, "std.io.println") == 0) {
        int nl = strstr(name, "println") != NULL;
        if (e->argc < 1) return;
        Expr *arg = e->args[0];

        switch (arg->kind) {
            case EXPR_FIELD: {
                /* Distinguish math constants from struct field access */
                char flat[256] = ""; flatten(arg, flat, sizeof(flat));
                char res[256]  = ""; resolve_name(cg, flat, res, sizeof(res));
                if (strcmp(res, "std.math.i") == 0) {
                    const char *txt = nl ? "sqrt(-1)\n" : "sqrt(-1)";
                    const char *lbl = pool_add(txt, &cg->str_counter);
                    emit_write_label(cg->out, lbl, strlen(txt));
                } else if (strncmp(res, "std.math.", 9) == 0) {
                    /* math constant (pi, e, etc.) → float */
                    emit_float_expr(cg, arg);
                    emit_print_float(cg, nl);
                } else if (arg->object && arg->object->kind == EXPR_IDENT) {
                    /* struct field access: detect type from struct def */
                    Var *sv = find_var(cg, arg->object->sval);
                    if (sv && sv->type == TYPE_STRING && strcmp(arg->field, "length") == 0) {
                        emit_int_expr(cg, arg);
                        emit_print_int(cg, nl);
                    } else if (sv && sv->type == TYPE_STRUCT && sv->struct_name) {
                        StructDecl *sd = find_struct(cg, sv->struct_name);
                        StructField *sf = sd ? find_field(sd, arg->field) : NULL;
                        if (sf && sf->type == TYPE_FLOAT) {
                            emit_float_expr(cg, arg);
                            emit_print_float(cg, nl);
                        } else {
                            emit_int_expr(cg, arg);
                            emit_print_int(cg, nl);
                        }
                    } else {
                        emit_int_expr(cg, arg);
                        emit_print_int(cg, nl);
                    }
                } else {
                    emit_float_expr(cg, arg);
                    emit_print_float(cg, nl);
                }
                break;
            }
            case EXPR_STRING_LIT: {
                char *txt = nl ? malloc(strlen(arg->sval) + 2) : strdup(arg->sval);
                if (nl) { strcpy(txt, arg->sval); strcat(txt, "\n"); }
                const char *lbl = pool_add(txt, &cg->str_counter);
                emit_write_label(cg->out, lbl, strlen(txt));
                free(txt);
                break;
            }
            case EXPR_BOOL_LIT: {
                const char *txt = nl
                    ? (arg->ival ? "true\n" : "false\n")
                    : (arg->ival ? "true"   : "false");
                const char *lbl = pool_add(txt, &cg->str_counter);
                emit_write_label(cg->out, lbl, strlen(txt));
                break;
            }
            case EXPR_CHAR_LIT:
                fprintf(cg->out, "    movb $%d, %%al\n", (unsigned char)arg->cval);
                emit_print_char_reg(cg->out, nl);
                break;
            case EXPR_FLOAT_LIT: {
                double d = arg->fval;
                unsigned long long bits;
                memcpy(&bits, &d, 8);
                fprintf(cg->out,
                    "    movq $0x%llx, %%rax\n"
                    "    movq %%rax, -512(%%rbp)\n"
                    "    movsd -512(%%rbp), %%xmm0\n",
                    (unsigned long long)bits);
                emit_print_float(cg, nl);
                break;
            }
            case EXPR_IDENT: {
                Var *v = find_var(cg, arg->sval);
                if (!v) {
                    /* check if it's an enum constant */
                    int found_enum = 0;
                    for (int ei = 0; ei < cg->nenums && !found_enum; ei++) {
                        EnumDecl *ed = cg->enums[ei];
                        for (int mi = 0; mi < ed->nmembers; mi++) {
                            if (strcmp(ed->member_names[mi], arg->sval) == 0) {
                                fprintf(cg->out, "    movq $%ld, %%rax\n", ed->member_values[mi]);
                                emit_print_int(cg, nl);
                                found_enum = 1;
                                break;
                            }
                        }
                    }
                    if (!found_enum)
                        fprintf(stderr, "[codegen] unknown var '%s'\n", arg->sval);
                    break;
                }
                switch (v->type) {
                    case TYPE_STRING:
                        emit_write_strvar(cg->out, v->offset);
                        if (nl) {
                            const char *nlbl = pool_add("\n", &cg->str_counter);
                            emit_write_label(cg->out, nlbl, 1);
                        }
                        break;
                    case TYPE_INT: case TYPE_LONG: case TYPE_UINT:
                        fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset);
                        emit_print_int(cg, nl);
                        break;
                    case TYPE_BYTE:
                        fprintf(cg->out, "    movzbq -%d(%%rbp), %%rax\n", v->offset);
                        emit_print_int(cg, nl);
                        break;
                    case TYPE_CHAR:
                        fprintf(cg->out, "    movb -%d(%%rbp), %%al\n", v->offset);
                        emit_print_char_reg(cg->out, nl);
                        break;
                    case TYPE_BOOL: {
                        int lbl = cg->lbl_counter++;
                        const char *t_str = nl ? "true\n"  : "true";
                        const char *f_str = nl ? "false\n" : "false";
                        const char *tl = pool_add(t_str, &cg->str_counter);
                        const char *fl = pool_add(f_str, &cg->str_counter);
                        fprintf(cg->out,
                            "    cmpq $0, -%d(%%rbp)\n"
                            "    je .Lbool_false%d\n"
                            "    movq $1, %%rax\n"
                            "    movq $1, %%rdi\n"
                            "    leaq %s(%%rip), %%rsi\n"
                            "    movq $%zu, %%rdx\n"
                            "    syscall\n"
                            "    jmp .Lbool_end%d\n"
                            ".Lbool_false%d:\n"
                            "    movq $1, %%rax\n"
                            "    movq $1, %%rdi\n"
                            "    leaq %s(%%rip), %%rsi\n"
                            "    movq $%zu, %%rdx\n"
                            "    syscall\n"
                            ".Lbool_end%d:\n",
                            v->offset, lbl,
                            tl, strlen(t_str), lbl,
                            lbl,
                            fl, strlen(f_str),
                            lbl);
                        break;
                    }
                    case TYPE_FLOAT:
                        fprintf(cg->out, "    movsd -%d(%%rbp), %%xmm0\n", v->offset);
                        emit_print_float(cg, nl);
                        break;
                }
                break;
            }
            default: {
                /* check if it's a float-returning expression */
                int is_float = 0;
                if (arg->kind == EXPR_FIELD) {
                    /* math constants */
                    char flat[256]=""; flatten(arg,flat,sizeof(flat));
                    char res[256]="";  resolve_name(cg,flat,res,sizeof(res));
                    if (strcmp(res,"std.math.pi")==0 || strcmp(res,"std.math.e")==0)
                        is_float = 1;
                    /* math.i handled above in EXPR_FIELD case */
                }
                /* check if it's a string-returning call */
                int is_string = 0;
                if (arg->kind == EXPR_CALL) {
                    char craw[256] = "";
                    flatten(arg->callee, craw, sizeof(craw));
                    char cres[256] = "";
                    resolve_name(cg, craw, cres, sizeof(cres));
                    /* std library float-returning functions */
                    if (strcmp(cres,"std.math.sin")==0 || strcmp(cres,"std.math.cos")==0
                            || strcmp(cres,"std.math.log")==0
                            || strcmp(cres,"std.math.integral")==0)
                        is_float = 1;
                    /* std library string-returning functions */
                    if (strcmp(cres,"std.str.concat")==0
                            || strcmp(cres,"std.str.slice")==0
                            || strcmp(cres,"std.str.from_int")==0
                            || strcmp(cres,"std.str.from_float")==0
                            || strcmp(cres,"std.str.upper")==0
                            || strcmp(cres,"std.str.lower")==0
                            || strcmp(cres,"std.str.trim")==0
                            || strcmp(cres,"std.str.repeat")==0
                            || strcmp(cres,"std.env.argv")==0
                            || strcmp(cres,"std.env.get")==0
                            || strcmp(cres,"std.net.recv")==0
                            || strcmp(cres,"std.fs.read")==0
                            || strcmp(cres,"str.concat")==0
                            || strcmp(cres,"str.slice")==0
                            || strcmp(cres,"str.from_int")==0
                            || strcmp(cres,"str.upper")==0
                            || strcmp(cres,"str.lower")==0
                            || strcmp(cres,"str.trim")==0
                            || strcmp(cres,"str.repeat")==0
                            || strcmp(cres,"env.argv")==0
                            || strcmp(cres,"env.get")==0)
                        is_string = 1;
                    /* user-defined float-returning functions */
                    if (!is_float && !is_string) {
                        FuncDecl *fd = find_userfunc_overload(cg, craw, arg->argc, arg->args);
                        if (fd && fd->rettype == TYPE_FLOAT) is_float = 1;
                        if (fd && fd->rettype == TYPE_STRING) is_string = 1;
                    }
                }
                if (arg->kind == EXPR_FLOAT_LIT) is_float = 1;
                if (is_string) {
                    /* call returns rax=ptr, rdx=len — write it */
                    emit_call(cg, arg);
                    fprintf(cg->out,
                        "    /* print string from call: rax=ptr rdx=len */\n"
                        "    movq %%rax, %%rsi\n"
                        "    movq $1, %%rax\n"
                        "    movq $1, %%rdi\n"
                        "    syscall\n");
                    if (nl) {
                        const char *nlbl = pool_add("\n", &cg->str_counter);
                        emit_write_label(cg->out, nlbl, 1);
                    }
                } else if (is_float) {
                    emit_float_expr(cg, arg);
                    emit_print_float(cg, nl);
                } else if (is_numeric_expr(arg)) {
                    emit_int_expr(cg, arg);
                    emit_print_int(cg, nl);
                } else {
                    fprintf(stderr, "[codegen] print: unsupported argument type\n");
                }
            }
        }
        return;
    }

    /* ── mem.alloc(n, unit) → void* ── allocates n*unit bytes, returns pointer ── */
    if (strcmp(name, "std.mem.alloc") == 0) {
        if (e->argc < 2) return;
        long count = (e->args[0]->kind == EXPR_INT_LIT) ? e->args[0]->ival : 0;
        long unit  = (e->args[1]->kind == EXPR_INT_LIT) ? e->args[1]->ival : 0;
        long bytes = count;
        switch (unit) {
            case 0: bytes = count;                       break;
            case 1: bytes = count * 1024LL;              break;
            case 2: bytes = count * 1024LL * 1024;       break;
            case 3: bytes = count * 1024LL * 1024 * 1024; break;
        }
        fprintf(cg->out,
            "    /* mem.alloc(%ld, %ld) = %ld bytes → rax=ptr */\n"
            "    movq $12, %%rax\n"          /* sys_brk(0) → current brk */
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"        /* r12 = old brk = our ptr */
            "    movq %%rax, %%rdi\n"
            "    addq $%ld, %%rdi\n"
            "    movq $12, %%rax\n"          /* sys_brk(new) → extend heap */
            "    syscall\n"
            "    movq %%r12, %%rax\n",       /* return old brk as the pointer */
            count, unit, bytes, bytes);
        return;
    }

    /* ── mem.alloc_raw(bytes) → void* ── allocates exactly N bytes, returns pointer ── */
    if (strcmp(name, "std.mem.alloc_raw") == 0) {
        if (e->argc < 1) return;
        fprintf(cg->out, "    /* mem.alloc_raw(n) → rax=ptr */\n");
        emit_int_expr(cg, e->args[0]);          /* rax = n */
        fprintf(cg->out,
            "    movq %%rax, %%r13\n"            /* r13 = n */
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"                      /* rax = old brk */
            "    movq %%rax, %%r12\n"            /* r12 = ptr */
            "    movq %%rax, %%rdi\n"
            "    addq %%r13, %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            "    movq %%r12, %%rax\n");          /* return ptr */
        return;
    }

    /* ── mem.free(ptr) ── no-op stub (brk allocator has no free) ── */
    if (strcmp(name, "std.mem.free") == 0) {
        fprintf(cg->out, "    /* mem.free: no-op (brk allocator) */\n");
        if (e->argc > 0) emit_int_expr(cg, e->args[0]); /* evaluate for side effects */
        return;
    }

    /* ── io.input / io.inputln ── */
    if (strcmp(name, "std.io.input") == 0 || strcmp(name, "std.io.inputln") == 0) {
        int strip_nl = (strstr(name, "inputln") != NULL);
        if (e->argc < 2) {
            fprintf(stderr, "[codegen] io.input needs 2 args: (\"prompt\", varname)\n");
            return;
        }

        /* print prompt */
        Expr *prompt = e->args[0];
        if (prompt->kind == EXPR_STRING_LIT) {
            const char *lbl = pool_add(prompt->sval, &cg->str_counter);
            emit_write_label(cg->out, lbl, strlen(prompt->sval));
        }

        /* resolve target variable */
        Expr *target = e->args[1];
        const char *varname = (target->kind == EXPR_IDENT) ? target->sval : NULL;
        Var *v = varname ? find_var(cg, varname) : NULL;
        if (!v || v->type != TYPE_STRING) {
            fprintf(stderr, "[codegen] io.input: '%s' must be a declared string variable\n",
                    varname ? varname : "?");
            return;
        }

        int lbl = cg->lbl_counter++;
        /*
         * Read one line from stdin into a 4096-byte brk buffer.
         * We call __silica_readline which reads byte-by-byte until
         * \n or EOF, returning ptr in r12 and byte-count in r13.
         * strip_nl variant then trims the trailing \n.
         */
        fprintf(cg->out,
            "    /* io.input%s(\"%s\", %s) */\n"
            "    callq __silica_readline\n",   /* r12=buf, r13=len (includes \n) */
            strip_nl ? "ln" : "",
            prompt->kind == EXPR_STRING_LIT ? prompt->sval : "",
            v->name);

        if (strip_nl) {
            fprintf(cg->out,
                "    /* strip trailing newline */\n"
                "    testq %%r13, %%r13\n"
                "    jz .Linput_done%d\n"
                "    movq %%r13, %%rcx\n"
                "    decq %%rcx\n"
                "    movb (%%r12,%%rcx,1), %%al\n"
                "    cmpb $10, %%al\n"
                "    jne .Linput_done%d\n"
                "    decq %%r13\n"
                ".Linput_done%d:\n",
                lbl, lbl, lbl);
        }

        fprintf(cg->out,
            "    movq %%r12, -%d(%%rbp)\n"
            "    movq %%r13, -%d(%%rbp)\n",
            v->offset, v->offset - 8);
        return;
    }

    /* ── string operations ── */

    /* str.length(s) → rax = len  (also handles s.length as field — see emit_expr) */
    if (strcmp(name, "std.str.length") == 0 || strcmp(name, "str.length") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.length needs 1 arg\n"); return; }
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out, "    movq %%rdx, %%rax\n");   /* len is in rdx */
        return;
    }

    /* str.concat(a, b) → rax=ptr, rdx=len  — allocate new buffer via brk */
    if (strcmp(name, "std.str.concat") == 0 || strcmp(name, "str.concat") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] str.concat needs 2 args\n"); return; }
        int lbl = cg->lbl_counter++;
        (void)lbl;
        /* Push a (ptr, len) then b (ptr, len).
           After 4 pushes, stack layout (rsp=0 is last pushed):
             [rsp+ 0] = b_len
             [rsp+ 8] = b_ptr
             [rsp+16] = a_len
             [rsp+24] = a_ptr  */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);   /* rax=a_ptr, rdx=a_len */
        fprintf(cg->out,
            "    pushq %%rax\n"     /* a_ptr → [rsp+24] after all pushes */
            "    pushq %%rdx\n");   /* a_len → [rsp+16] */
        emit_init_expr(cg, TYPE_STRING, e->args[1]);   /* rax=b_ptr, rdx=b_len */
        fprintf(cg->out,
            "    pushq %%rax\n"     /* b_ptr → [rsp+8] */
            "    pushq %%rdx\n"     /* b_len → [rsp+0] */
            "    /* str.concat: total = a_len + b_len; alloc via brk */\n"
            "    movq 16(%%rsp), %%r13\n"   /* r13 = a_len */
            "    movq 0(%%rsp),  %%r14\n"   /* r14 = b_len */
            "    movq %%r13, %%r15\n"
            "    addq %%r14, %%r15\n"       /* r15 = total len */
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"       /* r12 = buf base */
            "    movq %%rax, %%rdi\n"
            "    addq %%r15, %%rdi\n"
            "    incq %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"                 /* extend heap */
            /* copy a into buf */
            "    movq %%r12, %%rdi\n"       /* dst = buf base */
            "    movq 24(%%rsp), %%rsi\n"   /* src = a_ptr */
            "    movq %%r13, %%rcx\n"       /* count = a_len */
            "    rep movsb\n"
            /* copy b into buf (rdi now points to a's end) */
            "    movq 8(%%rsp), %%rsi\n"    /* src = b_ptr */
            "    movq %%r14, %%rcx\n"       /* count = b_len */
            "    rep movsb\n"
            "    movq %%r12, %%rax\n"       /* return ptr */
            "    movq %%r15, %%rdx\n"       /* return len */
            "    addq $32, %%rsp\n");        /* pop 4 saved values */
        return;
    }

    /* str.contains(haystack, needle) → rax = 1 if found, 0 otherwise */
    if (strcmp(name, "std.str.contains") == 0 || strcmp(name, "str.contains") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] str.contains needs 2 args\n"); return; }
        int lbl = cg->lbl_counter++;
        /* Push hay (ptr, len), then load needle into r12/r13 directly.
           Stack after 2 pushes: [rsp+0]=hay_len, [rsp+8]=hay_ptr */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);   /* rax=hay_ptr, rdx=hay_len */
        fprintf(cg->out, "    pushq %%rax\n    pushq %%rdx\n");
        emit_init_expr(cg, TYPE_STRING, e->args[1]);   /* rax=needle_ptr, rdx=needle_len */
        fprintf(cg->out,
            "    movq %%rax, %%r12\n"    /* r12 = needle ptr */
            "    movq %%rdx, %%r13\n"    /* r13 = needle len */
            "    movq 8(%%rsp), %%r14\n" /* r14 = hay ptr */
            "    movq 0(%%rsp), %%r15\n" /* r15 = hay len */
            "    addq $16, %%rsp\n"
            "    xorq %%rbx, %%rbx\n"   /* rbx = i (outer pos) */
            ".Lcontains_outer%d:\n"
            /* while i <= hay_len - needle_len */
            "    movq %%r15, %%rax\n"
            "    subq %%r13, %%rax\n"    /* rax = hay_len - needle_len */
            "    cmpq %%rbx, %%rax\n"    /* jl if rax < rbx → stop */
            "    jl .Lcontains_no%d\n"
            /* inner: j from 0 to needle_len, comparing hay[i+j] vs needle[j] */
            "    xorq %%rcx, %%rcx\n"   /* rcx = j */
            ".Lcontains_inner%d:\n"
            "    cmpq %%r13, %%rcx\n"   /* if j >= needle_len → found */
            "    jge .Lcontains_yes%d\n"
            "    movq %%rbx, %%rax\n"
            "    addq %%rcx, %%rax\n"   /* rax = i+j */
            "    movb (%%r14,%%rax,1), %%al\n"  /* hay[i+j] */
            "    movb (%%r12,%%rcx,1), %%dl\n"  /* needle[j] */
            "    cmpb %%al, %%dl\n"
            "    jne .Lcontains_next%d\n"
            "    incq %%rcx\n"
            "    jmp .Lcontains_inner%d\n"
            ".Lcontains_next%d:\n"
            "    incq %%rbx\n"
            "    jmp .Lcontains_outer%d\n"
            ".Lcontains_yes%d:\n"
            "    movq $1, %%rax\n"
            "    jmp .Lcontains_end%d\n"
            ".Lcontains_no%d:\n"
            "    xorq %%rax, %%rax\n"
            ".Lcontains_end%d:\n",
            lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* str.slice(s, start, length) → rax=ptr+start, rdx=length  (zero-copy view) */
    if (strcmp(name, "std.str.slice") == 0 || strcmp(name, "str.slice") == 0) {
        if (e->argc < 3) { fprintf(stderr, "[codegen] str.slice needs 3 args: (s, start, len)\n"); return; }
        int lbl = cg->lbl_counter++;
        /* eval string → push ptr */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out, "    pushq %%rax\n");              /* [rsp+0] = ptr */
        emit_int_expr(cg, e->args[1]);
        fprintf(cg->out, "    pushq %%rax\n");              /* [rsp+0] = start, [rsp+8] = ptr */
        emit_int_expr(cg, e->args[2]);                      /* rax = slice_len */
        fprintf(cg->out,
            "    /* str.slice%d */\n"
            "    movq %%rax, %%rdx\n"          /* rdx = slice_len */
            "    movq 0(%%rsp), %%rcx\n"       /* rcx = start */
            "    movq 8(%%rsp), %%rax\n"       /* rax = ptr */
            "    addq %%rcx, %%rax\n"          /* rax = ptr + start */
            "    addq $16, %%rsp\n",
            lbl);
        return;
    }
    if (strcmp(name, "std.math.random") == 0) {
        if (e->argc < 1) {
            fprintf(stderr, "[codegen] math.random needs 1 arg: (seed)\n");
            return;
        }
        fprintf(cg->out, "    /* math.random(seed) */\n");
        emit_int_expr(cg, e->args[0]);          /* seed → rax */
        fprintf(cg->out, "    callq __silica_random\n");
        /* result in rax — caller stores if needed */
        return;
    }

    /* ── std.math.sqrt(n) ── integer sqrt, result in rax ── */
    if (strcmp(name, "std.math.sqrt") == 0) {
        if (e->argc < 1) {
            fprintf(stderr, "[codegen] math.sqrt needs 1 arg: (n)\n");
            return;
        }
        fprintf(cg->out, "    /* math.sqrt(n) */\n");
        emit_int_expr(cg, e->args[0]);          /* n → rax */
        fprintf(cg->out, "    callq __silica_sqrt\n");
        return;
    }

    /* ── std.math.root(root, n) ── integer nth root, result in rax ── */
    if (strcmp(name, "std.math.root") == 0) {
        if (e->argc < 2) {
            fprintf(stderr, "[codegen] math.root needs 2 args: (root, n)\n");
            return;
        }
        fprintf(cg->out, "    /* math.root(root, n) */\n");
        /* pass root in rdi, n in rax */
        emit_int_expr(cg, e->args[0]);          /* root → rax */
        fprintf(cg->out, "    movq %%rax, %%rdi\n");
        emit_int_expr(cg, e->args[1]);          /* n    → rax */
        fprintf(cg->out, "    callq __silica_root\n");
        return;
    }

    /* ── std.math.sin(degrees) ── sin of angle in degrees → xmm0 (float) ── */
    if (strcmp(name, "std.math.sin") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] math.sin needs 1 arg\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* math.sin(degrees): deg*pi/180 via x87, fsin → xmm0 */\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    fildll -8(%%rsp)\n"            /* ST(0) = degrees           */
            "    fldpi\n"                       /* ST(0)=pi   ST(1)=degrees  */
            "    fmulp\n"                       /* ST(0) = degrees*pi        */
            "    movq $180, %%rax\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    fildll -8(%%rsp)\n"            /* ST(0)=180  ST(1)=deg*pi   */
            "    fdivrp\n"                      /* ST(0) = ST(1)/ST(0) = deg*pi/180 = radians */
            "    fsin\n"
            "    fstpl -8(%%rsp)\n"
            "    movsd -8(%%rsp), %%xmm0\n");
        return;
    }

    /* ── std.math.cos(degrees) ── cos of angle in degrees → xmm0 (float) ── */
    if (strcmp(name, "std.math.cos") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] math.cos needs 1 arg\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* math.cos(degrees): deg*pi/180 via x87, fcos → xmm0 */\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    fildll -8(%%rsp)\n"
            "    fldpi\n"
            "    fmulp\n"
            "    movq $180, %%rax\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    fildll -8(%%rsp)\n"
            "    fdivrp\n"                      /* radians = degrees*pi/180  */
            "    fcos\n"
            "    fstpl -8(%%rsp)\n"
            "    movsd -8(%%rsp), %%xmm0\n");
        return;
    }

    /* ── std.math.log(n) ── natural log ln(n) → xmm0 (float) ── */
    if (strcmp(name, "std.math.log") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] math.log needs 1 arg\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* math.log(n): ln(n) = log2(n) * ln2 via x87 fyl2x */\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    fildll -8(%%rsp)\n"            /* ST(0) = n                 */
            "    fldln2\n"                      /* ST(0)=ln2   ST(1)=n       */
            "    fxch\n"                        /* ST(0)=n     ST(1)=ln2     */
            "    fyl2x\n"                       /* ST(0) = ln2 * log2(n) = ln(n) */
            "    fstpl -8(%%rsp)\n"
            "    movsd -8(%%rsp), %%xmm0\n");
        return;
    }

    /* ── std.math.integral(a, b) ── definite integral ∫[a..b] x dx = (b²-a²)/2 → xmm0 ── */
    if (strcmp(name, "std.math.integral") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] math.integral needs 2 args: (a, b)\n"); return; }
        /* SSE2: xmm0 = (double)b, xmm1 = (double)a
         * result = (b*b - a*a) / 2.0  which equals ∫[a..b] x dx */
        emit_int_expr(cg, e->args[1]);               /* b → rax */
        fprintf(cg->out,
            "    /* math.integral(a,b) = (b²-a²)/2 = ∫[a..b] x dx */\n"
            "    cvtsi2sdq %%rax, %%xmm0\n"          /* xmm0 = (double)b */
            "    movsd %%xmm0, %%xmm2\n"
            "    mulsd %%xmm2, %%xmm0\n");            /* xmm0 = b*b */
        emit_int_expr(cg, e->args[0]);               /* a → rax */
        fprintf(cg->out,
            "    cvtsi2sdq %%rax, %%xmm1\n"          /* xmm1 = (double)a */
            "    movsd %%xmm1, %%xmm3\n"
            "    mulsd %%xmm3, %%xmm1\n"             /* xmm1 = a*a */
            "    subsd %%xmm1, %%xmm0\n"             /* xmm0 = b*b - a*a */
            "    movq $2, %%rax\n"
            "    cvtsi2sdq %%rax, %%xmm1\n"          /* xmm1 = 2.0 */
            "    divsd %%xmm1, %%xmm0\n");            /* xmm0 = (b²-a²)/2 */
        return;
    }

    /* ── std.math.sigma(start, end) ── Σ(i=start..end) i = (end-start+1)*(start+end)/2 → rax ── */
    if (strcmp(name, "std.math.sigma") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] math.sigma needs 2 args: (start, end)\n"); return; }
        /* Gauss formula: sum = n*(a1+an)/2 where n = end-start+1 */
        emit_int_expr(cg, e->args[1]);          /* end → rax */
        fprintf(cg->out,
            "    /* math.sigma(start,end): Gauss sum */\n"
            "    pushq %%rax\n");               /* save end */
        emit_int_expr(cg, e->args[0]);          /* start → rax */
        fprintf(cg->out,
            "    pushq %%rax\n"                 /* save start */
            "    movq 8(%%rsp), %%rcx\n"        /* rcx = end */
            "    movq 0(%%rsp), %%rbx\n"        /* rbx = start */
            "    /* n = end - start + 1 */\n"
            "    movq %%rcx, %%rax\n"
            "    subq %%rbx, %%rax\n"
            "    incq %%rax\n"                  /* rax = n */
            "    /* sum = n * (start + end) / 2 */\n"
            "    movq %%rbx, %%rdx\n"
            "    addq %%rcx, %%rdx\n"           /* rdx = start + end */
            "    imulq %%rdx, %%rax\n"          /* rax = n * (start+end) */
            "    sarq $1, %%rax\n"              /* rax /= 2 */
            "    addq $16, %%rsp\n");           /* pop start, end */
        return;
    }
    if (strcmp(name, "std.math.pwr") == 0) {
        if (e->argc < 2) {
            fprintf(stderr, "[codegen] math.pwr needs 2 args: (exp, base)\n");
            return;
        }
        fprintf(cg->out, "    /* math.pwr(exp, base) */\n");
        /* pass exp in rdi, base in rax */
        emit_int_expr(cg, e->args[0]);          /* exp  → rax */
        fprintf(cg->out, "    movq %%rax, %%rdi\n");
        emit_int_expr(cg, e->args[1]);          /* base → rax */
        fprintf(cg->out, "    callq __silica_pwr\n");
        return;
    }

    /* ── std.fs.create(path) ── create file or directory ── */
    if (strcmp(name, "std.fs.create") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] fs.create needs 1 arg\n"); return; }
        int lbl = cg->lbl_counter++;

        /* load path string → rax=ptr, rdx=len */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* fs.create: null-terminate path on stack scratch */\n"
            "    movq %%rax, %%rsi\n"        /* rsi = src ptr */
            "    movq %%rdx, %%rcx\n"        /* rcx = len     */
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"        /* rdi = dst     */
            "    pushq %%rdi\n"              /* save dst ptr  */
            "    rep movsb\n"               /* copy bytes    */
            "    movb $0, (%%rdi)\n"         /* null term     */
            "    popq %%rdi\n"              /* rdi = path ptr */
            /* check if last char is '/' → mkdir, else creat */
            "    movq %%rdx, %%rcx\n"        /* rcx = len     */
            "    testq %%rcx, %%rcx\n"
            "    jz .Lfscreate_file%d\n"
            "    movb -1(%%rdi,%%rcx,1), %%al\n"
            "    cmpb $47, %%al\n"           /* '/' = 0x2F   */
            "    jne .Lfscreate_file%d\n"
            /* mkdir(path, 0755) */
            "    movq $83, %%rax\n"          /* sys_mkdir     */
            "    movq $0755, %%rsi\n"
            "    syscall\n"
            "    jmp .Lfscreate_done%d\n"
            ".Lfscreate_file%d:\n"
            /* open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644) = creat */
            "    movq $2, %%rax\n"           /* sys_open      */
            "    movq $0101, %%rsi\n"        /* O_WRONLY|O_CREAT = 0101 */
            "    orq  $01000, %%rsi\n"       /* |O_TRUNC      */
            "    movq $0644, %%rdx\n"        /* mode          */
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    js .Lfscreate_done%d\n"
            "    movq %%rax, %%rdi\n"
            "    movq $3, %%rax\n"           /* sys_close     */
            "    syscall\n"
            ".Lfscreate_done%d:\n"
            "    addq $4096, %%rsp\n",
            lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* ── std.fs.append(path, content) ── append string to file ── */
    if (strcmp(name, "std.fs.append") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] fs.append needs 2 args\n"); return; }
        int lbl = cg->lbl_counter++;

        /* evaluate content arg first (save ptr+len on stack) */
        emit_init_expr(cg, TYPE_STRING, e->args[1]);
        fprintf(cg->out,
            "    /* fs.append: save content ptr+len */\n"
            "    pushq %%rax\n"   /* content ptr */
            "    pushq %%rdx\n"); /* content len */

        /* evaluate path, null-terminate */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    movq %%rax, %%rsi\n"
            "    movq %%rdx, %%rcx\n"
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"
            "    pushq %%rdi\n"
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"
            "    popq %%rdi\n"
            /* open(path, O_WRONLY|O_CREAT|O_APPEND, 0644) */
            "    movq $2, %%rax\n"           /* sys_open */
            "    movq $01, %%rsi\n"          /* O_WRONLY */
            "    orq  $0100, %%rsi\n"        /* |O_CREAT */
            "    orq  $02000, %%rsi\n"       /* |O_APPEND */
            "    movq $0644, %%rdx\n"
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    js .Lfsappend_done%d\n"
            "    movq %%rax, %%r15\n"        /* r15 = fd */
            "    addq $4096, %%rsp\n"
            /* write(fd, ptr, len) */
            "    movq %%r15, %%rdi\n"
            "    movq 8(%%rsp), %%rsi\n"     /* content ptr (pushed earlier) */
            "    movq 0(%%rsp), %%rdx\n"     /* content len */
            "    movq $1, %%rax\n"           /* sys_write */
            "    syscall\n"
            /* close(fd) */
            "    movq %%r15, %%rdi\n"
            "    movq $3, %%rax\n"           /* sys_close */
            "    syscall\n"
            "    addq $16, %%rsp\n"          /* pop content ptr+len */
            "    jmp .Lfsappend_end%d\n"
            ".Lfsappend_done%d:\n"
            "    addq $4096, %%rsp\n"
            "    addq $16, %%rsp\n"
            ".Lfsappend_end%d:\n",
            lbl, lbl, lbl, lbl);
        return;
    }

    /* ── std.fs.delete(path) ── unlink file ── */
    if (strcmp(name, "std.fs.delete") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] fs.delete needs 1 arg\n"); return; }

        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* fs.delete: null-terminate then unlink */\n"
            "    movq %%rax, %%rsi\n"
            "    movq %%rdx, %%rcx\n"
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"
            "    pushq %%rdi\n"
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"
            "    popq %%rdi\n"
            "    movq $87, %%rax\n"          /* sys_unlink */
            "    syscall\n"
            "    addq $4096, %%rsp\n");
        return;
    }

    /* ── std.fs.open(path, mode) → int fd ──
     *   mode: 0=read, 1=write(trunc), 2=write(append)
     *   Returns fd on success, -1 on failure.
     */
    if (strcmp(name, "std.fs.open") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] fs.open needs 2 args: (path, mode)\n"); return; }
        int lbl = cg->lbl_counter++;
        /* evaluate mode → r13 */
        emit_int_expr(cg, e->args[1]);
        fprintf(cg->out, "    movq %%rax, %%r13\n"); /* r13 = mode */
        /* evaluate path, null-terminate */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* fs.open: null-terminate path */\n"
            "    movq %%rax, %%rsi\n"
            "    movq %%rdx, %%rcx\n"
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"
            "    pushq %%rdi\n"
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"
            "    popq %%rdi\n"                  /* rdi = path */
            /* select flags based on mode */
            "    testq %%r13, %%r13\n"
            "    jnz .Lfsopen_write%d\n"
            /* mode 0: O_RDONLY */
            "    movq $2, %%rax\n"              /* sys_open */
            "    xorq %%rsi, %%rsi\n"           /* O_RDONLY=0 */
            "    xorq %%rdx, %%rdx\n"
            "    syscall\n"
            "    jmp .Lfsopen_done%d\n"
            ".Lfsopen_write%d:\n"
            "    cmpq $2, %%r13\n"
            "    je .Lfsopen_append%d\n"
            /* mode 1: O_WRONLY|O_CREAT|O_TRUNC */
            "    movq $2, %%rax\n"
            "    movq $0101, %%rsi\n"
            "    orq  $01000, %%rsi\n"
            "    movq $0644, %%rdx\n"
            "    syscall\n"
            "    jmp .Lfsopen_done%d\n"
            ".Lfsopen_append%d:\n"
            /* mode 2: O_WRONLY|O_CREAT|O_APPEND */
            "    movq $2, %%rax\n"
            "    movq $01, %%rsi\n"
            "    orq  $0100, %%rsi\n"
            "    orq  $02000, %%rsi\n"
            "    movq $0644, %%rdx\n"
            "    syscall\n"
            ".Lfsopen_done%d:\n"
            "    addq $4096, %%rsp\n"
            "    /* fd in rax, negative = error */\n",
            lbl, lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* ── std.fs.close(fd) ── */
    if (strcmp(name, "std.fs.close") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] fs.close needs 1 arg: (fd)\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* fs.close(fd) */\n"
            "    movq %%rax, %%rdi\n"
            "    movq $3, %%rax\n"              /* sys_close */
            "    syscall\n");
        return;
    }

    /* ── std.fs.write(fd, content) → int bytes_written ── */
    if (strcmp(name, "std.fs.write") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] fs.write needs 2 args: (fd, content)\n"); return; }
        /* evaluate content first */
        emit_init_expr(cg, TYPE_STRING, e->args[1]);
        fprintf(cg->out,
            "    /* fs.write: save content ptr+len, load fd */\n"
            "    pushq %%rax\n"                 /* content ptr */
            "    pushq %%rdx\n");               /* content len */
        emit_int_expr(cg, e->args[0]);          /* rax = fd */
        fprintf(cg->out,
            "    movq %%rax, %%rdi\n"           /* rdi = fd */
            "    movq 8(%%rsp), %%rsi\n"        /* rsi = ptr */
            "    movq 0(%%rsp), %%rdx\n"        /* rdx = len */
            "    movq $1, %%rax\n"              /* sys_write */
            "    syscall\n"
            "    addq $16, %%rsp\n"
            "    /* rax = bytes written */\n");
        return;
    }

    /* ── std.fs.read_all(fd) → string ── reads entire file into heap buffer ── */
    if (strcmp(name, "std.fs.read_all") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] fs.read_all needs 1 arg: (fd)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_int_expr(cg, e->args[0]);          /* rax = fd */
        fprintf(cg->out,
            "    /* fs.read_all(fd): heap buffer, read 4096 bytes at a time */\n"
            "    movq %%rax, %%r15\n"           /* r15 = fd */
            /* allocate 65536-byte initial buffer via brk */
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"           /* r12 = buf base */
            "    movq %%rax, %%rdi\n"
            "    addq $65536, %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            "    xorq %%r13, %%r13\n"           /* r13 = total bytes read */
            /* read loop */
            ".Lfsreadall_loop%d:\n"
            "    movq %%r15, %%rdi\n"           /* fd */
            "    movq %%r12, %%rsi\n"
            "    addq %%r13, %%rsi\n"           /* buf + offset */
            "    movq $4096, %%rdx\n"
            "    movq $0, %%rax\n"              /* sys_read */
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    jle .Lfsreadall_done%d\n"
            "    addq %%rax, %%r13\n"
            "    jmp .Lfsreadall_loop%d\n"
            ".Lfsreadall_done%d:\n"
            "    movq %%r12, %%rax\n"           /* rax = ptr */
            "    movq %%r13, %%rdx\n",          /* rdx = total len */
            lbl, lbl, lbl, lbl);
        return;
    }

    /* ── std.fs.size(path) → int bytes ── */
    if (strcmp(name, "std.fs.size") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] fs.size needs 1 arg: (path)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* fs.size: stat(path) → st_size */\n"
            "    movq %%rax, %%rsi\n"
            "    movq %%rdx, %%rcx\n"
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"
            "    pushq %%rdi\n"
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"
            "    popq %%rdi\n"                  /* rdi = null-term path */
            /* allocate stat buffer (144 bytes on x86-64) */
            "    subq $152, %%rsp\n"
            "    movq %%rsp, %%rsi\n"           /* rsi = &statbuf */
            "    movq $4, %%rax\n"              /* sys_stat */
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    js .Lfssize_fail%d\n"
            "    movq 48(%%rsp), %%rax\n"       /* st_size offset = 48 in struct stat */
            "    jmp .Lfssize_done%d\n"
            ".Lfssize_fail%d:\n"
            "    movq $-1, %%rax\n"
            ".Lfssize_done%d:\n"
            "    addq $152, %%rsp\n"
            "    addq $4096, %%rsp\n",
            lbl, lbl, lbl, lbl);
        return;
    }
    if (strcmp(name, "std.fs.read") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] fs.read needs 2 args: (path, line)\n"); return; }
        int lbl = cg->lbl_counter++;

        /* Step 1: evaluate line number → r13 (callee-saved, safe across syscalls) */
        emit_int_expr(cg, e->args[1]);
        fprintf(cg->out,
            "    movq %%rax, %%r13\n"       /* r13 = target line# (1-indexed) */
        );

        /* Step 2: evaluate path string, null-terminate to stack scratch */
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* fs.read: null-terminate path */\n"
            "    movq %%rax, %%rsi\n"        /* rsi = path ptr */
            "    movq %%rdx, %%rcx\n"        /* rcx = path len */
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"
            "    pushq %%rdi\n"              /* save scratch ptr */
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"
            "    popq %%rdi\n"               /* rdi = null-term path */
            /* Step 3: open(path, O_RDONLY=0, 0) */
            "    movq $2, %%rax\n"
            "    xorq %%rsi, %%rsi\n"
            "    xorq %%rdx, %%rdx\n"
            "    syscall\n"
            "    addq $4096, %%rsp\n"        /* free path scratch */
            "    testq %%rax, %%rax\n"
            "    js .Lfsread_fail%d\n"
            "    movq %%rax, %%r15\n"        /* r15 = fd */
            /* Step 4: allocate 4096-byte output buffer via brk */
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"        /* r12 = buf base */
            "    movq %%rax, %%rdi\n"
            "    addq $4096, %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            /* r13=target line, r14=current line, rbx=output index */
            "    movq $1, %%r14\n"           /* start at line 1 */
            "    xorq %%rbx, %%rbx\n"        /* output index = 0 */
            /* Step 5: byte-by-byte read loop */
            ".Lfsread_loop%d:\n"
            "    leaq -8(%%rsp), %%rsi\n"    /* 1-byte scratch in red zone */
            "    movq %%r15, %%rdi\n"
            "    xorq %%rax, %%rax\n"        /* sys_read */
            "    movq $1, %%rdx\n"
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    jle .Lfsread_eof%d\n"       /* 0=EOF, <0=error */
            "    movb -8(%%rsp), %%al\n"
            "    cmpb $10, %%al\n"           /* newline? */
            "    jne .Lfsread_notNL%d\n"
            /* newline: if we're on the target line, done */
            "    cmpq %%r13, %%r14\n"
            "    je  .Lfsread_eof%d\n"
            "    incq %%r14\n"               /* advance line counter */
            "    xorq %%rbx, %%rbx\n"        /* reset output index */
            "    jmp .Lfsread_loop%d\n"
            ".Lfsread_notNL%d:\n"
            "    cmpq %%r13, %%r14\n"
            "    jne .Lfsread_loop%d\n"      /* not our line yet */
            "    movb %%al, (%%r12,%%rbx,1)\n"  /* store byte */
            "    incq %%rbx\n"
            "    cmpq $4095, %%rbx\n"
            "    jl  .Lfsread_loop%d\n"
            ".Lfsread_eof%d:\n"
            "    movq %%r15, %%rdi\n"
            "    movq $3, %%rax\n"           /* sys_close */
            "    syscall\n"
            "    movq %%r12, %%rax\n"        /* return: ptr in rax */
            "    movq %%rbx, %%rdx\n"        /* return: len in rdx */
            "    jmp .Lfsread_end%d\n"
            ".Lfsread_fail%d:\n"
            "    xorq %%rax, %%rax\n"
            "    xorq %%rdx, %%rdx\n"
            ".Lfsread_end%d:\n",
            lbl,           /* js .fail */
            lbl,           /* .loop    */
            lbl,           /* jle .eof */
            lbl,           /* jne .notNL */
            lbl,           /* je .eof  */
            lbl,           /* jmp .loop */
            lbl,           /* .notNL   */
            lbl,           /* jne .loop */
            lbl,           /* jl .loop  */
            lbl,           /* .eof      */
            lbl,           /* jmp .end  */
            lbl,           /* .fail     */
            lbl);          /* .end      */
        return;
    }
    /* ════════════════════════════════════════════════════════════════
     * std.time
     * ════════════════════════════════════════════════════════════════ */

    /* time.now() → rax = seconds since epoch (CLOCK_REALTIME) */
    if (strcmp(name, "std.time.now") == 0) {
        int lbl = cg->lbl_counter++;
        fprintf(cg->out,
            "    /* time.now(): sys_clock_gettime(CLOCK_REALTIME, buf) → seconds */\n"
            "    subq $16, %%rsp\n"           /* struct timespec: tv_sec(8) + tv_nsec(8) */
            "    movq $228, %%rax\n"          /* sys_clock_gettime */
            "    xorq %%rdi, %%rdi\n"         /* CLOCK_REALTIME = 0 */
            "    movq %%rsp, %%rsi\n"         /* &timespec */
            "    syscall\n"
            "    movq 0(%%rsp), %%rax\n"      /* tv_sec → rax */
            "    addq $16, %%rsp\n"
            "    /* time.now done */\n");
        return;
    }

    /* time.now_ms() → rax = milliseconds since epoch */
    if (strcmp(name, "std.time.now_ms") == 0) {
        fprintf(cg->out,
            "    /* time.now_ms(): ms since epoch */\n"
            "    subq $16, %%rsp\n"
            "    movq $228, %%rax\n"          /* sys_clock_gettime */
            "    xorq %%rdi, %%rdi\n"         /* CLOCK_REALTIME */
            "    movq %%rsp, %%rsi\n"
            "    syscall\n"
            "    movq 0(%%rsp), %%rbx\n"      /* rbx = tv_sec */
            "    imulq $1000, %%rbx\n"        /* rbx = sec * 1000 */
            "    movq 8(%%rsp), %%rax\n"      /* rax = tv_nsec */
            "    movq $1000000, %%rcx\n"
            "    xorq %%rdx, %%rdx\n"         /* MUST zero rdx before divq */
            "    divq %%rcx\n"                /* rax = nsec / 1000000 = ms */
            "    addq %%rbx, %%rax\n"         /* rax = sec*1000 + nsec_ms */
            "    addq $16, %%rsp\n");
        return;
    }

    /* time.sleep(ms) — sleep for N milliseconds via nanosleep */
    if (strcmp(name, "std.time.sleep") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] time.sleep needs 1 arg: (ms)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_int_expr(cg, e->args[0]);       /* rax = ms */
        fprintf(cg->out,
            "    /* time.sleep(ms): nanosleep({sec=ms/1000, nsec=(ms%%1000)*1000000}) */\n"
            "    subq $32, %%rsp\n"           /* space for two timespec structs */
            "    movq %%rax, %%rbx\n"         /* rbx = ms */
            "    movq %%rbx, %%rax\n"
            "    xorq %%rdx, %%rdx\n"
            "    movq $1000, %%rcx\n"
            "    divq %%rcx\n"                /* rax = ms/1000 = sec, rdx = ms%%1000 */
            "    movq %%rax, 0(%%rsp)\n"      /* tv_sec */
            "    imulq $1000000, %%rdx\n"     /* rdx = (ms%%1000)*1e6 = nsec */
            "    movq %%rdx, 8(%%rsp)\n"      /* tv_nsec */
            "    movq $35, %%rax\n"           /* sys_nanosleep */
            "    movq %%rsp, %%rdi\n"         /* req = &timespec */
            "    xorq %%rsi, %%rsi\n"         /* rem = NULL */
            "    syscall\n"
            "    addq $32, %%rsp\n"
            "    /* time.sleep done */\n");
        return;
    }

    /* time.mono() → rax = monotonic seconds (CLOCK_MONOTONIC, good for timing) */
    if (strcmp(name, "std.time.mono") == 0) {
        int lbl = cg->lbl_counter++;
        fprintf(cg->out,
            "    /* time.mono(): CLOCK_MONOTONIC seconds */\n"
            "    subq $16, %%rsp\n"
            "    movq $228, %%rax\n"
            "    movq $1, %%rdi\n"            /* CLOCK_MONOTONIC = 1 */
            "    movq %%rsp, %%rsi\n"
            "    syscall\n"
            "    movq 0(%%rsp), %%rax\n"
            "    addq $16, %%rsp\n"
            "    /* time.mono done */\n");
        return;
    }

    /* ════════════════════════════════════════════════════════════════
     * std.net  — pure syscall TCP sockets, AF_INET (IPv4)
     * ════════════════════════════════════════════════════════════════ */

    /* net.connect(ip_int, port) → rax = fd (or negative on error)
     * ip_int is the 32-bit IPv4 address as an integer (host byte order).
     * Converts to network byte order internally.
     * Usage: int fd = net.connect(0x7f000001, 8080);  // 127.0.0.1:8080 */
    if (strcmp(name, "std.net.connect") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] net.connect needs 2 args: (ip, port)\n"); return; }
        int lbl = cg->lbl_counter++;
        /* Step 1: socket(AF_INET=2, SOCK_STREAM=1, 0) */
        fprintf(cg->out,
            "    /* net.connect(ip, port) */\n"
            "    movq $41, %%rax\n"           /* sys_socket */
            "    movq $2,  %%rdi\n"           /* AF_INET */
            "    movq $1,  %%rsi\n"           /* SOCK_STREAM */
            "    xorq %%rdx, %%rdx\n"
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    js .Lnet_conn_fail%d\n"
            "    movq %%rax, %%r15\n",        /* r15 = fd */
            lbl);
        /* Step 2: build sockaddr_in on stack
           struct sockaddr_in: sin_family(2) sin_port(2) sin_addr(4) sin_zero(8) = 16 bytes */
        emit_int_expr(cg, e->args[1]);        /* port → rax */
        fprintf(cg->out,
            "    /* bswap port for network byte order */\n"
            "    movq %%rax, %%rbx\n"
            "    rolw $8, %%bx\n"             /* bswap 16-bit port */
            "    subq $16, %%rsp\n"           /* sockaddr_in */
            "    movq $0, 0(%%rsp)\n"         /* zero all 16 bytes */
            "    movq $0, 8(%%rsp)\n"
            "    movw $2, 0(%%rsp)\n"         /* sin_family = AF_INET = 2 */
            "    movw %%bx, 2(%%rsp)\n");     /* sin_port (network order) */
        emit_int_expr(cg, e->args[0]);        /* ip → rax */
        fprintf(cg->out,
            "    /* bswap ip for network byte order */\n"
            "    bswapl %%eax\n"
            "    movl %%eax, 4(%%rsp)\n"      /* sin_addr */
            /* Step 3: connect(fd, &sockaddr, 16) */
            "    movq $42, %%rax\n"           /* sys_connect */
            "    movq %%r15, %%rdi\n"         /* fd */
            "    movq %%rsp, %%rsi\n"         /* &sockaddr_in */
            "    movq $16, %%rdx\n"           /* sizeof(sockaddr_in) */
            "    syscall\n"
            "    addq $16, %%rsp\n"           /* pop sockaddr */
            "    testq %%rax, %%rax\n"
            "    js .Lnet_conn_fail%d\n"
            "    movq %%r15, %%rax\n"         /* return fd */
            "    jmp .Lnet_conn_end%d\n"
            ".Lnet_conn_fail%d:\n"
            "    movq $-1, %%rax\n"
            ".Lnet_conn_end%d:\n",
            lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* net.send(fd, data_ptr, len) → rax = bytes sent */
    if (strcmp(name, "std.net.send") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] net.send needs 2 args: (fd, string)\n"); return; }
        /* args[0]=fd, args[1]=string */
        emit_init_expr(cg, TYPE_STRING, e->args[1]); /* rax=ptr, rdx=len */
        fprintf(cg->out,
            "    pushq %%rax\n"               /* save ptr */
            "    pushq %%rdx\n");             /* save len */
        emit_int_expr(cg, e->args[0]);        /* fd → rax */
        fprintf(cg->out,
            "    /* net.send(fd, str) → sys_sendto */\n"
            "    movq %%rax, %%rdi\n"         /* fd */
            "    movq 8(%%rsp), %%rsi\n"      /* ptr */
            "    movq 0(%%rsp), %%rdx\n"      /* len */
            "    xorq %%r10, %%r10\n"         /* flags = 0 */
            "    xorq %%r8, %%r8\n"           /* dest_addr = NULL */
            "    xorq %%r9, %%r9\n"           /* addrlen = 0 */
            "    movq $44, %%rax\n"           /* sys_sendto */
            "    syscall\n"
            "    addq $16, %%rsp\n");
        return;
    }

    /* net.recv(fd, buf_size) → rax=ptr rdx=len (heap-allocated buffer) */
    if (strcmp(name, "std.net.recv") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] net.recv needs 2 args: (fd, maxlen)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_int_expr(cg, e->args[1]);        /* maxlen → rax */
        fprintf(cg->out,
            "    /* net.recv(fd, maxlen): alloc buffer, sys_recvfrom */\n"
            "    pushq %%rax\n"               /* save maxlen */
            "    /* alloc maxlen bytes via brk */\n"
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"         /* r12 = buf base */
            "    movq 0(%%rsp), %%rdi\n"      /* maxlen */
            "    addq %%rax, %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"                   /* extend heap */
            "    popq %%rcx\n"               /* rcx = maxlen */
        );
        emit_int_expr(cg, e->args[0]);        /* fd → rax */
        fprintf(cg->out,
            "    movq %%rax, %%rdi\n"         /* fd */
            "    movq %%r12, %%rsi\n"         /* buf */
            "    movq %%rcx, %%rdx\n"         /* maxlen */
            "    xorq %%r10, %%r10\n"         /* flags = 0 */
            "    xorq %%r8, %%r8\n"
            "    xorq %%r9, %%r9\n"
            "    movq $45, %%rax\n"           /* sys_recvfrom */
            "    syscall\n"
            "    testq %%rax, %%rax\n"
            "    js .Lnet_recv_fail%d\n"
            "    movq %%r12, %%rax\n"         /* ptr */
            "    /* rdx already = bytes received (from syscall) — no, rax=bytes */\n"
            "    movq %%rax, %%rdx\n"         /* rdx = len = bytes received */
            "    movq %%r12, %%rax\n"         /* rax = ptr */
            "    jmp .Lnet_recv_end%d\n"
            ".Lnet_recv_fail%d:\n"
            "    xorq %%rax, %%rax\n"
            "    xorq %%rdx, %%rdx\n"
            ".Lnet_recv_end%d:\n",
            lbl, lbl, lbl, lbl);
        return;
    }

    /* net.close(fd) — close a socket */
    if (strcmp(name, "std.net.close") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] net.close needs 1 arg: (fd)\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* net.close(fd) */\n"
            "    movq %%rax, %%rdi\n"
            "    movq $3, %%rax\n"            /* sys_close */
            "    syscall\n");
        return;
    }

    /* net.ip(a, b, c, d) → rax = IPv4 as packed int (host order) e.g. net.ip(127,0,0,1) */
    if (strcmp(name, "std.net.ip") == 0) {
        if (e->argc < 4) { fprintf(stderr, "[codegen] net.ip needs 4 args: (a,b,c,d)\n"); return; }
        emit_int_expr(cg, e->args[0]);        /* a */
        fprintf(cg->out, "    pushq %%rax\n");
        emit_int_expr(cg, e->args[1]);        /* b */
        fprintf(cg->out, "    pushq %%rax\n");
        emit_int_expr(cg, e->args[2]);        /* c */
        fprintf(cg->out, "    pushq %%rax\n");
        emit_int_expr(cg, e->args[3]);        /* d */
        fprintf(cg->out,
            "    /* net.ip(a,b,c,d): pack into 32-bit int */\n"
            "    movq %%rax, %%rbx\n"          /* rbx = d */
            "    movq 0(%%rsp), %%rax\n"       /* c */
            "    shlq $8, %%rax\n"
            "    orq %%rbx, %%rax\n"           /* c<<8 | d */
            "    movq 8(%%rsp), %%rbx\n"       /* b */
            "    shlq $16, %%rbx\n"
            "    orq %%rbx, %%rax\n"           /* b<<16 | c<<8 | d */
            "    movq 16(%%rsp), %%rbx\n"      /* a */
            "    shlq $24, %%rbx\n"
            "    orq %%rbx, %%rax\n"           /* a<<24 | b<<16 | c<<8 | d */
            "    addq $24, %%rsp\n");
        return;
    }

    /* ════════════════════════════════════════════════════════════════
     * std.proc
     * ════════════════════════════════════════════════════════════════ */

    /* proc.exit(code) — immediately exit with given code */
    if (strcmp(name, "std.proc.exit") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] proc.exit needs 1 arg: (code)\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* proc.exit(code) */\n"
            "    movq %%rax, %%rdi\n"
            "    movq $60, %%rax\n"            /* sys_exit */
            "    syscall\n");
        return;
    }

    /* proc.fork() → rax = 0 in child, child_pid in parent, -1 on error */
    if (strcmp(name, "std.proc.fork") == 0) {
        fprintf(cg->out,
            "    /* proc.fork() */\n"
            "    movq $57, %%rax\n"            /* sys_fork */
            "    syscall\n");
        return;
    }

    /* proc.exec(path) — execve(path, [path, NULL], environ)
     * Replaces current process. Does not return on success. */
    if (strcmp(name, "std.proc.exec") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] proc.exec needs 1 arg: (path)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr, rdx=len */
        fprintf(cg->out,
            "    /* proc.exec(path): null-terminate, build argv[], execve */\n"
            "    movq %%rax, %%rsi\n"          /* src ptr */
            "    movq %%rdx, %%rcx\n"          /* len */
            "    subq $4096, %%rsp\n"
            "    movq %%rsp, %%rdi\n"          /* dst = path buf */
            "    pushq %%rdi\n"
            "    rep movsb\n"
            "    movb $0, (%%rdi)\n"           /* null terminate */
            "    popq %%r12\n"                 /* r12 = path ptr */
            /* build argv = [path, NULL] on stack */
            "    subq $16, %%rsp\n"
            "    movq %%r12, 0(%%rsp)\n"       /* argv[0] = path */
            "    movq $0, 8(%%rsp)\n"          /* argv[1] = NULL */
            "    movq $59, %%rax\n"            /* sys_execve */
            "    movq %%r12, %%rdi\n"          /* filename */
            "    movq %%rsp, %%rsi\n"          /* argv */
            "    movq __silica_envp(%%rip), %%rdx\n" /* envp from startup */
            "    syscall\n"
            "    addq $16, %%rsp\n"
            "    addq $4096, %%rsp\n"
            "    /* proc.exec%d: rax = -errno on failure */\n", lbl);
        return;
    }

    /* proc.pid() → rax = current process id */
    if (strcmp(name, "std.proc.pid") == 0) {
        fprintf(cg->out,
            "    /* proc.pid() */\n"
            "    movq $39, %%rax\n"            /* sys_getpid */
            "    syscall\n");
        return;
    }

    /* proc.wait() → rax = exit status of any child (sys_wait4(-1, &status, 0, NULL)) */
    if (strcmp(name, "std.proc.wait") == 0) {
        fprintf(cg->out,
            "    /* proc.wait(): wait4(-1, &status, 0, NULL) */\n"
            "    subq $8, %%rsp\n"
            "    movq $61, %%rax\n"            /* sys_wait4 */
            "    movq $-1, %%rdi\n"            /* any child */
            "    movq %%rsp, %%rsi\n"          /* &status */
            "    xorq %%rdx, %%rdx\n"          /* options = 0 */
            "    xorq %%r10, %%r10\n"          /* rusage = NULL */
            "    syscall\n"
            "    movq 0(%%rsp), %%rdx\n"       /* rdx = raw wait status */
            "    addq $8, %%rsp\n"
            "    /* rax = child pid, rdx = status */\n");
        return;
    }

    /* ════════════════════════════════════════════════════════════════
     * std.str  (renamed from bare str.* — old names still work)
     * ════════════════════════════════════════════════════════════════ */

    /* str.from_int(n) → rax=ptr rdx=len  (heap string of integer) */
    if (strcmp(name, "std.str.from_int") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.from_int needs 1 arg\n"); return; }
        emit_int_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* str.from_int(n): convert int to heap string */\n"
            "    callq __silica_itoa\n"        /* rax=ptr rdx=len */
        );
        return;
    }

    /* str.from_float(f) → rax=ptr rdx=len  (heap string "%.6f"-style) */
    if (strcmp(name, "std.str.from_float") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.from_float needs 1 arg\n"); return; }
        emit_float_expr(cg, e->args[0]);
        fprintf(cg->out,
            "    /* str.from_float(f): convert float to heap string */\n"
            "    callq __silica_ftoa\n"        /* rax=ptr rdx=len */
        );
        return;
    }

    /* str.to_int(s) → rax = integer parsed from string */
    if (strcmp(name, "std.str.to_int") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.to_int needs 1 arg\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out,
            "    /* str.to_int(s): parse decimal integer */\n"
            "    movq %%rax, %%rsi\n"          /* rsi = ptr */
            "    movq %%rdx, %%rcx\n"          /* rcx = len */
            "    xorq %%rax, %%rax\n"          /* rax = result = 0 */
            "    xorq %%rbx, %%rbx\n"          /* rbx = sign flag */
            "    testq %%rcx, %%rcx\n"
            "    jz .Latoi_done%d\n"
            "    movb (%%rsi), %%dl\n"
            "    cmpb $45, %%dl\n"             /* '-' */
            "    jne .Latoi_loop%d\n"
            "    movq $1, %%rbx\n"
            "    incq %%rsi\n"
            "    decq %%rcx\n"
            ".Latoi_loop%d:\n"
            "    testq %%rcx, %%rcx\n"
            "    jz .Latoi_sign%d\n"
            "    movb (%%rsi), %%dl\n"
            "    cmpb $48, %%dl\n"             /* '0' */
            "    jl .Latoi_sign%d\n"
            "    cmpb $57, %%dl\n"             /* '9' */
            "    jg .Latoi_sign%d\n"
            "    imulq $10, %%rax\n"
            "    movzbq %%dl, %%rdx\n"
            "    subq $48, %%rdx\n"
            "    addq %%rdx, %%rax\n"
            "    incq %%rsi\n"
            "    decq %%rcx\n"
            "    jmp .Latoi_loop%d\n"
            ".Latoi_sign%d:\n"
            "    testq %%rbx, %%rbx\n"
            "    jz .Latoi_done%d\n"
            "    negq %%rax\n"
            ".Latoi_done%d:\n",
            lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* str.upper(s) → rax=ptr rdx=len  (new heap string, ASCII a-z → A-Z) */
    if (strcmp(name, "std.str.upper") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.upper needs 1 arg\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out,
            "    /* str.upper: alloc copy, uppercase ASCII */\n"
            "    pushq %%rax\n"                /* src ptr */
            "    pushq %%rdx\n"               /* src len */
            "    /* alloc len+1 bytes */\n"
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"         /* r12 = dst base */
            "    movq 0(%%rsp), %%rcx\n"      /* rcx = len */
            "    movq %%rax, %%rdi\n"
            "    addq %%rcx, %%rdi\n"
            "    incq %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            "    movq 8(%%rsp), %%rsi\n"      /* rsi = src ptr */
            "    movq 0(%%rsp), %%rcx\n"      /* rcx = len */
            "    xorq %%rbx, %%rbx\n"
            ".Lupper_loop%d:\n"
            "    cmpq %%rcx, %%rbx\n"
            "    jge .Lupper_done%d\n"
            "    movb (%%rsi,%%rbx,1), %%al\n"
            "    cmpb $97, %%al\n"            /* 'a' */
            "    jl .Lupper_noconv%d\n"
            "    cmpb $122, %%al\n"           /* 'z' */
            "    jg .Lupper_noconv%d\n"
            "    subb $32, %%al\n"            /* toLower: add 32; toUpper: sub 32 */
            ".Lupper_noconv%d:\n"
            "    movb %%al, (%%r12,%%rbx,1)\n"
            "    incq %%rbx\n"
            "    jmp .Lupper_loop%d\n"
            ".Lupper_done%d:\n"
            "    movq %%r12, %%rax\n"
            "    movq 0(%%rsp), %%rdx\n"
            "    addq $16, %%rsp\n",
            lbl, lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* str.lower(s) → rax=ptr rdx=len  (new heap string, ASCII A-Z → a-z) */
    if (strcmp(name, "std.str.lower") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.lower needs 1 arg\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out,
            "    /* str.lower: alloc copy, lowercase ASCII */\n"
            "    pushq %%rax\n"
            "    pushq %%rdx\n"
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"
            "    movq 0(%%rsp), %%rcx\n"
            "    movq %%rax, %%rdi\n"
            "    addq %%rcx, %%rdi\n"
            "    incq %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            "    movq 8(%%rsp), %%rsi\n"
            "    movq 0(%%rsp), %%rcx\n"
            "    xorq %%rbx, %%rbx\n"
            ".Llower_loop%d:\n"
            "    cmpq %%rcx, %%rbx\n"
            "    jge .Llower_done%d\n"
            "    movb (%%rsi,%%rbx,1), %%al\n"
            "    cmpb $65, %%al\n"            /* 'A' */
            "    jl .Llower_noconv%d\n"
            "    cmpb $90, %%al\n"            /* 'Z' */
            "    jg .Llower_noconv%d\n"
            "    addb $32, %%al\n"
            ".Llower_noconv%d:\n"
            "    movb %%al, (%%r12,%%rbx,1)\n"
            "    incq %%rbx\n"
            "    jmp .Llower_loop%d\n"
            ".Llower_done%d:\n"
            "    movq %%r12, %%rax\n"
            "    movq 0(%%rsp), %%rdx\n"
            "    addq $16, %%rsp\n",
            lbl, lbl, lbl, lbl, lbl, lbl, lbl);
        return;
    }

    /* str.trim(s) → rax=ptr rdx=len  (zero-copy: advances ptr, reduces len) */
    if (strcmp(name, "std.str.trim") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] str.trim needs 1 arg\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    /* str.trim: strip leading/trailing spaces, tabs, newlines */\n"
            "    movq %%rax, %%rsi\n"          /* rsi = ptr */
            "    movq %%rdx, %%rcx\n"          /* rcx = len */
            /* trim leading */
            ".Ltrim_lead%d:\n"
            "    testq %%rcx, %%rcx\n"
            "    jz .Ltrim_done%d\n"
            "    movb (%%rsi), %%al\n"
            "    cmpb $32, %%al\n"             /* space */
            "    je .Ltrim_lead_adv%d\n"
            "    cmpb $9, %%al\n"              /* tab */
            "    je .Ltrim_lead_adv%d\n"
            "    cmpb $10, %%al\n"             /* newline */
            "    je .Ltrim_lead_adv%d\n"
            "    cmpb $13, %%al\n"             /* carriage return */
            "    je .Ltrim_lead_adv%d\n"
            "    jmp .Ltrim_trail%d\n"
            ".Ltrim_lead_adv%d:\n"
            "    incq %%rsi\n"
            "    decq %%rcx\n"
            "    jmp .Ltrim_lead%d\n"
            /* trim trailing */
            ".Ltrim_trail%d:\n"
            "    testq %%rcx, %%rcx\n"
            "    jz .Ltrim_done%d\n"
            "    movb -1(%%rsi,%%rcx,1), %%al\n"  /* last char */
            "    cmpb $32, %%al\n"
            "    je .Ltrim_trail_adv%d\n"
            "    cmpb $9, %%al\n"
            "    je .Ltrim_trail_adv%d\n"
            "    cmpb $10, %%al\n"
            "    je .Ltrim_trail_adv%d\n"
            "    cmpb $13, %%al\n"
            "    je .Ltrim_trail_adv%d\n"
            "    jmp .Ltrim_done%d\n"
            ".Ltrim_trail_adv%d:\n"
            "    decq %%rcx\n"
            "    jmp .Ltrim_trail%d\n"
            ".Ltrim_done%d:\n"
            "    movq %%rsi, %%rax\n"
            "    movq %%rcx, %%rdx\n",
            lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl);
        return;
    }

    /* str.repeat(s, n) → rax=ptr rdx=len  (concatenate s with itself n times) */
    if (strcmp(name, "std.str.repeat") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] str.repeat needs 2 args: (s, n)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_int_expr(cg, e->args[1]);         /* n → rax */
        fprintf(cg->out, "    pushq %%rax\n"); /* save n */
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out,
            "    /* str.repeat(s, n): alloc s.len*n bytes, copy n times */\n"
            "    movq %%rax, %%r14\n"          /* r14 = src ptr */
            "    movq %%rdx, %%r13\n"          /* r13 = src len */
            "    movq 0(%%rsp), %%r15\n"       /* r15 = n */
            "    /* total = len * n */\n"
            "    movq %%r13, %%rax\n"
            "    imulq %%r15, %%rax\n"         /* rax = total len */
            "    pushq %%rax\n"               /* save total */
            "    /* alloc total+1 bytes */\n"
            "    movq $12, %%rax\n"
            "    xorq %%rdi, %%rdi\n"
            "    syscall\n"
            "    movq %%rax, %%r12\n"         /* r12 = dst base */
            "    movq 0(%%rsp), %%rcx\n"      /* rcx = total */
            "    movq %%rax, %%rdi\n"
            "    addq %%rcx, %%rdi\n"
            "    incq %%rdi\n"
            "    movq $12, %%rax\n"
            "    syscall\n"
            /* copy loop: r15 times */
            "    movq %%r12, %%rdi\n"         /* dst = start of out buf */
            ".Lrepeat_loop%d:\n"
            "    testq %%r15, %%r15\n"
            "    jz .Lrepeat_done%d\n"
            "    movq %%r14, %%rsi\n"
            "    movq %%r13, %%rcx\n"
            "    rep movsb\n"
            "    decq %%r15\n"
            "    jmp .Lrepeat_loop%d\n"
            ".Lrepeat_done%d:\n"
            "    movq %%r12, %%rax\n"
            "    movq 0(%%rsp), %%rdx\n"      /* total len */
            "    addq $16, %%rsp\n",          /* pop total and n */
            lbl, lbl, lbl, lbl);
        return;
    }

    /* str.char_at(s, i) → rax = character code at index i */
    if (strcmp(name, "std.str.char_at") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] str.char_at needs 2 args: (s, i)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out, "    pushq %%rax\n"); /* save ptr */
        emit_int_expr(cg, e->args[1]);         /* idx → rax */
        fprintf(cg->out,
            "    /* str.char_at(s, i) */\n"
            "    movq 0(%%rsp), %%rcx\n"      /* ptr */
            "    addq $8, %%rsp\n"
            "    movzbq (%%rcx,%%rax,1), %%rax\n"
            "    /* str.char_at done */\n");
        return;
    }

    /* str.eq(a, b) → rax = 1 if equal, 0 otherwise */
    if (strcmp(name, "std.str.eq") == 0) {
        if (e->argc < 2) { fprintf(stderr, "[codegen] str.eq needs 2 args\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]);
        fprintf(cg->out,
            "    pushq %%rax\n"               /* a_ptr */
            "    pushq %%rdx\n");             /* a_len */
        emit_init_expr(cg, TYPE_STRING, e->args[1]);
        fprintf(cg->out,
            "    /* str.eq: compare a and b */\n"
            "    movq %%rax, %%r12\n"         /* b_ptr */
            "    movq %%rdx, %%r13\n"         /* b_len */
            "    movq 8(%%rsp), %%r14\n"      /* a_ptr */
            "    movq 0(%%rsp), %%r15\n"      /* a_len */
            "    addq $16, %%rsp\n"
            "    /* lengths must match */\n"
            "    cmpq %%r13, %%r15\n"
            "    jne .Lstreq_no%d\n"
            "    xorq %%rbx, %%rbx\n"         /* i = 0 */
            ".Lstreq_loop%d:\n"
            "    cmpq %%r15, %%rbx\n"
            "    jge .Lstreq_yes%d\n"
            "    movb (%%r14,%%rbx,1), %%al\n"
            "    movb (%%r12,%%rbx,1), %%dl\n"
            "    cmpb %%al, %%dl\n"
            "    jne .Lstreq_no%d\n"
            "    incq %%rbx\n"
            "    jmp .Lstreq_loop%d\n"
            ".Lstreq_yes%d:\n"
            "    movq $1, %%rax\n"
            "    jmp .Lstreq_end%d\n"
            ".Lstreq_no%d:\n"
            "    xorq %%rax, %%rax\n"
            ".Lstreq_end%d:\n",
            lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl);
        return;
    }

    /* ════════════════════════════════════════════════════════════════
     * std.env
     * ════════════════════════════════════════════════════════════════ */

    /* env.argc() → rax = number of command-line arguments */
    if (strcmp(name, "std.env.argc") == 0) {
        fprintf(cg->out,
            "    /* env.argc() */\n"
            "    movq __silica_argc(%%rip), %%rax\n");
        return;
    }

    /* env.argv(i) → rax=ptr rdx=len  (the i-th argument as a string) */
    if (strcmp(name, "std.env.argv") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] env.argv needs 1 arg: (i)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_int_expr(cg, e->args[0]);         /* i → rax */
        fprintf(cg->out,
            "    /* env.argv(i): load argv[i], compute strlen */\n"
            "    movq __silica_argv(%%rip), %%rcx\n"  /* rcx = argv ptr */
            "    movq (%%rcx,%%rax,8), %%rsi\n"        /* rsi = argv[i] (C string) */
            "    testq %%rsi, %%rsi\n"
            "    jz .Largv_null%d\n"
            /* compute strlen(argv[i]) */
            "    xorq %%rdx, %%rdx\n"
            ".Largv_len%d:\n"
            "    movb (%%rsi,%%rdx,1), %%al\n"
            "    testb %%al, %%al\n"
            "    jz .Largv_done%d\n"
            "    incq %%rdx\n"
            "    jmp .Largv_len%d\n"
            ".Largv_done%d:\n"
            "    movq %%rsi, %%rax\n"
            "    jmp .Largv_end%d\n"
            ".Largv_null%d:\n"
            "    xorq %%rax, %%rax\n"
            "    xorq %%rdx, %%rdx\n"
            ".Largv_end%d:\n",
            lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl);
        return;
    }

    /* env.get(name) → rax=ptr rdx=len  (value of env var, or empty string) */
    if (strcmp(name, "std.env.get") == 0) {
        if (e->argc < 1) { fprintf(stderr, "[codegen] env.get needs 1 arg: (name)\n"); return; }
        int lbl = cg->lbl_counter++;
        emit_init_expr(cg, TYPE_STRING, e->args[0]); /* rax=ptr rdx=len */
        fprintf(cg->out,
            "    /* env.get(name): walk envp looking for name=value */\n"
            "    movq %%rax, %%r12\n"         /* r12 = name ptr */
            "    movq %%rdx, %%r13\n"         /* r13 = name len */
            "    movq __silica_envp(%%rip), %%r14\n"  /* r14 = envp array */
            "    xorq %%r15, %%r15\n"         /* r15 = envp index */
            ".Lenvget_outer%d:\n"
            "    movq (%%r14,%%r15,8), %%rbx\n"  /* rbx = envp[i] */
            "    testq %%rbx, %%rbx\n"
            "    jz .Lenvget_notfound%d\n"
            /* compare first r13 chars of envp[i] with name */
            "    xorq %%rcx, %%rcx\n"
            ".Lenvget_inner%d:\n"
            "    cmpq %%r13, %%rcx\n"
            "    jge .Lenvget_check_eq%d\n"
            "    movb (%%rbx,%%rcx,1), %%al\n"
            "    movb (%%r12,%%rcx,1), %%dl\n"
            "    cmpb %%al, %%dl\n"
            "    jne .Lenvget_next%d\n"
            "    incq %%rcx\n"
            "    jmp .Lenvget_inner%d\n"
            ".Lenvget_check_eq%d:\n"
            /* envp[i][name_len] must be '=' */
            "    movb (%%rbx,%%rcx,1), %%al\n"
            "    cmpb $61, %%al\n"            /* '=' */
            "    jne .Lenvget_next%d\n"
            /* found: value starts at envp[i] + name_len + 1 */
            "    incq %%rcx\n"               /* skip '=' */
            "    movq %%rbx, %%rax\n"
            "    addq %%rcx, %%rax\n"        /* rax = value ptr */
            /* compute strlen(value) */
            "    xorq %%rdx, %%rdx\n"
            ".Lenvget_vlen%d:\n"
            "    movb (%%rax,%%rdx,1), %%al\n"
            "    testb %%al, %%al\n"
            "    jz .Lenvget_found%d\n"
            "    incq %%rdx\n"
            "    jmp .Lenvget_vlen%d\n"
            ".Lenvget_found%d:\n"
            "    movq %%rbx, %%rax\n"
            "    addq %%rcx, %%rax\n"
            "    jmp .Lenvget_end%d\n"
            ".Lenvget_next%d:\n"
            "    incq %%r15\n"
            "    jmp .Lenvget_outer%d\n"
            ".Lenvget_notfound%d:\n"
            /* emit a local empty byte right here in .text — safe as data */
            "    jmp .Lenvget_skip_empty%d\n"
            ".Lenvget_empty%d:\n"
            "    .byte 0\n"
            ".Lenvget_skip_empty%d:\n"
            "    leaq .Lenvget_empty%d(%%rip), %%rax\n"
            "    xorq %%rdx, %%rdx\n"
            ".Lenvget_end%d:\n",
            lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl,lbl);
        return;
    }

    {
        FuncDecl *fd = find_userfunc_overload(cg, raw, e->argc, e->args);
        if (!fd) fd = find_userfunc_overload(cg, name, e->argc, e->args);
        if (fd) {
            char lbl[128];
            func_label(cg, fd, lbl, sizeof(lbl));
            fprintf(cg->out, "    /* call user func: %s */\n", lbl);
            /*
             * System V AMD64 ABI: integer/pointer args in rdi, rsi, rdx, rcx, r8, r9.
             * We evaluate each arg into rax then move to the correct register.
             * We push extra args onto the stack (beyond 6) in reverse order.
             */
            /*
             * Build argument list with register assignment.
             * Strings consume 2 registers (ptr, len). Integers consume 1.
             * We push all onto a temp stack, then pop into the registers.
             */
            static const char *argregs[] = {
                "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
            };

            /* Count total registers needed */
            int reg_idx = 0;
            /* First pass: push all arg values onto stack in order */
            for (int i = 0; i < fd->nparams && i < e->argc; i++) {
                VarType ptype = fd->params[i].type;
                Expr *arg = e->args[i];
                if (ptype == TYPE_STRING) {
                    /* push ptr then len (ptr on top of stack = lower address) */
                    if (arg->kind == EXPR_STRING_LIT) {
                        const char *lbl = pool_add(arg->sval, &cg->str_counter);
                        fprintf(cg->out,
                            "    leaq %s(%%rip), %%rax\n"
                            "    pushq %%rax\n"
                            "    movq $%zu, %%rax\n"
                            "    pushq %%rax\n",
                            lbl, strlen(arg->sval));
                    } else if (arg->kind == EXPR_IDENT) {
                        Var *sv = find_var(cg, arg->sval);
                        if (sv) {
                            fprintf(cg->out,
                                "    movq -%d(%%rbp), %%rax\n"  /* ptr */
                                "    pushq %%rax\n"
                                "    movq -%d(%%rbp), %%rax\n"  /* len */
                                "    pushq %%rax\n",
                                sv->offset, sv->offset - 8);
                        }
                    }
                    reg_idx += 2;
                } else {
                    emit_int_expr(cg, arg);
                    fprintf(cg->out, "    pushq %%rax\n");
                    reg_idx++;
                }
            }
            /* Second pass: pop into registers in reverse */
            for (int i = reg_idx - 1; i >= 0; i--) {
                if (i < 6)
                    fprintf(cg->out, "    popq %s\n", argregs[i]);
                else
                    break; /* leave extra on stack — caller-cleaned */
            }
            fprintf(cg->out, "    callq %s\n", lbl);
            return;
        }
    }

    if (name[0] != '\0')
        fprintf(stderr, "[codegen] warning: unknown call '%s'\n", name);
}

static void emit_expr(CG *cg, Expr *e) {
    switch (e->kind) {
        case EXPR_CALL:
            emit_call(cg, e);
            break;

        case EXPR_FIELD: {
            /* myStr.length — emit the length of a string variable */
            if (e->object && e->object->kind == EXPR_IDENT &&
                strcmp(e->field, "length") == 0) {
                Var *v = find_var(cg, e->object->sval);
                if (v && v->type == TYPE_STRING) {
                    fprintf(cg->out, "    movq -%d(%%rbp), %%rax\n", v->offset - 8);
                }
            }
            break;
        }

        case EXPR_ASSIGN: {
            char lhs[256] = "";
            flatten(e->lhs, lhs, sizeof(lhs));

            /* funcname.errorcode = N */
            if (strstr(lhs, ".errorcode")) {
                long code = (e->rhs && e->rhs->kind == EXPR_INT_LIT) ? e->rhs->ival : 0;
                cg->exit_code = (int)code;
                fprintf(cg->out, "    /* %s = %d */\n", lhs, cg->exit_code);
                break;
            }

            /* pointer dereference assignment: *p = val */
            if (e->lhs->kind == EXPR_DEREF) {
                emit_int_expr(cg, e->rhs);           /* rax = value to store */
                fprintf(cg->out, "    pushq %%rax\n");
                emit_int_expr(cg, e->lhs->rhs);      /* rax = pointer address */
                fprintf(cg->out,
                    "    popq %%rcx\n"
                    "    movq %%rcx, (%%rax)\n");     /* *ptr = value */
                break;
            }

            /* struct field assignment: s.field = val */
            if (e->lhs->kind == EXPR_FIELD &&
                e->lhs->object && e->lhs->object->kind == EXPR_IDENT) {
                Var *sv = find_var(cg, e->lhs->object->sval);
                if (sv && sv->type == TYPE_STRUCT && sv->struct_name) {
                    StructDecl *sd = find_struct(cg, sv->struct_name);
                    if (sd) {
                        StructField *sf = find_field(sd, e->lhs->field);
                        if (sf) {
                            emit_int_expr(cg, e->rhs);  /* rax = value */
                            fprintf(cg->out, "    movq %%rax, -%d(%%rbp)\n",
                                    sv->offset - sf->offset);
                            break;
                        }
                    }
                }
            }

            /* pointer-to-struct field assignment: p->field = val */
            if (e->lhs->kind == EXPR_PTR_FIELD) {
                emit_int_expr(cg, e->rhs);           /* rax = value */
                fprintf(cg->out, "    pushq %%rax\n");
                emit_int_expr(cg, e->lhs->lhs);      /* rax = struct pointer */
                /* resolve field offset */
                int field_off = 0;
                if (e->lhs->lhs->kind == EXPR_IDENT) {
                    Var *pv = find_var(cg, e->lhs->lhs->sval);
                    if (pv && pv->struct_name) {
                        StructDecl *sd = find_struct(cg, pv->struct_name);
                        if (sd && e->lhs->ptr_field) {
                            StructField *sf = find_field(sd, e->lhs->ptr_field);
                            if (sf) field_off = sf->offset;
                        }
                    }
                }
                fprintf(cg->out,
                    "    popq %%rcx\n"                          /* rcx = value */
                    "    movq %%rcx, %d(%%rax)\n", field_off);  /* ptr[field_off] = value */
                break;
            }

            /* array element assignment: arr[i] = val */
            if (e->lhs->kind == EXPR_INDEX) {
                Expr *arr_expr = e->lhs->lhs;
                Expr *idx_expr = e->lhs->rhs;
                emit_int_expr(cg, e->rhs);           /* rax = value */
                fprintf(cg->out, "    pushq %%rax\n");
                emit_int_expr(cg, idx_expr);          /* rax = index */
                fprintf(cg->out, "    pushq %%rax\n");
                /* load array base ptr */
                if (arr_expr->kind == EXPR_IDENT) {
                    Var *av = find_var(cg, arr_expr->sval);
                    if (av) fprintf(cg->out, "    movq -%d(%%rbp), %%r11\n", av->offset);
                }
                fprintf(cg->out,
                    "    popq %%rcx\n"              /* rcx = index */
                    "    popq %%rax\n"              /* rax = value */
                    "    movq %%rax, (%%r11,%%rcx,8)\n");
                break;
            }

            /* plain variable assignment */
            char base[128] = "";
            if (e->lhs->kind == EXPR_IDENT) strncpy(base, e->lhs->sval, sizeof(base)-1);
            Var *v = find_var(cg, base);
            if (!v) break;

            if (v->is_const) {
                fprintf(stderr, "[codegen] error: assignment to const variable '%s'\n", base);
                break;
            }

            emit_init_expr(cg, v->type, e->rhs);
            emit_store_var(cg, v);
            break;
        }

        default: break;
    }
}

/*
 * emit_float_expr:
 *   Evaluates a float-producing expression and leaves result in xmm0.
 */
static void emit_float_expr(CG *cg, Expr *e) {
    /* ── math constants: math.pi, math.e, math.i ── */
    if (e->kind == EXPR_FIELD) {
        char flat[256] = "";
        flatten(e, flat, sizeof(flat));
        char resolved[256] = "";
        resolve_name(cg, flat, resolved, sizeof(resolved));
        if (strcmp(resolved, "std.math.pi") == 0) {
            /* π = 3.14159265358979... */
            unsigned long long bits; double d = 3.14159265358979323846;
            memcpy(&bits, &d, 8);
            fprintf(cg->out,
                "    movabsq $0x%llx, %%rax\n"
                "    movq %%rax, -8(%%rsp)\n"
                "    movsd -8(%%rsp), %%xmm0\n",
                (unsigned long long)bits);
            return;
        }
        if (strcmp(resolved, "std.math.e") == 0) {
            /* e = 2.71828182845904... */
            unsigned long long bits; double d = 2.71828182845904523536;
            memcpy(&bits, &d, 8);
            fprintf(cg->out,
                "    movabsq $0x%llx, %%rax\n"
                "    movq %%rax, -8(%%rsp)\n"
                "    movsd -8(%%rsp), %%xmm0\n",
                (unsigned long long)bits);
            return;
        }
        /* math.i = sqrt(-1): not a real float — signal via NaN and let print handle it */
        if (strcmp(resolved, "std.math.i") == 0) {
            /* store a sentinel NaN so the type system stays float — print intercepts it */
            unsigned long long bits = 0x7FF8000000000001ULL; /* quiet NaN sentinel */
            fprintf(cg->out,
                "    movabsq $0x%llx, %%rax\n"
                "    movq %%rax, -8(%%rsp)\n"
                "    movsd -8(%%rsp), %%xmm0\n",
                (unsigned long long)bits);
            return;
        }
    }
    if (e->kind == EXPR_CAST) {
        VarType target = (VarType)e->cast_type;
        if (target == TYPE_FLOAT) {
            /* (float)intExpr → convert int to xmm0 */
            emit_int_expr(cg, e->rhs);
            fprintf(cg->out, "    cvtsi2sdq %%rax, %%xmm0\n");
            return;
        }
    }
    if (e->kind == EXPR_FLOAT_LIT) {
        unsigned long long bits;
        double d = e->fval;
        memcpy(&bits, &d, 8);
        fprintf(cg->out,
            "    movabsq $0x%llx, %%rax\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    movsd -8(%%rsp), %%xmm0\n",
            (unsigned long long)bits);
    } else if (e->kind == EXPR_INT_LIT) {
        double d = (double)e->ival;
        unsigned long long bits;
        memcpy(&bits, &d, 8);
        fprintf(cg->out,
            "    movabsq $0x%llx, %%rax\n"
            "    movq %%rax, -8(%%rsp)\n"
            "    movsd -8(%%rsp), %%xmm0\n",
            (unsigned long long)bits);
    } else if (e->kind == EXPR_IDENT) {
        Var *v = find_var(cg, e->sval);
        if (v) fprintf(cg->out, "    movsd -%d(%%rbp), %%xmm0\n", v->offset);
    } else if (e->kind == EXPR_CAST && (VarType)e->cast_type == TYPE_FLOAT) {
        /* (float)intExpr */
        emit_int_expr(cg, e->rhs);
        fprintf(cg->out, "    cvtsi2sdq %%rax, %%xmm0\n");
    } else if (e->kind == EXPR_CALL) {
        /* float-returning function: call it, result comes back in xmm0 */
        emit_call(cg, e);
    }
}

/*
 * emit_store_var:
 *   Stores a value into a variable of known type.
 *   Source conventions:
 *     int/long/uint/bool  -> rax
 *     char/byte           -> al  (low byte of rax)
 *     float               -> xmm0
 *     string              -> rax (ptr) + rdx (len)
 */
static void emit_store_var(CG *cg, Var *v) {
    switch (v->type) {
        case TYPE_INT: case TYPE_LONG: case TYPE_UINT: case TYPE_BOOL:
            fprintf(cg->out, "    movq %%rax, -%d(%%rbp)\n", v->offset);
            break;
        case TYPE_CHAR: case TYPE_BYTE:
            fprintf(cg->out, "    movb %%al, -%d(%%rbp)\n", v->offset);
            break;
        case TYPE_FLOAT:
            fprintf(cg->out, "    movsd %%xmm0, -%d(%%rbp)\n", v->offset);
            break;
        case TYPE_STRING:
            fprintf(cg->out,
                "    movq %%rax, -%d(%%rbp)\n"  /* ptr */
                "    movq %%rdx, -%d(%%rbp)\n", /* len */
                v->offset, v->offset - 8);
            break;
        case TYPE_VOID: break;
        case TYPE_PTR:
        case TYPE_VOID_PTR:
            fprintf(cg->out, "    movq %%rax, -%d(%%rbp)\n", v->offset);
            break;
        default:
            fprintf(cg->out, "    movq %%rax, -%d(%%rbp)\n", v->offset);
            break;
    }
}

/*
 * emit_init_expr:
 *   Evaluates any initialiser expression for a given destination type,
 *   placing the result in the correct return register(s).
 *   After this call, use emit_store_var to write to the stack slot.
 */
static void emit_init_expr(CG *cg, VarType dtype, Expr *src) {
    if (!src) { fprintf(cg->out, "    xorq %%rax, %%rax\n"); return; }

    switch (dtype) {
        case TYPE_INT: case TYPE_LONG: case TYPE_UINT: case TYPE_BOOL:
            emit_int_expr(cg, src);   /* → rax */
            break;

        case TYPE_CHAR: case TYPE_BYTE:
            if (src->kind == EXPR_CHAR_LIT)
                fprintf(cg->out, "    movq $%d, %%rax\n", (unsigned char)src->cval);
            else
                emit_int_expr(cg, src);   /* → rax, caller uses al */
            break;

        case TYPE_FLOAT:
            if (src->kind == EXPR_FLOAT_LIT || src->kind == EXPR_INT_LIT
                    || src->kind == EXPR_IDENT || src->kind == EXPR_CALL
                    || src->kind == EXPR_FIELD || src->kind == EXPR_CAST) {
                emit_float_expr(cg, src); /* → xmm0 */
            } else {
                /* fall back: treat as int and convert */
                emit_int_expr(cg, src);
                fprintf(cg->out,
                    "    movq %%rax, -8(%%rsp)\n"
                    "    cvtsi2sdq -8(%%rsp), %%xmm0\n");
            }
            break;

        case TYPE_STRING:
            if (src->kind == EXPR_STRING_LIT) {
                const char *lbl = pool_add(src->sval, &cg->str_counter);
                fprintf(cg->out,
                    "    leaq %s(%%rip), %%rax\n"
                    "    movq $%zu, %%rdx\n",
                    lbl, strlen(src->sval));
            } else if (src->kind == EXPR_IDENT) {
                Var *sv = find_var(cg, src->sval);
                if (sv) {
                    fprintf(cg->out,
                        "    movq -%d(%%rbp), %%rax\n"
                        "    movq -%d(%%rbp), %%rdx\n",
                        sv->offset, sv->offset - 8);
                }
            } else if (src->kind == EXPR_CALL) {
                /* string-returning call: ptr→rax, len→rdx after call */
                emit_call(cg, src);
            }
            break;

        case TYPE_VOID: break;
        case TYPE_PTR:
        case TYPE_VOID_PTR:
            emit_int_expr(cg, src);   /* → rax (address) */
            break;
        default:
            emit_int_expr(cg, src);
            break;
    }
}

static void emit_stmt(CG *cg, Stmt *s) {
    switch (s->kind) {
        case STMT_BREAK:
            if (cg->loop_depth > 0)
                fprintf(cg->out, "    jmp %s\n", cg->loop_end[cg->loop_depth - 1]);
            else
                fprintf(stderr, "[codegen] break outside loop\n");
            break;

        case STMT_CONTINUE:
            if (cg->loop_depth > 0)
                fprintf(cg->out, "    jmp %s\n", cg->loop_top[cg->loop_depth - 1]);
            else
                fprintf(stderr, "[codegen] continue outside loop\n");
            break;

        case STMT_RETURN: {
            if (s->init) {
                /* Use the current function's return type (stored in cg) */
                emit_init_expr(cg, cg->cur_rettype, s->init);
            }
            if (cg->ret_label[0])
                fprintf(cg->out, "    jmp %s\n", cg->ret_label);
            break;
        }
        case STMT_VAR_DECL: {
            /* struct variable: Point p; */
            if (s->vtype == TYPE_STRUCT && s->struct_name) {
                StructDecl *sd = find_struct(cg, s->struct_name);
                int total = sd ? sd->total_size : 8;
                int idx = alloc_struct_var(cg, s->varname, s->struct_name, total);
                Var *v  = &cg->vars[idx];
                fprintf(cg->out, "    /* struct %s %s at rbp-%d (size=%d) */\n",
                        s->struct_name, s->varname, v->offset, total);
                /* zero-initialise the whole struct */
                for (int i = 0; i < total; i += 8)
                    fprintf(cg->out, "    movq $0, -%d(%%rbp)\n", v->offset - i);
                break;
            }

            /* pointer variable: int* p = &x;  /  void* p = mem.alloc(...) */
            if (s->is_ptr || s->vtype == TYPE_VOID_PTR) {
                VarType slot_type = (s->vtype == TYPE_VOID_PTR) ? TYPE_VOID_PTR : TYPE_PTR;
                int idx = alloc_var(cg, s->varname, slot_type, s->is_const);
                Var *v  = &cg->vars[idx];
                /* keep struct_name for ptr-to-struct */
                if (s->struct_name) v->struct_name = strdup(s->struct_name);
                fprintf(cg->out, "    /* ptr %s at rbp-%d */\n", s->varname, v->offset);
                if (s->init) {
                    emit_int_expr(cg, s->init);   /* rax = address */
                    fprintf(cg->out, "    movq %%rax, -%d(%%rbp)\n", v->offset);
                } else {
                    fprintf(cg->out, "    movq $0, -%d(%%rbp)\n", v->offset);
                }
                break;
            }

            /* array declaration: int[] arr = {1,2,3};
               Allocates heap via brk; stores base ptr in a single qword slot */
            if (s->is_array) {
                int idx = alloc_var(cg, s->varname, TYPE_INT, s->is_const);
                Var *v  = &cg->vars[idx];
                fprintf(cg->out, "    /* array %s at rbp-%d */\n", s->varname, v->offset);
                if (s->init && s->init->kind == EXPR_ARRAY_LIT) {
                    int n = s->init->argc;
                    /* allocate n * 8 bytes via brk */
                    fprintf(cg->out,
                        "    /* alloc array[%d] */\n"
                        "    movq $12, %%rax\n"
                        "    xorq %%rdi, %%rdi\n"
                        "    syscall\n"
                        "    movq %%rax, %%r12\n"        /* r12 = base ptr */
                        "    movq %%rax, %%rdi\n"
                        "    addq $%d, %%rdi\n"
                        "    movq $12, %%rax\n"
                        "    syscall\n",                 /* extend heap */
                        n, n * 8);
                    /* evaluate and store each element */
                    for (int i = 0; i < n; i++) {
                        emit_int_expr(cg, s->init->args[i]);
                        fprintf(cg->out,
                            "    movq %%rax, %d(%%r12)\n", i * 8);
                    }
                    /* store base ptr in the variable slot */
                    fprintf(cg->out, "    movq %%r12, -%d(%%rbp)\n", v->offset);
                } else {
                    fprintf(cg->out, "    movq $0, -%d(%%rbp)\n", v->offset);
                }
                break;
            }

            int idx = alloc_var(cg, s->varname, s->vtype, s->is_const);
            Var *v  = &cg->vars[idx];
            fprintf(cg->out, "    /* var %s at rbp-%d */\n", s->varname, v->offset);

            if (!s->init) {
                fprintf(cg->out, "    movq $0, -%d(%%rbp)\n", v->offset);
                if (s->vtype == TYPE_STRING)
                    fprintf(cg->out, "    movq $0, -%d(%%rbp)\n", v->offset - 8);
                break;
            }

            /* evaluate init expression into the correct register(s), then store */
            emit_init_expr(cg, s->vtype, s->init);
            emit_store_var(cg, v);
            break;
        }
        case STMT_IF: {
            int lbl = cg->lbl_counter++;
            /* evaluate condition into rax */
            emit_int_expr(cg, s->cond);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jz .Lif_else%d\n",
                lbl);
            /* then-branch */
            for (int i = 0; i < s->nbody; i++)
                emit_stmt(cg, s->body[i]);
            if (s->nelsebody > 0)
                fprintf(cg->out, "    jmp .Lif_end%d\n", lbl);
            fprintf(cg->out, ".Lif_else%d:\n", lbl);
            /* else-branch (may be empty) */
            for (int i = 0; i < s->nelsebody; i++)
                emit_stmt(cg, s->elsebody[i]);
            fprintf(cg->out, ".Lif_end%d:\n", lbl);
            break;
        }

        case STMT_WHILE: {
            int lbl = cg->lbl_counter++;
            Expr *cond = s->cond;

            /* push loop labels for break/continue */
            int d = cg->loop_depth++;
            snprintf(cg->loop_top[d], 64, ".Lwhile_top%d", lbl);
            snprintf(cg->loop_end[d], 64, ".Lwhile_end%d", lbl);

            /* while(true) — unconditional loop */
            if (cond->kind == EXPR_BOOL_LIT && cond->ival == 1) {
                fprintf(cg->out, ".Lwhile_top%d:\n", lbl);
                for (int i = 0; i < s->nbody; i++)
                    emit_stmt(cg, s->body[i]);
                fprintf(cg->out,
                    "    jmp .Lwhile_top%d\n"
                    ".Lwhile_end%d:\n",
                    lbl, lbl);
                cg->loop_depth--;
                break;
            }

            /* while(N) integer literal — execute exactly N times */
            if (cond->kind == EXPR_INT_LIT) {
                fprintf(cg->out,
                    "    movq $%ld, %%r15\n"
                    ".Lwhile_top%d:\n"
                    "    testq %%r15, %%r15\n"
                    "    jz .Lwhile_end%d\n",
                    cond->ival, lbl, lbl);
                for (int i = 0; i < s->nbody; i++)
                    emit_stmt(cg, s->body[i]);
                fprintf(cg->out,
                    "    decq %%r15\n"
                    "    jmp .Lwhile_top%d\n"
                    ".Lwhile_end%d:\n",
                    lbl, lbl);
                cg->loop_depth--;
                break;
            }

            /* while(expr) — evaluate condition each iteration */
            fprintf(cg->out, ".Lwhile_top%d:\n", lbl);
            emit_int_expr(cg, cond);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jz .Lwhile_end%d\n",
                lbl);
            for (int i = 0; i < s->nbody; i++)
                emit_stmt(cg, s->body[i]);
            fprintf(cg->out,
                "    jmp .Lwhile_top%d\n"
                ".Lwhile_end%d:\n",
                lbl, lbl);
            cg->loop_depth--;
            break;
        }

        case STMT_FOR: {
            int lbl = cg->lbl_counter++;
            Expr *cond = s->cond;

            /* push loop labels for break/continue */
            int d = cg->loop_depth++;
            snprintf(cg->loop_top[d], 64, ".Lfor_top%d", lbl);
            snprintf(cg->loop_end[d], 64, ".Lfor_end%d", lbl);

            /* for(N) — run exactly N times */
            if (cond->kind == EXPR_INT_LIT) {
                fprintf(cg->out,
                    "    movq $%ld, %%r15\n"
                    ".Lfor_top%d:\n"
                    "    testq %%r15, %%r15\n"
                    "    jz .Lfor_end%d\n",
                    cond->ival, lbl, lbl);
                for (int i = 0; i < s->nbody; i++)
                    emit_stmt(cg, s->body[i]);
                fprintf(cg->out,
                    "    decq %%r15\n"
                    "    jmp .Lfor_top%d\n"
                    ".Lfor_end%d:\n",
                    lbl, lbl);
                cg->loop_depth--;
                break;
            }

            /* for(expr) — loop while expression is truthy */
            fprintf(cg->out, ".Lfor_top%d:\n", lbl);
            emit_int_expr(cg, cond);
            fprintf(cg->out,
                "    testq %%rax, %%rax\n"
                "    jz .Lfor_end%d\n",
                lbl);
            for (int i = 0; i < s->nbody; i++)
                emit_stmt(cg, s->body[i]);
            fprintf(cg->out,
                "    jmp .Lfor_top%d\n"
                ".Lfor_end%d:\n",
                lbl, lbl);
            cg->loop_depth--;
            break;
        }

        case STMT_EXPR:
            emit_expr(cg, s->expr);
            break;
    }
}

/* ── helper subroutines emitted into .text ── */
static void emit_helpers(FILE *out) {
    /*
     * __silica_readline
     *   Reads one line from stdin (fd=0) byte-by-byte until \n or EOF.
     *   Allocates a 4096-byte buffer via sys_brk on first call.
     *   Returns: r12 = pointer to buffer, r13 = bytes read (including \n if present)
     *   Clobbers: rax, rbx, rcx, rdi, rsi, rdx, r12, r13, r14
     */
    fprintf(out,
        "\n"
        "# ── readline helper (reads one line from stdin) ──\n"
        "__silica_readline:\n"
        "    /* allocate 4096-byte buffer via sys_brk */\n"
        "    movq $12, %%rax\n"
        "    xorq %%rdi, %%rdi\n"
        "    syscall\n"
        "    movq %%rax, %%r12\n"        /* r12 = buf base */
        "    movq %%rax, %%rdi\n"
        "    addq $4096, %%rdi\n"
        "    movq $12, %%rax\n"
        "    syscall\n"                  /* heap extended */
        "    xorq %%r13, %%r13\n"        /* r13 = index / length */
        "    leaq -8(%%rsp), %%r14\n"    /* r14 = 1-byte scratch in red zone */
        ".Lrl_loop:\n"
        "    movq $0, %%rax\n"           /* sys_read */
        "    movq $0, %%rdi\n"           /* fd = stdin */
        "    movq %%r14, %%rsi\n"        /* buf = scratch */
        "    movq $1, %%rdx\n"           /* read 1 byte */
        "    syscall\n"
        "    testq %%rax, %%rax\n"
        "    jle .Lrl_done\n"            /* 0=EOF, <0=error */
        "    movb (%%r14), %%al\n"
        "    movb %%al, (%%r12,%%r13,1)\n"  /* store into buf */
        "    incq %%r13\n"
        "    cmpb $10, %%al\n"           /* stop at newline */
        "    je .Lrl_done\n"
        "    cmpq $4095, %%r13\n"        /* stop at buffer limit */
        "    jl .Lrl_loop\n"
        ".Lrl_done:\n"
        "    retq\n"
    );

    fprintf(out,
        "\n"
        "# ── int-to-string print helper ──\n"
        "__silica_print_int_nl:\n"
        "    movq $1, %%r11\n"          /* r11 = 1 means add newline */
        "    jmp __silica_print_int_body\n"
        "__silica_print_int:\n"
        "    xorq %%r11, %%r11\n"       /* r11 = 0, no newline */
        "__silica_print_int_body:\n"
        "    movq %%rax, %%rbx\n"       /* rbx = value to convert */
        "    /* buffer: 24 bytes in red zone, build right-to-left */\n"
        "    leaq -24(%%rsp), %%rcx\n"  /* rcx = one past end of buf */
        "    /* handle sign */\n"
        "    movq $0, %%r10\n"          /* r10 = 0: positive */
        "    testq %%rbx, %%rbx\n"
        "    jns .Lpi_nonneg\n"
        "    movq $1, %%r10\n"
        "    negq %%rbx\n"
        ".Lpi_nonneg:\n"
        "    /* optional newline at far end of buf */\n"
        "    testq %%r11, %%r11\n"
        "    jz .Lpi_no_nl\n"
        "    decq %%rcx\n"
        "    movb $10, (%%rcx)\n"
        ".Lpi_no_nl:\n"
        "    movq $10, %%r8\n"          /* divisor */
        ".Lpi_digit_loop:\n"
        "    xorq %%rdx, %%rdx\n"
        "    movq %%rbx, %%rax\n"
        "    divq %%r8\n"               /* rax=quot rdx=remainder */
        "    movq %%rax, %%rbx\n"
        "    addq $48, %%rdx\n"
        "    decq %%rcx\n"
        "    movb %%dl, (%%rcx)\n"
        "    testq %%rbx, %%rbx\n"
        "    jnz .Lpi_digit_loop\n"
        "    /* prepend '-' if negative */\n"
        "    testq %%r10, %%r10\n"
        "    jz .Lpi_write\n"
        "    decq %%rcx\n"
        "    movb $45, (%%rcx)\n"       /* '-' */
        ".Lpi_write:\n"
        "    leaq -24(%%rsp), %%rdx\n"  /* end of region (fixed) */
        "    subq %%rcx, %%rdx\n"       /* rdx = length */
        "    movq $1, %%rax\n"          /* sys_write */
        "    movq $1, %%rdi\n"
        "    movq %%rcx, %%rsi\n"
        "    syscall\n"
        "    retq\n"
    );

    /*
     * __silica_print_float / __silica_print_float_nl
     *   Input: xmm0 = IEEE 754 double
     *   Prints: integer_part.fractional_part (6 digits)
     *   Pure integer arithmetic — no libm.
     */
    fprintf(out,
        "\n"
        "# ── float-to-string print helper ──\n"
        "__silica_print_float_nl:\n"
        "    movq $1, %%r11\n"
        "    jmp __silica_print_float_body\n"
        "__silica_print_float:\n"
        "    xorq %%r11, %%r11\n"
        "__silica_print_float_body:\n"
        "    pushq %%r11\n"             /* save nl flag on real stack */
        "    pushq %%rbp\n"
        "    movq %%rsp, %%rbp\n"
        "    subq $32, %%rsp\n"
        "    /* print integer part */\n"
        "    cvttsd2si %%xmm0, %%rax\n"
        "    /* save float and int part */\n"
        "    movsd %%xmm0, -8(%%rbp)\n"
        "    movq %%rax, -16(%%rbp)\n"
        "    callq __silica_print_int\n"
        "    /* print '.' */\n"
        "    movb $46, -24(%%rbp)\n"
        "    movq $1, %%rax\n"
        "    movq $1, %%rdi\n"
        "    leaq -24(%%rbp), %%rsi\n"
        "    movq $1, %%rdx\n"
        "    syscall\n"
        "    /* frac = |xmm0 - int_part| * 1000000 */\n"
        "    movsd -8(%%rbp), %%xmm0\n"
        "    cvtsi2sd -16(%%rbp), %%xmm1\n"
        "    subsd %%xmm1, %%xmm0\n"
        "    /* abs(frac) */\n"
        "    movq $0x7fffffffffffffff, %%rax\n"
        "    movq %%rax, -8(%%rbp)\n"
        "    movsd -8(%%rbp), %%xmm2\n"
        "    andpd %%xmm2, %%xmm0\n"
        "    movq $1000000, %%rax\n"
        "    cvtsi2sd %%rax, %%xmm1\n"
        "    mulsd %%xmm1, %%xmm0\n"
        "    cvttsd2si %%xmm0, %%rax\n"
        "    callq __silica_print_int\n"
        "    /* newline? */\n"
        "    movq 16(%%rbp), %%r11\n"   /* nl flag was at rbp+16 (pushed before rbp) */
        "    testq %%r11, %%r11\n"
        "    jz .Lfloat_done\n"
        "    movb $10, -24(%%rbp)\n"
        "    movq $1, %%rax\n"
        "    movq $1, %%rdi\n"
        "    leaq -24(%%rbp), %%rsi\n"
        "    movq $1, %%rdx\n"
        "    syscall\n"
        ".Lfloat_done:\n"
        "    movq %%rbp, %%rsp\n"
        "    popq %%rbp\n"
        "    popq %%r11\n"
        "    retq\n"
    );

    /*
     * __silica_random
     *   Input:  rax = seed (if 0, uses a default seed of 0xdeadbeefcafe1234)
     *   Output: rax = pseudo-random 64-bit integer (xorshift64 PRNG)
     *   The state is kept in a static .bss qword so each call advances it.
     *   Clobbers: rax, rcx
     */
    fprintf(out,
        "\n"
        "# ── math.random (xorshift64 PRNG) ──\n"
        "__silica_random:\n"
        "    /* if seed==0 use default */\n"
        "    testq %%rax, %%rax\n"
        "    jnz .Lrand_use_seed\n"
        "    movq $0xdeadbeefcafe1234, %%rax\n"
        ".Lrand_use_seed:\n"
        "    /* store seed into state */\n"
        "    movq %%rax, __silica_rand_state(%%rip)\n"
        "    /* xorshift64 */\n"
        "    movq __silica_rand_state(%%rip), %%rax\n"
        "    movq %%rax, %%rcx\n"
        "    shlq $13, %%rcx\n"
        "    xorq %%rcx, %%rax\n"
        "    movq %%rax, %%rcx\n"
        "    shrq $7,  %%rcx\n"
        "    xorq %%rcx, %%rax\n"
        "    movq %%rax, %%rcx\n"
        "    shlq $17, %%rcx\n"
        "    xorq %%rcx, %%rax\n"
        "    /* make positive (clear sign bit) */\n"
        "    movabsq $0x7fffffffffffffff, %%rcx\n"
        "    andq %%rcx, %%rax\n"
        "    movq %%rax, __silica_rand_state(%%rip)\n"
        "    retq\n"
    );

    /*
     * __silica_sqrt
     *   Input:  rax = n (non-negative integer)
     *   Output: rax = floor(sqrt(n))  using Newton-Raphson integer method
     *   Clobbers: rax, rbx, rcx, rdx
     */
    fprintf(out,
        "\n"
        "# ── math.sqrt (integer square root, Newton-Raphson) ──\n"
        "__silica_sqrt:\n"
        "    testq %%rax, %%rax\n"
        "    jz .Lsqrt_done\n"          /* sqrt(0) = 0 */
        "    cmpq $1, %%rax\n"
        "    je .Lsqrt_done\n"          /* sqrt(1) = 1 */
        "    movq %%rax, %%rbx\n"       /* rbx = n */
        "    /* initial guess x = n >> 1 (or at least 1) */\n"
        "    movq %%rbx, %%rcx\n"
        "    shrq $1, %%rcx\n"
        "    testq %%rcx, %%rcx\n"
        "    jnz .Lsqrt_loop\n"
        "    movq $1, %%rcx\n"
        ".Lsqrt_loop:\n"
        "    /* x1 = (x + n/x) / 2 */\n"
        "    movq %%rbx, %%rax\n"       /* rax = n */
        "    xorq %%rdx, %%rdx\n"
        "    divq %%rcx\n"              /* rax = n/x */
        "    addq %%rcx, %%rax\n"       /* rax = x + n/x */
        "    shrq $1, %%rax\n"          /* rax = (x + n/x)/2 = x1 */
        "    cmpq %%rcx, %%rax\n"       /* if x1 >= x, converged */
        "    jge .Lsqrt_converged\n"
        "    movq %%rax, %%rcx\n"       /* x = x1, iterate */
        "    jmp .Lsqrt_loop\n"
        ".Lsqrt_converged:\n"
        "    movq %%rcx, %%rax\n"       /* return x */
        ".Lsqrt_done:\n"
        "    retq\n"
    );

    /*
     * __silica_root
     *   Input:  rdi = k (the root degree, e.g. 3 for cube root)
     *           rax = n (the number to root)
     *   Output: rax = floor(n^(1/k))  using Newton-Raphson
     *   Clobbers: rax, rbx, rcx, rdx, r8, r9
     *
     *   Newton step: x1 = ((k-1)*x + n/x^(k-1)) / k
     */
    fprintf(out,
        "\n"
        "# ── math.root (integer nth root, Newton-Raphson) ──\n"
        "__silica_root:\n"
        "    /* rdi=k, rax=n */\n"
        "    movq %%rax, %%rbx\n"       /* rbx = n */
        "    movq %%rdi, %%r8\n"        /* r8  = k */
        "    /* edge cases */\n"
        "    testq %%rbx, %%rbx\n"
        "    jz .Lroot_done_zero\n"
        "    cmpq $1, %%r8\n"
        "    je .Lroot_k1\n"
        "    cmpq $2, %%r8\n"
        "    je .Lroot_sqrt\n"
        "    /* initial guess: n >> (k/2) or at least 2 */\n"
        "    movq %%r8, %%rcx\n"
        "    shrq $1, %%rcx\n"
        "    movq %%rbx, %%r9\n"
        "    shrq %%cl, %%r9\n"
        "    cmpq $2, %%r9\n"
        "    jge .Lroot_loop\n"
        "    movq $2, %%r9\n"
        ".Lroot_loop:\n"
        "    /* x^(k-1) via __silica_pwr: rdi=k-1, rax=x */\n"
        "    /* save r8(k), r9(x), rbx(n) around call */\n"
        "    pushq %%r8\n"
        "    pushq %%r9\n"
        "    pushq %%rbx\n"
        "    movq %%r8, %%rdi\n"
        "    decq %%rdi\n"             /* rdi = k-1 */
        "    movq %%r9, %%rax\n"       /* rax = x */
        "    callq __silica_pwr\n"     /* rax = x^(k-1) */
        "    movq %%rax, %%rcx\n"      /* rcx = x^(k-1) */
        "    popq %%rbx\n"             /* restore n */
        "    popq %%r9\n"              /* restore x */
        "    popq %%r8\n"              /* restore k */
        "    /* n / x^(k-1) */\n"
        "    movq %%rbx, %%rax\n"
        "    xorq %%rdx, %%rdx\n"
        "    testq %%rcx, %%rcx\n"
        "    jz .Lroot_done_zero\n"
        "    divq %%rcx\n"             /* rax = n / x^(k-1) */
        "    /* (k-1)*x + n/x^(k-1) */\n"
        "    movq %%r8, %%rcx\n"
        "    decq %%rcx\n"             /* rcx = k-1 */
        "    imulq %%r9, %%rcx\n"      /* rcx = (k-1)*x */
        "    addq %%rcx, %%rax\n"      /* rax = (k-1)*x + n/x^(k-1) */
        "    xorq %%rdx, %%rdx\n"
        "    divq %%r8\n"              /* rax = new x */
        "    cmpq %%r9, %%rax\n"
        "    jge .Lroot_converged\n"
        "    movq %%rax, %%r9\n"
        "    jmp .Lroot_loop\n"
        ".Lroot_converged:\n"
        "    /* rax = new_x which is the floor answer */\n"
        "    retq\n"
        ".Lroot_k1:\n"
        "    movq %%rbx, %%rax\n"
        "    retq\n"
        ".Lroot_sqrt:\n"
        "    movq %%rbx, %%rax\n"
        "    callq __silica_sqrt\n"
        "    retq\n"
        ".Lroot_done_zero:\n"
        "    xorq %%rax, %%rax\n"
        "    retq\n"
    );

    /*
     * __silica_pwr
     *   Input:  rdi = exponent (e)
     *           rax = base (b)
     *   Output: rax = b^e  (exponentiation by squaring)
     *   Clobbers: rax, rbx, rcx, rdx, rdi
     */
    fprintf(out,
        "\n"
        "# ── math.pwr (integer exponentiation by squaring) ──\n"
        "__silica_pwr:\n"
        "    /* rdi=exp, rax=base */\n"
        "    movq %%rax, %%rbx\n"       /* rbx = base */
        "    movq %%rdi, %%rcx\n"       /* rcx = exp  */
        "    movq $1, %%rax\n"          /* rax = result = 1 */
        "    testq %%rcx, %%rcx\n"
        "    jz .Lpwr_done\n"           /* b^0 = 1 */
        ".Lpwr_loop:\n"
        "    testq $1, %%rcx\n"         /* if exp is odd */
        "    jz .Lpwr_skip_mul\n"
        "    imulq %%rbx, %%rax\n"      /* result *= base */
        ".Lpwr_skip_mul:\n"
        "    imulq %%rbx, %%rbx\n"      /* base = base^2 */
        "    shrq $1, %%rcx\n"          /* exp >>= 1 */
        "    testq %%rcx, %%rcx\n"
        "    jnz .Lpwr_loop\n"
        ".Lpwr_done:\n"
        "    retq\n"
    );

    /* Static storage for the PRNG state (in .bss, zero-initialized) */
    fprintf(out,
        "\n"
        ".section .bss\n"
        "__silica_rand_state:\n"
        "    .quad 0\n"
        "# ── argc / argv / envp captured at _start ──\n"
        "__silica_argc:\n"
        "    .quad 0\n"
        "__silica_argv:\n"
        "    .quad 0\n"
        "__silica_envp:\n"
        "    .quad 0\n"
        ".section .text\n"
    );

    /*
     * __silica_itoa
     *   Input:  rax = 64-bit signed integer
     *   Output: rax = ptr to heap string, rdx = length
     *   Allocates a 24-byte buffer via brk (enough for any int64).
     *   Clobbers: rax, rbx, rcx, rdx, rdi, r12
     */
    fprintf(out,
        "\n"
        "# ── str.from_int (itoa: integer → heap string) ──\n"
        "__silica_itoa:\n"
        "    /* alloc 24 bytes for the string */\n"
        "    pushq %%rax\n"               /* save value */
        "    movq $12, %%rax\n"
        "    xorq %%rdi, %%rdi\n"
        "    syscall\n"
        "    movq %%rax, %%r12\n"         /* r12 = buf base */
        "    movq %%rax, %%rdi\n"
        "    addq $24, %%rdi\n"
        "    movq $12, %%rax\n"
        "    syscall\n"
        "    popq %%rax\n"               /* restore value */
        "    /* convert int in rax to ASCII into r12 */\n"
        "    movq %%rax, %%rbx\n"        /* rbx = value */
        "    movq $0, %%rcx\n"           /* rcx = negative flag */
        "    testq %%rbx, %%rbx\n"
        "    jns .Litoa_pos\n"
        "    movq $1, %%rcx\n"
        "    negq %%rbx\n"
        ".Litoa_pos:\n"
        "    /* build digits right-to-left in buf */\n"
        "    leaq 23(%%r12), %%rdi\n"    /* end of buffer */
        "    movb $0, (%%rdi)\n"         /* null term (not counted) */
        "    movq $10, %%r8\n"
        ".Litoa_loop:\n"
        "    decq %%rdi\n"
        "    xorq %%rdx, %%rdx\n"
        "    movq %%rbx, %%rax\n"
        "    divq %%r8\n"                /* rax=quot rdx=digit */
        "    movq %%rax, %%rbx\n"
        "    addq $48, %%rdx\n"
        "    movb %%dl, (%%rdi)\n"
        "    testq %%rbx, %%rbx\n"
        "    jnz .Litoa_loop\n"
        "    testq %%rcx, %%rcx\n"       /* prepend '-' if negative */
        "    jz .Litoa_write\n"
        "    decq %%rdi\n"
        "    movb $45, (%%rdi)\n"        /* '-' */
        ".Litoa_write:\n"
        "    /* compute length = buf_end - start */\n"
        "    leaq 23(%%r12), %%rax\n"    /* buf_end (before null) */
        "    subq %%rdi, %%rax\n"        /* len = end - start */
        "    movq %%rax, %%rdx\n"        /* rdx = len */
        "    movq %%rdi, %%rax\n"        /* rax = ptr to start of digits */
        "    retq\n"
    );

    /*
     * __silica_ftoa
     *   Input:  xmm0 = IEEE 754 double
     *   Output: rax = ptr, rdx = len  (heap string "int.frac" format)
     *   Reuses __silica_itoa for both halves.
     */
    fprintf(out,
        "\n"
        "# ── str.from_float (ftoa: float → heap string) ──\n"
        "__silica_ftoa:\n"
        "    pushq %%rbp\n"
        "    movq %%rsp, %%rbp\n"
        "    subq $64, %%rsp\n"
        "    movsd %%xmm0, -8(%%rbp)\n"      /* save float */
        "    /* alloc 32-byte output buffer */\n"
        "    movq $12, %%rax\n"
        "    xorq %%rdi, %%rdi\n"
        "    syscall\n"
        "    movq %%rax, -16(%%rbp)\n"        /* out_base */
        "    movq %%rax, %%rdi\n"
        "    addq $32, %%rdi\n"
        "    movq $12, %%rax\n"
        "    syscall\n"
        "    /* integer part → itoa → copy */\n"
        "    movsd -8(%%rbp), %%xmm0\n"
        "    cvttsd2siq %%xmm0, %%rax\n"
        "    callq __silica_itoa\n"            /* rax=ptr rdx=len */
        "    movq %%rax, %%rsi\n"              /* src = digit string */
        "    movq %%rdx, %%rcx\n"              /* count = len */
        "    movq -16(%%rbp), %%rdi\n"         /* dst = out_base */
        "    movq %%rdx, -24(%%rbp)\n"         /* save int_len */
        "    rep movsb\n"
        "    /* append '.' */\n"
        "    movq -16(%%rbp), %%rdi\n"
        "    addq -24(%%rbp), %%rdi\n"
        "    movb $46, (%%rdi)\n"
        "    incq -24(%%rbp)\n"                /* total_len++ */
        "    /* fractional part: |xmm0 - int| * 1000000 → itoa → copy (6 digits) */\n"
        "    movsd -8(%%rbp), %%xmm0\n"
        "    cvttsd2siq %%xmm0, %%rax\n"
        "    cvtsi2sdq %%rax, %%xmm1\n"
        "    subsd %%xmm1, %%xmm0\n"           /* frac part */
        "    movabsq $0x7fffffffffffffff, %%rax\n"
        "    movq %%rax, -32(%%rbp)\n"
        "    movsd -32(%%rbp), %%xmm2\n"
        "    andpd %%xmm2, %%xmm0\n"           /* abs(frac) */
        "    movq $1000000, %%rax\n"
        "    cvtsi2sdq %%rax, %%xmm1\n"
        "    mulsd %%xmm1, %%xmm0\n"
        "    cvttsd2siq %%xmm0, %%rax\n"
        "    callq __silica_itoa\n"            /* rax=ptr rdx=len */
        "    movq %%rax, %%rsi\n"
        "    movq %%rdx, %%rcx\n"
        "    movq -16(%%rbp), %%rdi\n"
        "    addq -24(%%rbp), %%rdi\n"
        "    rep movsb\n"
        "    addq %%rdx, -24(%%rbp)\n"         /* total_len += frac_len */
        "    movq -16(%%rbp), %%rax\n"         /* return ptr */
        "    movq -24(%%rbp), %%rdx\n"         /* return len */
        "    movq %%rbp, %%rsp\n"
        "    popq %%rbp\n"
        "    retq\n"
    );
}

/*
 * emit_func — emits a single user-defined function.
 *
 * Calling convention: System V AMD64 ABI.
 *   Args 1-6  : rdi, rsi, rdx, rcx, r8, r9  (integer/pointer)
 *   Return val: rax  (integer/pointer)
 *
 * Frame layout:
 *   [rbp+16] ... [rbp+N]  : stack args (if any, beyond 6)
 *   [rbp]                  : saved rbp
 *   [rbp-8]..[rbp-N]       : locals (parameters first, then declared vars)
 */
/* ── prescan: count stack bytes needed by a statement list (no emit) ── */
static int prescan_stmts(StructDecl **structs, int nstructs,
                          Stmt **stmts, int nstmts, int *running) {
    for (int i = 0; i < nstmts; i++) {
        Stmt *s = stmts[i];
        if (!s) continue;
        if (s->kind == STMT_VAR_DECL) {
            if (s->vtype == TYPE_STRUCT && s->struct_name) {
                int sz = 8;
                for (int j = 0; j < nstructs; j++)
                    if (strcmp(structs[j]->name, s->struct_name) == 0)
                        { sz = (structs[j]->total_size + 7) & ~7; if (sz < 8) sz = 8; break; }
                *running += sz;
            } else if (s->vtype == TYPE_STRING) {
                *running += 16;
            } else {
                *running += 8;
            }
        }
        /* recurse into branches */
        if (s->nbody)     prescan_stmts(structs, nstructs, s->body,     s->nbody,     running);
        if (s->nelsebody) prescan_stmts(structs, nstructs, s->elsebody, s->nelsebody, running);
    }
    return *running;
}

static void emit_func(FuncDecl *fd, FILE *out, CG *parent_cg) {
    /* fresh CG for this function, but sharing str/lbl counters with parent */
    CG cg;
    memset(&cg, 0, sizeof(cg));
    cg.out         = out;
    cg.using_io    = parent_cg->using_io;
    cg.using_math  = parent_cg->using_math;
    cg.using_fs    = parent_cg->using_fs;
    cg.using_mem   = parent_cg->using_mem;
    cg.using_time  = parent_cg->using_time;
    cg.using_net   = parent_cg->using_net;
    cg.using_proc  = parent_cg->using_proc;
    cg.using_str   = parent_cg->using_str;
    cg.using_env   = parent_cg->using_env;
    cg.str_counter = parent_cg->str_counter;
    cg.lbl_counter = parent_cg->lbl_counter;
    cg.funcs       = parent_cg->funcs;
    cg.nfuncs      = parent_cg->nfuncs;
    cg.structs     = parent_cg->structs;
    cg.nstructs    = parent_cg->nstructs;
    cg.enums       = parent_cg->enums;
    cg.nenums      = parent_cg->nenums;

    /* unique return label */
    snprintf(cg.ret_label, sizeof(cg.ret_label), ".Lret_%s_%d",
             fd->name, cg.lbl_counter++);
    cg.cur_rettype = fd->rettype;

    static const char *argregs[] = {
        "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
    };

    char sym[128];
    func_label(&cg, fd, sym, sizeof(sym));

    /* ── Dynamic frame sizing ──
     * Pre-scan to find how many bytes of locals this function needs.
     * Parameters each take 8 bytes (or 16 for strings).
     * Locals are counted by prescan_stmts.
     * We add 128 bytes of scratch/red-zone safety. */
    int param_bytes = 0;
    for (int i = 0; i < fd->nparams; i++)
        param_bytes += (fd->params[i].type == TYPE_STRING) ? 16 : 8;
    int local_bytes = 0;
    prescan_stmts(cg.structs, cg.nstructs, fd->stmts, fd->nstmts, &local_bytes);
    /* round up to 16-byte alignment, add 128 scratch */
    int frame_bytes = ((param_bytes + local_bytes + 128 + 15) & ~15);
    if (frame_bytes < 128) frame_bytes = 128;

    fprintf(out,
        "\n# ── user function: %s (params=%d, frame=%d%s%s) ──\n",
        fd->name, fd->nparams, frame_bytes,
        fd->is_static ? ", static" : "",
        fd->is_inline ? ", inline" : "");

    /* static and inline functions have local linkage — no .global directive */
    if (!fd->is_static && !fd->is_inline)
        fprintf(out, ".global %s\n", sym);

    fprintf(out,
        "%s:\n"
        "    pushq %%rbp\n"
        "    movq  %%rsp, %%rbp\n"
        "    subq  $%d, %%rsp\n",
        sym, frame_bytes);

    /* spill register args into local stack slots.
     * Strings take 2 registers: first reg=ptr, second reg=len.
     * We track a register index separately from param index. */
    int ri = 0;  /* register index */
    for (int i = 0; i < fd->nparams; i++) {
        int idx = alloc_var(&cg, fd->params[i].name, fd->params[i].type, 0);
        Var *v  = &cg.vars[idx];
        if (fd->params[i].type == TYPE_STRING) {
            /* ptr in argregs[ri], len in argregs[ri+1] */
            if (ri < 6)
                fprintf(out, "    movq %s, -%d(%%rbp)\n", argregs[ri], v->offset);
            if (ri + 1 < 6)
                fprintf(out, "    movq %s, -%d(%%rbp)\n", argregs[ri+1], v->offset - 8);
            ri += 2;
        } else if (fd->params[i].type == TYPE_CHAR || fd->params[i].type == TYPE_BYTE) {
            if (ri < 6)
                fprintf(out, "    movb %sb, -%d(%%rbp)\n", argregs[ri], v->offset);
            ri++;
        } else {
            if (ri < 6)
                fprintf(out, "    movq %s, -%d(%%rbp)\n", argregs[ri], v->offset);
            else {
                int stack_off = 16 + (ri - 6) * 8;
                fprintf(out, "    movq %d(%%rbp), %%rax\n", stack_off);
                fprintf(out, "    movq %%rax, -%d(%%rbp)\n", v->offset);
            }
            ri++;
        }
    }

    /* emit body statements */
    for (int i = 0; i < fd->nstmts; i++)
        emit_stmt(&cg, fd->stmts[i]);

    /* epilogue label — all returns jump here */
    fprintf(out,
        "%s:\n"
        "    movq  %%rbp, %%rsp\n"
        "    popq  %%rbp\n"
        "    retq\n",
        cg.ret_label);

    /* propagate counters back so labels stay unique globally */
    parent_cg->str_counter = cg.str_counter;
    parent_cg->lbl_counter = cg.lbl_counter;
}

/* ── emit .rodata ── */
static void emit_rodata(FILE *out) {
    if (str_pool_n == 0) return;
    fprintf(out, "\n.section .rodata\n");
    for (int i = 0; i < str_pool_n; i++) {
        fprintf(out, "%s:\n    .byte ", str_pool[i].label);
        const char *s = str_pool[i].value;
        for (size_t j = 0; j < str_pool[i].len; j++) {
            if (j) fprintf(out, ",");
            fprintf(out, "%d", (unsigned char)s[j]);
        }
        fprintf(out, "\n");
        fprintf(out, "%s_len = %zu\n", str_pool[i].label, str_pool[i].len);
    }
}

/* ── public entry point ── */
int codegen_emit(Program *prog, FILE *out) {
    CG cg;
    memset(&cg, 0, sizeof(cg));
    cg.out       = out;
    cg.funcs     = prog->funcs;
    cg.nfuncs    = prog->nfuncs;
    cg.structs   = prog->structs;
    cg.nstructs  = prog->nstructs;
    cg.enums     = prog->enums;
    cg.nenums    = prog->nenums;

    /* scan imports to enable stdlib modules — every module must be imported */
    for (int i = 0; i < prog->nimports; i++) {
        const char *m = prog->imports[i].module;
        if (strcmp(m, "std.io")   == 0) cg.using_io   = 1;
        if (strcmp(m, "std.math") == 0) cg.using_math = 1;
        if (strcmp(m, "std.fs")   == 0) cg.using_fs   = 1;
        if (strcmp(m, "std.mem")  == 0) cg.using_mem  = 1;
        if (strcmp(m, "std.time") == 0) cg.using_time = 1;
        if (strcmp(m, "std.net")  == 0) cg.using_net  = 1;
        if (strcmp(m, "std.proc") == 0) cg.using_proc = 1;
        if (strcmp(m, "std.str")  == 0) cg.using_str  = 1;
        if (strcmp(m, "std.env")  == 0) cg.using_env  = 1;
    }

    fprintf(out,
        "# Silica compiled output -- x86-64 Linux, no libc\n"
        ".section .text\n");

    /* emit user-defined functions */
    for (int i = 0; i < prog->nfuncs; i++)
        if (!prog->funcs[i]->is_extern)
            emit_func(prog->funcs[i], out, &cg);

    /* only emit _start and runtime helpers if this is an executable (has main) */
    if (prog->mainfn) {
        /* dynamic frame for main */
        int main_locals = 0;
        prescan_stmts(cg.structs, cg.nstructs,
                      prog->mainfn->stmts, prog->mainfn->nstmts, &main_locals);
        int main_frame = ((main_locals + 256 + 15) & ~15);
        if (main_frame < 256) main_frame = 256;

        fprintf(out,
            ".global _start\n"
            "\n_start:\n"
            "    /* capture argc/argv/envp from OS stack before frame setup */\n"
            "    movq 0(%%rsp), %%rax\n"
            "    movq %%rax, __silica_argc(%%rip)\n"
            "    leaq 8(%%rsp), %%rax\n"
            "    movq %%rax, __silica_argv(%%rip)\n"
            "    movq 0(%%rsp), %%rcx\n"
            "    leaq 16(%%rsp,%%rcx,8), %%rax\n"
            "    movq %%rax, __silica_envp(%%rip)\n"
            "    pushq %%rbp\n"
            "    movq  %%rsp, %%rbp\n"
            "    subq  $%d, %%rsp\n\n",
            main_frame);

        MainFunc *mf = prog->mainfn;
        fprintf(out, "    # main: %s\n", mf->name);
        for (int i = 0; i < mf->nstmts; i++)
            emit_stmt(&cg, mf->stmts[i]);

        fprintf(out,
            "\n    # exit\n"
            "    movq $60, %%rax\n"
            "    movq $%d, %%rdi\n"
            "    syscall\n",
            cg.exit_code);

        emit_helpers(out);
    }

    emit_rodata(out);

    for (int i = 0; i < str_pool_n; i++) {
        free(str_pool[i].label);
        free(str_pool[i].value);
    }
    str_pool_n = 0;
    return cg.had_error ? 1 : 0;
}
