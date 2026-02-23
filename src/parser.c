#define _GNU_SOURCE
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── error with source line + caret ── */
static void show_error_line(Parser *p, int line, int col) {
    if (!p->src) return;
    /* walk to the start of the given line */
    const char *s = p->src;
    for (int l = 1; l < line && *s; s++)
        if (*s == '\n') l++;
    /* find end of line */
    const char *end = s;
    while (*end && *end != '\n') end++;
    /* print the line */
    fprintf(stderr, "  %.*s\n  ", (int)(end - s), s);
    /* print caret, tab-aware */
    for (int c = 1; c < col; c++)
        fputc(s[c-1] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^\n");
}

/* ── internal helpers ── */

static void adv(Parser *p) {
    token_free(&p->cur);
    p->cur = p->nxt;
    p->nxt = lexer_next(&p->lexer);
}

static Token *cur(Parser *p) { return &p->cur; }
static int check(Parser *p, TokenType t) { return p->cur.type == t; }

static int match(Parser *p, TokenType t) {
    if (check(p, t)) { adv(p); return 1; }
    return 0;
}

static void expect(Parser *p, TokenType t) {
    if (!match(p, t)) {
        fprintf(stderr, "[parser] %d:%d: expected '%s', got '%s' (\"%s\")\n",
                p->cur.line, p->cur.col,
                token_type_name(t), token_type_name(p->cur.type),
                p->cur.value ? p->cur.value : "");
        show_error_line(p, p->cur.line, p->cur.col);
        p->errors++;
    }
}

/* ── forward declarations ── */
static Expr *parse_expr(Parser *p);
static Expr *parse_assign(Parser *p);
static Stmt *parse_stmt(Parser *p);
static void  parse_body(Parser *p, Stmt ***stmts_out, int *nstmts_out);

/* ── parse a type keyword → VarType, returns 0 if not a type ── */
/* For IDENT tokens, only treat as struct type when followed by IDENT or * (not . or () */
static int parse_type_kw(Parser *p, VarType *out) {
    switch (p->cur.type) {
        case TOK_INT:    *out = TYPE_INT;    break;
        case TOK_CHAR:   *out = TYPE_CHAR;   break;
        case TOK_STRING: *out = TYPE_STRING; break;
        case TOK_BOOL:   *out = TYPE_BOOL;   break;
        case TOK_FLOAT:  *out = TYPE_FLOAT;  break;
        case TOK_LONG:   *out = TYPE_LONG;   break;
        case TOK_BYTE:   *out = TYPE_BYTE;   break;
        case TOK_UINT:   *out = TYPE_UINT;   break;
        case TOK_VOID:   *out = TYPE_VOID;   break;
        /* IDENT as struct type name — only when next token is IDENT or * (not . or () */
        case TOK_IDENT:
            if (p->nxt.type == TOK_IDENT || p->nxt.type == TOK_STAR) {
                *out = TYPE_STRUCT;
                break;
            }
            return 0;
        default: return 0;
    }
    adv(p);
    return 1;
}

/* ── dot-chained qualified name: a.b.c ── */
static Expr *parse_qualified(Parser *p) {
    Expr *base = expr_new(EXPR_IDENT);
    base->sval = strdup(cur(p)->value);
    adv(p);
    while (check(p, TOK_DOT)) {
        adv(p);
        Expr *f = expr_new(EXPR_FIELD);
        f->object = base;
        f->field  = strdup(cur(p)->value);
        adv(p);
        base = f;
    }
    return base;
}

/* ── argument list (after '(' consumed) ── */
static Expr **parse_args(Parser *p, int *out_argc) {
    *out_argc = 0;
    if (check(p, TOK_RPAREN)) { adv(p); return NULL; }
    int cap = 8;
    Expr **args = malloc(cap * sizeof(Expr *));
    do {
        if (*out_argc >= cap) args = realloc(args, (cap *= 2) * sizeof(Expr *));
        args[(*out_argc)++] = parse_assign(p);
    } while (match(p, TOK_COMMA));
    expect(p, TOK_RPAREN);
    return args;
}

/* ── primary: literals, identifiers, calls, parenthesised exprs, casts, ! ── */
static Expr *parse_primary(Parser *p) {
    if (check(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT_LIT);
        e->ival = atol(cur(p)->value); adv(p); return e;
    }
    if (check(p, TOK_FLOAT_LIT)) {
        Expr *e = expr_new(EXPR_FLOAT_LIT);
        e->fval = atof(cur(p)->value); adv(p); return e;
    }
    if (check(p, TOK_TRUE))  { Expr *e = expr_new(EXPR_BOOL_LIT); e->ival = 1; adv(p); return e; }
    if (check(p, TOK_FALSE)) { Expr *e = expr_new(EXPR_BOOL_LIT); e->ival = 0; adv(p); return e; }
    if (check(p, TOK_CHAR_LIT)) {
        Expr *e = expr_new(EXPR_CHAR_LIT);
        e->cval = cur(p)->value[0]; adv(p); return e;
    }
    if (check(p, TOK_STRING_LIT)) {
        Expr *e = expr_new(EXPR_STRING_LIT);
        e->sval = strdup(cur(p)->value); adv(p); return e;
    }
    /* logical not: !expr */
    if (check(p, TOK_BANG)) {
        adv(p);
        Expr *e = expr_new(EXPR_LOGICAL_NOT);
        e->rhs = parse_primary(p);
        return e;
    }
    /* unary minus */
    if (check(p, TOK_MINUS)) {
        adv(p);
        Expr *e = expr_new(EXPR_UNARY_NEG);
        e->rhs = parse_primary(p);
        return e;
    }
    /* address-of: &x */
    if (check(p, TOK_AMP)) {
        adv(p);
        Expr *e = expr_new(EXPR_ADDROF);
        e->rhs = parse_primary(p);
        return e;
    }
    /* pointer dereference: *p */
    if (check(p, TOK_STAR)) {
        adv(p);
        Expr *e = expr_new(EXPR_DEREF);
        e->rhs = parse_primary(p);
        return e;
    }
    /* parenthesised expr OR type cast: (type)expr */
    if (check(p, TOK_LPAREN)) {
        adv(p);
        /* peek: is this a type cast? */
        VarType cast_vt;
        if (parse_type_kw(p, &cast_vt)) {
            /* could be (int), (float), (string), etc. */
            expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_CAST);
            e->cast_type = (int)cast_vt;
            e->rhs = parse_primary(p);
            return e;
        }
        Expr *e = parse_assign(p);
        expect(p, TOK_RPAREN);
        /* after ), check for subscript: (expr)[idx] — uncommon but valid */
        while (check(p, TOK_LBRACKET)) {
            adv(p);
            Expr *idx = expr_new(EXPR_INDEX);
            idx->lhs = e;
            idx->rhs = parse_assign(p);
            expect(p, TOK_RBRACKET);
            e = idx;
        }
        return e;
    }
    /* array literal: {e1, e2, ...} */
    if (check(p, TOK_LBRACE)) {
        adv(p);
        Expr *e = expr_new(EXPR_ARRAY_LIT);
        int cap = 8; e->argc = 0;
        e->args = malloc(cap * sizeof(Expr *));
        if (!check(p, TOK_RBRACE)) {
            do {
                if (e->argc >= cap) e->args = realloc(e->args, (cap*=2)*sizeof(Expr*));
                e->args[e->argc++] = parse_assign(p);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RBRACE);
        return e;
    }
    if (check(p, TOK_IDENT)) {
        Expr *e = parse_qualified(p);
        /* function call */
        if (check(p, TOK_LPAREN)) {
            adv(p);
            Expr *call = expr_new(EXPR_CALL);
            call->callee = e;
            call->args   = parse_args(p, &call->argc);
            e = call;
        }
        /* pointer field access: p->field (can chain) */
        while (check(p, TOK_ARROW)) {
            adv(p);
            Expr *pf = expr_new(EXPR_PTR_FIELD);
            pf->lhs = e;
            pf->ptr_field = strdup(cur(p)->value);
            expect(p, TOK_IDENT);
            e = pf;
        }
        /* array subscript: arr[i] (can chain: arr[i][j]) */
        while (check(p, TOK_LBRACKET)) {
            adv(p);
            Expr *idx = expr_new(EXPR_INDEX);
            idx->lhs = e;
            idx->rhs = parse_assign(p);
            expect(p, TOK_RBRACKET);
            e = idx;
        }
        return e;
    }
    fprintf(stderr, "[parser] %d:%d: unexpected token '%s' in expression\n",
            cur(p)->line, cur(p)->col, cur(p)->value ? cur(p)->value : "?");
    show_error_line(p, cur(p)->line, cur(p)->col);
    p->errors++;
    adv(p);
    return expr_new(EXPR_INT_LIT);
}

/* ── multiplicative: * / % ── */
static Expr *parse_mul(Parser *p) {
    Expr *left = parse_primary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        char op = cur(p)->value[0];
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = op;
        e->lhs = left;
        e->rhs = parse_primary(p);
        left = e;
    }
    return left;
}

/* ── additive: + - ── */
static Expr *parse_add(Parser *p) {
    Expr *left = parse_mul(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        char op = cur(p)->value[0];
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = op;
        e->lhs = left;
        e->rhs = parse_mul(p);
        left = e;
    }
    return left;
}

/* ── relational and equality comparisons ── */
static Expr *parse_cmp(Parser *p) {
    Expr *left = parse_add(p);
    for (;;) {
        char op = 0;
        /* encode ops as single chars: = (==), ! (!=), < (lt), > (gt), L (<=), G (>=) */
        if      (check(p, TOK_EQ))  op = '=';
        else if (check(p, TOK_NEQ)) op = '!';
        else if (check(p, TOK_LT))  op = '<';
        else if (check(p, TOK_GT))  op = '>';
        else if (check(p, TOK_LTE)) op = 'L';
        else if (check(p, TOK_GTE)) op = 'G';
        if (!op) break;
        adv(p);
        Expr *e = expr_new(EXPR_COMPARE);
        e->op  = op;
        e->lhs = left;
        e->rhs = parse_add(p);
        left = e;
    }
    return left;
}

/* ── logical AND: && ── */
static Expr *parse_logical_and(Parser *p) {
    Expr *left = parse_cmp(p);
    while (check(p, TOK_AND)) {
        adv(p);
        Expr *e = expr_new(EXPR_LOGICAL_AND);
        e->lhs = left;
        e->rhs = parse_cmp(p);
        left = e;
    }
    return left;
}

/* ── logical OR: || ── */
static Expr *parse_logical_or(Parser *p) {
    Expr *left = parse_logical_and(p);
    while (check(p, TOK_OR)) {
        adv(p);
        Expr *e = expr_new(EXPR_LOGICAL_OR);
        e->lhs = left;
        e->rhs = parse_logical_and(p);
        left = e;
    }
    return left;
}

/* ── assignment: lhs = rhs ── */
static Expr *parse_assign(Parser *p) {
    Expr *e = parse_logical_or(p);
    if (check(p, TOK_ASSIGN)) {
        adv(p);
        Expr *asgn = expr_new(EXPR_ASSIGN);
        asgn->lhs = e;
        asgn->rhs = parse_assign(p);
        return asgn;
    }
    return e;
}

static Expr *parse_expr(Parser *p) { return parse_assign(p); }

/* ── variable tracking (redeclaration detection) ── */
static int parser_var_exists(Parser *p, const char *name) {
    for (int i = 0; i < p->nlocals; i++)
        if (strcmp(p->locals[i], name) == 0) return 1;
    return 0;
}

static void parser_add_var(Parser *p, const char *name) {
    if (p->nlocals < 256) p->locals[p->nlocals++] = strdup(name);
}

/* ── statement parser ── */
static Stmt *parse_stmt(Parser *p) {
    /* break */
    if (check(p, TOK_BREAK)) {
        adv(p); expect(p, TOK_SEMICOLON);
        return stmt_new(STMT_BREAK);
    }
    /* continue */
    if (check(p, TOK_CONTINUE)) {
        adv(p); expect(p, TOK_SEMICOLON);
        return stmt_new(STMT_CONTINUE);
    }

    /* if / else if / else */
    if (check(p, TOK_IF)) {
        adv(p);
        expect(p, TOK_LPAREN);
        Stmt *s  = stmt_new(STMT_IF);
        s->cond  = parse_expr(p);
        expect(p, TOK_RPAREN);
        parse_body(p, &s->body, &s->nbody);
        if (check(p, TOK_ELSE)) {
            adv(p);
            if (check(p, TOK_IF)) {
                /* else if — recursively parse the if as the single else stmt */
                Stmt *elif = parse_stmt(p);
                s->elsebody  = malloc(sizeof(Stmt *));
                s->elsebody[0] = elif;
                s->nelsebody = 1;
            } else {
                parse_body(p, &s->elsebody, &s->nelsebody);
            }
        }
        return s;
    }

    /* return statement */
    if (check(p, TOK_RETURN)) {
        adv(p);
        Stmt *s = stmt_new(STMT_RETURN);
        if (!check(p, TOK_SEMICOLON))
            s->init = parse_expr(p);
        expect(p, TOK_SEMICOLON);
        return s;
    }

    /* variable declarations (optionally prefixed with const) */
    int is_const = 0;
    if (check(p, TOK_CONST)) { is_const = 1; adv(p); }

    /* struct VarName varname = ...; */
    int is_struct_decl = 0;
    char struct_type_name[256] = "";
    if (check(p, TOK_STRUCT)) {
        adv(p);
        is_struct_decl = 1;
        strncpy(struct_type_name, cur(p)->value, sizeof(struct_type_name)-1);
        expect(p, TOK_IDENT);
    }

    VarType vt;
    int is_decl = is_struct_decl ? 1 : parse_type_kw(p, &vt);
    if (is_struct_decl) vt = TYPE_STRUCT;

    if (is_decl) {
        /* check for pointer: type* name */
        int is_ptr = 0;
        VarType ptr_base = vt;
        if (check(p, TOK_STAR)) {
            adv(p);
            is_ptr = 1;
            vt = TYPE_PTR;
        }

        /* check for array declaration: type[] name = {...} */
        int is_array = 0;
        if (!is_ptr && check(p, TOK_LBRACKET)) {
            adv(p); expect(p, TOK_RBRACKET);
            is_array = 1;
        }

        char *name = strdup(cur(p)->value);
        expect(p, TOK_IDENT);

        /* redeclaration → assignment */
        if (!is_array && !is_ptr && !is_struct_decl && parser_var_exists(p, name)) {
            Stmt *s = stmt_new(STMT_EXPR);
            Expr *lhs = expr_new(EXPR_IDENT);
            lhs->sval = name;
            if (check(p, TOK_ASSIGN)) {
                adv(p);
                Expr *asgn = expr_new(EXPR_ASSIGN);
                asgn->lhs = lhs;
                asgn->rhs = parse_expr(p);
                s->expr = asgn;
            } else {
                s->expr = lhs;
            }
            expect(p, TOK_SEMICOLON);
            return s;
        }

        parser_add_var(p, name);
        Stmt *s     = stmt_new(STMT_VAR_DECL);
        s->vtype    = is_array ? TYPE_INT : vt;
        s->elem_type = is_array ? ptr_base : (is_ptr ? ptr_base : vt);
        s->varname  = name;
        s->is_const = is_const;
        s->is_array = is_array;
        s->is_ptr   = is_ptr;
        if (is_struct_decl || vt == TYPE_STRUCT)
            s->struct_name = strdup(struct_type_name);
        if (check(p, TOK_ASSIGN)) {
            adv(p);
            s->init = parse_expr(p);
        }
        expect(p, TOK_SEMICOLON);
        return s;
    }

    /*
     * Check for loops.while(...) { } and loops.for(...) { }
     */
    if (check(p, TOK_IDENT) && strcmp(cur(p)->value, "loops") == 0
            && p->nxt.type == TOK_DOT) {
        adv(p); adv(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "[parser] %d:%d: expected 'while' or 'for' after 'loops.'\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            return stmt_new(STMT_EXPR);
        }
        int is_while = strcmp(cur(p)->value, "while") == 0;
        int is_for   = strcmp(cur(p)->value, "for")   == 0;
        if (!is_while && !is_for) {
            fprintf(stderr, "[parser] %d:%d: unknown loops function '%s'\n",
                    cur(p)->line, cur(p)->col, cur(p)->value);
            p->errors++;
            return stmt_new(STMT_EXPR);
        }
        adv(p);
        expect(p, TOK_LPAREN);
        Stmt *s = stmt_new(is_while ? STMT_WHILE : STMT_FOR);
        s->cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        parse_body(p, &s->body, &s->nbody);
        return s;
    }

    /* expression statement */
    Stmt *s = stmt_new(STMT_EXPR);
    s->expr = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    return s;
}

/* ── parse a block of statements into stmts/nstmts ── */
static void parse_body(Parser *p, Stmt ***stmts_out, int *nstmts_out) {
    expect(p, TOK_LBRACE);
    int cap = 16;
    Stmt **stmts = malloc(cap * sizeof(Stmt *));
    int n = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (n >= cap) stmts = realloc(stmts, (cap *= 2) * sizeof(Stmt *));
        stmts[n++] = parse_stmt(p);
    }
    expect(p, TOK_RBRACE);
    *stmts_out  = stmts;
    *nstmts_out = n;
}

/* ── top-level ── */

static void parse_import(Parser *p, Program *prog) {
    adv(p);
    char buf[256] = "";
    while (cur(p)->type != TOK_SEMICOLON && cur(p)->type != TOK_EOF) {
        if (check(p, TOK_DOT)) { strncat(buf, ".", sizeof(buf)-strlen(buf)-1); adv(p); }
        else if (cur(p)->value) { strncat(buf, cur(p)->value, sizeof(buf)-strlen(buf)-1); adv(p); }
        else break;
    }
    expect(p, TOK_SEMICOLON);
    prog->imports = realloc(prog->imports, (prog->nimports+1)*sizeof(ImportDecl));
    prog->imports[prog->nimports++].module = strdup(buf);
}

static void parse_using(Parser *p, Program *prog) {
    adv(p);
    char *ns = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_SEMICOLON);
    prog->usings = realloc(prog->usings, (prog->nusings+1)*sizeof(UsingDecl));
    prog->usings[prog->nusings++].ns = ns;
}

static void parse_struct(Parser *p, Program *prog) {
    adv(p); /* consume 'struct' */
    char *sname = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    StructDecl *sd = structdecl_new();
    sd->name = sname;
    int fcap = 8;
    sd->fields = malloc(fcap * sizeof(StructField));

    int offset = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (sd->nfields >= fcap)
            sd->fields = realloc(sd->fields, (fcap *= 2) * sizeof(StructField));

        VarType ftype;
        int is_ptr = 0;
        char nested_struct[256] = "";

        if (check(p, TOK_STRUCT)) {
            adv(p);
            strncpy(nested_struct, cur(p)->value, sizeof(nested_struct)-1);
            expect(p, TOK_IDENT);
            ftype = TYPE_STRUCT;
        } else if (!parse_type_kw(p, &ftype)) {
            fprintf(stderr, "[parser] %d:%d: expected field type in struct\n",
                    cur(p)->line, cur(p)->col);
            p->errors++; break;
        }
        if (check(p, TOK_STAR)) { adv(p); is_ptr = 1; ftype = TYPE_PTR; }

        char *fname = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
        expect(p, TOK_SEMICOLON);

        int sz = (ftype == TYPE_STRUCT) ? 8 : 8; /* all fields are 8 bytes for now */
        (void)is_ptr;
        sd->fields[sd->nfields].type        = ftype;
        sd->fields[sd->nfields].name        = fname;
        sd->fields[sd->nfields].struct_name = nested_struct[0] ? strdup(nested_struct) : NULL;
        sd->fields[sd->nfields].offset      = offset;
        sd->nfields++;
        offset += sz;
    }
    expect(p, TOK_RBRACE);
    match(p, TOK_SEMICOLON); /* optional trailing ; after struct body */
    sd->total_size = offset;

    prog->structs = realloc(prog->structs, (prog->nstructs+1)*sizeof(StructDecl*));
    prog->structs[prog->nstructs++] = sd;
}

static void parse_main(Parser *p, Program *prog) {
    adv(p);
    char *name = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);
    expect(p, TOK_RPAREN);

    p->nlocals = 0;
    MainFunc *mf = calloc(1, sizeof(MainFunc));
    mf->name = name;
    parse_body(p, &mf->stmts, &mf->nstmts);
    prog->mainfn = mf;
}

/*
 * parse_func: parses a user-defined function at top level.
 * The return type token has already been consumed into `rettype`.
 *
 *   <rettype> <name> ( [<type> <param>, ...] ) { <body> }
 */
static void parse_func(Parser *p, Program *prog, VarType rettype) {
    /* name */
    char *fname = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);

    /* parameter list */
    int pcap = 8, nparams = 0;
    FuncParam *params = malloc(pcap * sizeof(FuncParam));

    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        if (nparams >= pcap)
            params = realloc(params, (pcap *= 2) * sizeof(FuncParam));
        VarType ptype;
        char pstruct[256] = "";
        if (check(p, TOK_STRUCT)) {
            adv(p);
            strncpy(pstruct, cur(p)->value, sizeof(pstruct)-1);
            expect(p, TOK_IDENT);
            ptype = TYPE_STRUCT;
        } else if (!parse_type_kw(p, &ptype)) {
            fprintf(stderr, "[parser] %d:%d: expected type in parameter list\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            break;
        }
        VarType ptr_base = ptype;
        if (check(p, TOK_STAR)) { adv(p); ptype = TYPE_PTR; }
        params[nparams].type        = ptype;
        params[nparams].ptr_base    = ptr_base;
        params[nparams].struct_name = pstruct[0] ? strdup(pstruct) : NULL;
        params[nparams].name        = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
        nparams++;
        if (!check(p, TOK_RPAREN)) expect(p, TOK_COMMA);
    }
    expect(p, TOK_RPAREN);

    /* reset locals and seed with parameter names so body can reference them */
    p->nlocals = 0;
    for (int i = 0; i < nparams; i++)
        parser_add_var(p, params[i].name);

    FuncDecl *fd  = funcdecl_new();
    fd->name      = fname;
    fd->rettype   = rettype;
    fd->params    = params;
    fd->nparams   = nparams;
    parse_body(p, &fd->stmts, &fd->nstmts);

    prog->funcs = realloc(prog->funcs, (prog->nfuncs+1) * sizeof(FuncDecl *));
    prog->funcs[prog->nfuncs++] = fd;
}

/* ── public API ── */

void parser_init(Parser *p, const char *src) {
    memset(p, 0, sizeof(*p));
    p->src = src;
    lexer_init(&p->lexer, src);
    p->cur = lexer_next(&p->lexer);
    p->nxt = lexer_next(&p->lexer);
}

Program *parser_parse(Parser *p) {
    Program *prog = program_new();
    while (!check(p, TOK_EOF) && !p->errors) {
        if (check(p, TOK_IMPORT)) {
            parse_import(p, prog);
        } else if (check(p, TOK_USING)) {
            parse_using(p, prog);
        } else if (check(p, TOK_STRUCT)) {
            parse_struct(p, prog);
        } else if (check(p, TOK_MAIN)) {
            parse_main(p, prog);
        } else {
            /* Try to parse a user function: starts with a type keyword */
            /* Handle: struct TypeName funcName(...) for struct-returning functions */
            VarType rettype = TYPE_VOID;
            int is_struct_ret = 0;
            char struct_ret_name[256] = "";
            if (check(p, TOK_STRUCT)) {
                adv(p);
                is_struct_ret = 1;
                strncpy(struct_ret_name, cur(p)->value, sizeof(struct_ret_name)-1);
                expect(p, TOK_IDENT);
                rettype = TYPE_STRUCT;
            }
            if (!is_struct_ret && !parse_type_kw(p, &rettype)) {
                fprintf(stderr, "[parser] %d:%d: unexpected top-level token '%s'\n",
                        cur(p)->line, cur(p)->col,
                        cur(p)->value ? cur(p)->value : "?");
                p->errors++;
                break;
            }
            parse_func(p, prog, rettype);
        }
    }
    return prog;
}