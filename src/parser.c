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
static Expr *parse_shift(Parser *p);
static Expr *parse_bitand(Parser *p);
static Expr *parse_bitxor(Parser *p);
static Expr *parse_bitor(Parser *p);
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
        case TOK_VOID:
            /* void* is a generic pointer; plain void is return type only */
            if (p->nxt.type == TOK_STAR) {
                *out = TYPE_VOID_PTR;
                adv(p); /* consume 'void' */
                adv(p); /* consume '*'    */
                return 1;
            }
            *out = TYPE_VOID; break;
        /* IDENT as struct/typedef/enum type name — only when next token is IDENT or * (not . or () */
        case TOK_IDENT:
            if (p->nxt.type == TOK_IDENT || p->nxt.type == TOK_STAR) {
                /* check if it's a typedef alias — resolve to underlying type */
                *out = TYPE_STRUCT; /* default: treat as named type */
                for (int i = 0; i < p->ntypedefs; i++) {
                    if (strcmp(p->typedefs[i]->alias, p->cur.value) == 0) {
                        *out = p->typedefs[i]->base_type;
                        break;
                    }
                }
                break;
            }
            return 0;
        default: return 0;
    }
    adv(p);
    return 1;
}

/* ── instance class lookup ── */
static const char *find_instance_class(Parser *p, const char *varname) {
    for (int i = 0; i < p->ninst; i++)
        if (strcmp(p->inst_names[i], varname) == 0)
            return p->inst_classes[i];
    /* also check 'self' inside a method body */
    if (p->cur_class && strcmp(varname, "self") == 0)
        return p->cur_class->name;
    return NULL;
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
    /* bitwise not: ~expr */
    if (check(p, TOK_TILDE)) {
        adv(p);
        Expr *e = expr_new(EXPR_BITWISE_NOT);
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
    /* parenthesised expr OR type cast: (type)expr OR (type*)expr */
    if (check(p, TOK_LPAREN)) {
        adv(p);
        /* peek: is this a type cast? */
        VarType cast_vt;
        /* capture ident name for struct pointer casts: (Node*) */
        char cast_sname[256] = "";
        if (cur(p)->type == TOK_IDENT)
            strncpy(cast_sname, cur(p)->value, sizeof(cast_sname)-1);
        if (parse_type_kw(p, &cast_vt)) {
            /* handle pointer cast: (int*) (void*) etc. */
            int is_ptr_cast = 0;
            if (check(p, TOK_STAR)) { adv(p); is_ptr_cast = 1; }
            expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_CAST);
            e->cast_type = (int)(is_ptr_cast ? TYPE_PTR : cast_vt);
            e->ptr_base  = cast_vt;
            /* propagate struct name for (Node*) casts */
            if (is_ptr_cast && cast_vt == TYPE_STRUCT && cast_sname[0])
                e->cast_struct_name = strdup(cast_sname);
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
        /* Check: is this IDENT a class instance, and is the next token '.'? */
        const char *inst_cls = find_instance_class(p, cur(p)->value);
        if (inst_cls && p->nxt.type == TOK_DOT) {
            /* obj.member — could be method call or field access */
            char obj_name[256];
            strncpy(obj_name, cur(p)->value, sizeof(obj_name)-1);
            adv(p); /* consume obj name */
            adv(p); /* consume '.' */

            char member_name[256];
            strncpy(member_name, cur(p)->value, sizeof(member_name)-1);
            expect(p, TOK_IDENT);

            if (check(p, TOK_LPAREN)) {
                /* Method call: obj.method(args) →
                 * __class_ClassName_method(&obj, args) */
                adv(p); /* consume '(' */
                int argc = 0;
                Expr **user_args = NULL;
                int cap = 8;
                user_args = malloc(cap * sizeof(Expr *));
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    if (argc >= cap) user_args = realloc(user_args, (cap*=2)*sizeof(Expr*));
                    user_args[argc++] = parse_assign(p);
                    if (!check(p, TOK_RPAREN)) match(p, TOK_COMMA);
                }
                expect(p, TOK_RPAREN);

                /* Build mangled name: __class_ClassName_method */
                char mangled[512];
                snprintf(mangled, sizeof(mangled), "__class_%s_%s", inst_cls, member_name);

                Expr *callee = expr_new(EXPR_IDENT);
                callee->sval = strdup(mangled);

                /* self argument: address of the instance variable */
                Expr *self_arg = expr_new(EXPR_ADDROF);
                Expr *obj_ident = expr_new(EXPR_IDENT);
                obj_ident->sval = strdup(obj_name);
                self_arg->rhs = obj_ident;

                /* prepend self to args */
                Expr **full_args = malloc((argc + 1) * sizeof(Expr *));
                full_args[0] = self_arg;
                for (int i = 0; i < argc; i++) full_args[i+1] = user_args[i];
                free(user_args);

                Expr *call = expr_new(EXPR_CALL);
                call->callee = callee;
                call->args   = full_args;
                call->argc   = argc + 1;
                return call;
            } else {
                /* Field access: obj.field →
                 * just build an EXPR_FIELD (codegen handles it as struct field) */
                Expr *obj_e = expr_new(EXPR_IDENT);
                obj_e->sval = strdup(obj_name);
                Expr *f = expr_new(EXPR_FIELD);
                f->object = obj_e;
                f->field  = strdup(member_name);
                return f;
            }
        }

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

/* ── shift: << >> ── */
static Expr *parse_shift(Parser *p) {
    Expr *left = parse_add(p);
    while (check(p, TOK_SHL) || check(p, TOK_SHR)) {
        char op = check(p, TOK_SHL) ? 'L' : 'R';  /* L=shl, R=shr */
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = op;
        e->lhs = left;
        e->rhs = parse_add(p);
        left = e;
    }
    return left;
}

/* ── bitwise AND: & ── (lower precedence than shift) */
static Expr *parse_bitand(Parser *p) {
    Expr *left = parse_shift(p);
    while (check(p, TOK_AMP)) {
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = '&';
        e->lhs = left;
        e->rhs = parse_shift(p);
        left = e;
    }
    return left;
}

/* ── bitwise XOR: ^ ── */
static Expr *parse_bitxor(Parser *p) {
    Expr *left = parse_bitand(p);
    while (check(p, TOK_CARET)) {
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = '^';
        e->lhs = left;
        e->rhs = parse_bitand(p);
        left = e;
    }
    return left;
}

/* ── bitwise OR: | ── */
static Expr *parse_bitor(Parser *p) {
    Expr *left = parse_bitxor(p);
    while (check(p, TOK_PIPE)) {
        adv(p);
        Expr *e = expr_new(EXPR_BINOP);
        e->op  = '|';
        e->lhs = left;
        e->rhs = parse_bitxor(p);
        left = e;
    }
    return left;
}

/* ── relational and equality comparisons ── */
static Expr *parse_cmp(Parser *p) {
    Expr *left = parse_bitor(p);
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

/* ── assignment: lhs = rhs, lhs += rhs, etc. ── */
static Expr *parse_assign(Parser *p) {
    Expr *e = parse_logical_or(p);
    if (check(p, TOK_ASSIGN)) {
        adv(p);
        Expr *asgn = expr_new(EXPR_ASSIGN);
        asgn->lhs = e;
        asgn->rhs = parse_assign(p);
        return asgn;
    }
    /* compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>= */
    char cop = 0;
    if      (check(p, TOK_PLUS_ASSIGN))    cop = '+';
    else if (check(p, TOK_MINUS_ASSIGN))   cop = '-';
    else if (check(p, TOK_STAR_ASSIGN))    cop = '*';
    else if (check(p, TOK_SLASH_ASSIGN))   cop = '/';
    else if (check(p, TOK_PERCENT_ASSIGN)) cop = '%';
    else if (check(p, TOK_AND_ASSIGN))     cop = '&';
    else if (check(p, TOK_OR_ASSIGN))      cop = '|';
    else if (check(p, TOK_XOR_ASSIGN))     cop = '^';
    else if (check(p, TOK_SHL_ASSIGN))     cop = 'L'; /* << */
    else if (check(p, TOK_SHR_ASSIGN))     cop = 'R'; /* >> */
    if (cop) {
        adv(p);
        Expr *ca = expr_new(EXPR_COMPOUND_ASSIGN);
        ca->op  = cop;
        ca->lhs = e;
        ca->rhs = parse_assign(p);
        return ca;
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

    /* Capture IDENT-based type name before parse_type_kw consumes it.
     * This covers typedef alias names (e.g. Vec2 v;) and plain struct names. */
    if (!is_struct_decl && cur(p)->type == TOK_IDENT) {
        strncpy(struct_type_name, cur(p)->value, sizeof(struct_type_name)-1);
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
        if (is_struct_decl || vt == TYPE_STRUCT) {
            /* resolve typedef alias → underlying struct name if needed */
            const char *resolved_name = struct_type_name;
            for (int i = 0; i < p->ntypedefs; i++) {
                if (strcmp(p->typedefs[i]->alias, struct_type_name) == 0
                        && p->typedefs[i]->base_type == TYPE_STRUCT
                        && p->typedefs[i]->base_name) {
                    resolved_name = p->typedefs[i]->base_name;
                    break;
                }
            }
            s->struct_name = strdup(resolved_name);
        }
        if (check(p, TOK_ASSIGN)) {
            adv(p);
            s->init = parse_expr(p);
        }
        expect(p, TOK_SEMICOLON);
        return s;
    }

    /*
     * Native while(cond) { } — keyword form
     */
    if (check(p, TOK_WHILE)) {
        adv(p);
        expect(p, TOK_LPAREN);
        Stmt *s = stmt_new(STMT_WHILE);
        s->cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        parse_body(p, &s->body, &s->nbody);
        return s;
    }

    /*
     * Native for(cond) { } — keyword form
     */
    if (check(p, TOK_FOR)) {
        adv(p);
        expect(p, TOK_LPAREN);
        Stmt *s = stmt_new(STMT_FOR);
        s->cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        parse_body(p, &s->body, &s->nbody);
        return s;
    }

    /*
     * asm("..."); — inline assembly
     */
    if (check(p, TOK_ASM)) {
        adv(p);
        expect(p, TOK_LPAREN);
        if (!check(p, TOK_STRING_LIT)) {
            fprintf(stderr, "[parser] %d:%d: asm() requires a string literal\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            return stmt_new(STMT_EXPR);
        }
        Stmt *s = stmt_new(STMT_ASM);
        s->asm_code = strdup(cur(p)->value);
        adv(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_SEMICOLON);
        return s;
    }

    /*
     * new ClassName varName;
     */
    if (check(p, TOK_NEW)) {
        adv(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "[parser] %d:%d: expected class name after 'new'\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            return stmt_new(STMT_EXPR);
        }
        Stmt *s = stmt_new(STMT_NEW);
        s->class_name = strdup(cur(p)->value);
        adv(p);
        s->varname = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
        expect(p, TOK_SEMICOLON);
        /* register as instance variable for method dispatch */
        parser_add_var(p, s->varname);
        if (p->ninst < 256) {
            p->inst_names[p->ninst]   = strdup(s->varname);
            p->inst_classes[p->ninst] = strdup(s->class_name);
            p->ninst++;
        }
        return s;
    }

    /*
     * Check for loops.while(...) { } and loops.for(...) { } (legacy form)
     */
    if (check(p, TOK_IDENT) && strcmp(cur(p)->value, "loops") == 0
            && p->nxt.type == TOK_DOT) {
        adv(p); adv(p);
        /* Accept both IDENT ("while"/"for") and keyword tokens TOK_WHILE/TOK_FOR */
        int is_while = check(p, TOK_WHILE) ||
                       (check(p, TOK_IDENT) && strcmp(cur(p)->value, "while") == 0);
        int is_for   = check(p, TOK_FOR)   ||
                       (check(p, TOK_IDENT) && strcmp(cur(p)->value, "for")   == 0);
        if (!is_while && !is_for) {
            fprintf(stderr, "[parser] %d:%d: expected 'while' or 'for' after 'loops.'\n",
                    cur(p)->line, cur(p)->col);
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

    /* Anonymous or named struct */
    char *sname = NULL;
    if (check(p, TOK_IDENT)) {
        sname = strdup(cur(p)->value);
        adv(p);
    }

    /* If there is no body (just a type reference like "struct Foo Bar;"),
     * the caller handles it — we should not be called in that case.
     * But if the next token is NOT '{' we have a forward-decl struct type name:
     * nothing to parse as a declaration here. */
    if (!check(p, TOK_LBRACE)) {
        /* This was called speculatively and is just a struct type reference —
         * put the name back by synthesising: treat as no-op.
         * In practice parser_parse only calls us when TOK_STRUCT is the lookahead
         * and we consumed it already.  Emit an error to be safe. */
        if (!sname) {
            fprintf(stderr, "[parser] %d:%d: expected struct name or '{'\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            free(sname);
            return;
        }
        /* forward declaration — nothing to register, skip optional ; */
        match(p, TOK_SEMICOLON);
        free(sname);
        return;
    }

    expect(p, TOK_LBRACE);

    StructDecl *sd = structdecl_new();
    sd->name = sname ? sname : strdup("__anon");
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

        int sz = 8; /* all fields are 8 bytes */
        (void)is_ptr;
        sd->fields[sd->nfields].type        = ftype;
        sd->fields[sd->nfields].name        = fname;
        sd->fields[sd->nfields].struct_name = nested_struct[0] ? strdup(nested_struct) : NULL;
        sd->fields[sd->nfields].offset      = offset;
        sd->nfields++;
        offset += sz;
    }
    expect(p, TOK_RBRACE);
    sd->total_size = offset;

    /* Optional typedef alias: typedef struct Foo { ... } Bar; */
    if (check(p, TOK_IDENT)) {
        sd->typedef_name = strdup(cur(p)->value);
        /* Register the alias as a typedef pointing at this struct */
        TypedefDecl *td = typedefdecl_new();
        td->alias     = strdup(cur(p)->value);
        td->base_type = TYPE_STRUCT;
        td->base_name = strdup(sd->name);
        if (p->ntypedefs < 128) p->typedefs[p->ntypedefs++] = td;
        prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs + 1) * sizeof(TypedefDecl *));
        prog->typedefs[prog->ntypedefs++] = td;
        adv(p);
    }
    match(p, TOK_SEMICOLON); /* optional trailing ; */

    prog->structs = realloc(prog->structs, (prog->nstructs+1)*sizeof(StructDecl*));
    prog->structs[prog->nstructs++] = sd;
}

static void parse_enum(Parser *p, Program *prog) {
    adv(p); /* consume 'enum' */

    char *ename = NULL;
    if (check(p, TOK_IDENT)) {
        ename = strdup(cur(p)->value);
        adv(p);
    }

    /* If no body — forward declaration, skip */
    if (!check(p, TOK_LBRACE)) {
        match(p, TOK_SEMICOLON);
        free(ename);
        return;
    }
    expect(p, TOK_LBRACE);

    EnumDecl *ed = enumdecl_new();
    ed->name = ename ? ename : strdup("__anon_enum");
    int cap = 8;
    ed->member_names  = malloc(cap * sizeof(char *));
    ed->member_values = malloc(cap * sizeof(long));

    long next_val = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (ed->nmembers >= cap) {
            ed->member_names  = realloc(ed->member_names,  (cap *= 2) * sizeof(char *));
            ed->member_values = realloc(ed->member_values, cap * sizeof(long));
        }
        ed->member_names[ed->nmembers] = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
        /* optional = value */
        if (check(p, TOK_ASSIGN)) {
            adv(p);
            /* support negative literals */
            int neg = 0;
            if (check(p, TOK_MINUS)) { adv(p); neg = 1; }
            long v = atol(cur(p)->value);
            if (neg) v = -v;
            expect(p, TOK_INT_LIT);
            next_val = v;
        }
        ed->member_values[ed->nmembers] = next_val++;
        ed->nmembers++;
        match(p, TOK_COMMA); /* optional comma between members */
    }
    expect(p, TOK_RBRACE);

    /* Optional typedef alias: typedef enum Foo { ... } Bar; */
    if (check(p, TOK_IDENT)) {
        TypedefDecl *td = typedefdecl_new();
        td->alias     = strdup(cur(p)->value);
        td->base_type = TYPE_INT; /* enums are ints */
        td->base_name = strdup(ed->name);
        if (p->ntypedefs < 128) p->typedefs[p->ntypedefs++] = td;
        prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs + 1) * sizeof(TypedefDecl *));
        prog->typedefs[prog->ntypedefs++] = td;
        adv(p);
    }
    match(p, TOK_SEMICOLON);

    prog->enums = realloc(prog->enums, (prog->nenums + 1) * sizeof(EnumDecl *));
    prog->enums[prog->nenums++] = ed;

    /* register in parser for identifier recognition */
    if (p->nenums < 128)
        p->enums[p->nenums++] = ed;
}

static void parse_typedef(Parser *p, Program *prog) {
    adv(p); /* consume 'typedef' */

    /* ── typedef struct ... or typedef enum ... ── */
    if (check(p, TOK_STRUCT) || check(p, TOK_ENUM)) {
        int is_enum = check(p, TOK_ENUM);
        adv(p); /* consume 'struct' or 'enum' */

        /* Read optional name */
        char name_buf[256] = "";
        if (check(p, TOK_IDENT) && p->nxt.type != TOK_SEMICOLON) {
            /* Could be: struct Name { ... } Alias   — has body ahead
             *        or: struct Name Alias;          — forward ref alias */
            strncpy(name_buf, cur(p)->value, sizeof(name_buf)-1);
            adv(p);
        }

        /* If next token is '{', this is a compound struct/enum definition.
         * Reconstruct: push back what we consumed and let parse_struct/enum do it,
         * but since we already consumed the keyword + name, handle inline here. */
        if (check(p, TOK_LBRACE)) {
            /* inline body — parse fields/members ourselves */
            if (!is_enum) {
                /* ── inline struct body ── */
                StructDecl *sd = structdecl_new();
                sd->name = name_buf[0] ? strdup(name_buf) : strdup("__anon_struct");
                int fcap = 8;
                sd->fields = malloc(fcap * sizeof(StructField));
                int offset = 0;
                adv(p); /* consume '{' */
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    if (sd->nfields >= fcap)
                        sd->fields = realloc(sd->fields, (fcap *= 2) * sizeof(StructField));
                    VarType ftype;
                    char nested[256] = "";
                    int is_ptr_field = 0;
                    if (check(p, TOK_STRUCT)) {
                        adv(p);
                        strncpy(nested, cur(p)->value, sizeof(nested)-1);
                        expect(p, TOK_IDENT);
                        ftype = TYPE_STRUCT;
                    } else if (!parse_type_kw(p, &ftype)) {
                        fprintf(stderr, "[parser] %d:%d: expected field type\n",
                                cur(p)->line, cur(p)->col);
                        p->errors++; break;
                    }
                    if (check(p, TOK_STAR)) { adv(p); is_ptr_field = 1; ftype = TYPE_PTR; }
                    (void)is_ptr_field;
                    char *fname = strdup(cur(p)->value);
                    expect(p, TOK_IDENT);
                    expect(p, TOK_SEMICOLON);
                    sd->fields[sd->nfields].type        = ftype;
                    sd->fields[sd->nfields].name        = fname;
                    sd->fields[sd->nfields].struct_name = nested[0] ? strdup(nested) : NULL;
                    sd->fields[sd->nfields].offset      = offset;
                    sd->nfields++;
                    offset += 8;
                }
                expect(p, TOK_RBRACE);
                sd->total_size = offset;
                prog->structs = realloc(prog->structs, (prog->nstructs+1)*sizeof(StructDecl*));
                prog->structs[prog->nstructs++] = sd;

                /* trailing alias name */
                if (check(p, TOK_IDENT)) {
                    sd->typedef_name = strdup(cur(p)->value);
                    TypedefDecl *td = typedefdecl_new();
                    td->alias     = strdup(cur(p)->value);
                    td->base_type = TYPE_STRUCT;
                    td->base_name = strdup(sd->name);
                    if (p->ntypedefs < 128) p->typedefs[p->ntypedefs++] = td;
                    prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs+1)*sizeof(TypedefDecl*));
                    prog->typedefs[prog->ntypedefs++] = td;
                    adv(p);
                }
                match(p, TOK_SEMICOLON);
            } else {
                /* ── inline enum body ── */
                EnumDecl *ed = enumdecl_new();
                ed->name = name_buf[0] ? strdup(name_buf) : strdup("__anon_enum");
                int cap = 8;
                ed->member_names  = malloc(cap * sizeof(char *));
                ed->member_values = malloc(cap * sizeof(long));
                long next_val = 0;
                adv(p); /* consume '{' */
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    if (ed->nmembers >= cap) {
                        ed->member_names  = realloc(ed->member_names,  (cap*=2)*sizeof(char*));
                        ed->member_values = realloc(ed->member_values, cap*sizeof(long));
                    }
                    ed->member_names[ed->nmembers] = strdup(cur(p)->value);
                    expect(p, TOK_IDENT);
                    if (check(p, TOK_ASSIGN)) {
                        adv(p);
                        int neg = check(p, TOK_MINUS); if (neg) adv(p);
                        long v = atol(cur(p)->value); if (neg) v = -v;
                        expect(p, TOK_INT_LIT);
                        next_val = v;
                    }
                    ed->member_values[ed->nmembers] = next_val++;
                    ed->nmembers++;
                    match(p, TOK_COMMA);
                }
                expect(p, TOK_RBRACE);
                prog->enums = realloc(prog->enums, (prog->nenums+1)*sizeof(EnumDecl*));
                prog->enums[prog->nenums++] = ed;
                if (p->nenums < 128) p->enums[p->nenums++] = ed;

                /* trailing alias name */
                if (check(p, TOK_IDENT)) {
                    TypedefDecl *td = typedefdecl_new();
                    td->alias     = strdup(cur(p)->value);
                    td->base_type = TYPE_INT;
                    td->base_name = strdup(ed->name);
                    if (p->ntypedefs < 128) p->typedefs[p->ntypedefs++] = td;
                    prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs+1)*sizeof(TypedefDecl*));
                    prog->typedefs[prog->ntypedefs++] = td;
                    adv(p);
                }
                match(p, TOK_SEMICOLON);
            }
            return;
        }

        /* No brace — forward-ref form: typedef struct Name Alias; */
        /* name_buf holds the struct/enum name; cur token is the alias IDENT */
        if (name_buf[0] && check(p, TOK_IDENT)) {
            TypedefDecl *td = typedefdecl_new();
            td->alias     = strdup(cur(p)->value);
            td->base_type = is_enum ? TYPE_INT : TYPE_STRUCT;
            td->base_name = strdup(name_buf);
            if (p->ntypedefs < 128) p->typedefs[p->ntypedefs++] = td;
            prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs+1)*sizeof(TypedefDecl*));
            prog->typedefs[prog->ntypedefs++] = td;
            adv(p);
        }
        match(p, TOK_SEMICOLON);
        return;
    }

    /* ── Simple alias form: typedef int MyInt;  typedef void* Handle; etc. ── */
    TypedefDecl *td = typedefdecl_new();
    char base_name_buf[256] = "";

    VarType bt = TYPE_INT;
    if (!parse_type_kw(p, &bt)) {
        fprintf(stderr, "[parser] %d:%d: expected type after 'typedef'\n",
                cur(p)->line, cur(p)->col);
        p->errors++;
        free(td);
        return;
    }
    /* handle void* */
    if (bt == TYPE_VOID && check(p, TOK_STAR)) { adv(p); bt = TYPE_VOID_PTR; }
    td->base_type = bt;
    td->base_name = base_name_buf[0] ? strdup(base_name_buf) : NULL;

    /* optional * for pointer typedef */
    if (check(p, TOK_STAR)) {
        adv(p);
        if (td->base_type != TYPE_VOID_PTR) td->base_type = TYPE_PTR;
    }

    td->alias = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_SEMICOLON);

    /* register alias in parser for use in variable declarations */
    if (p->ntypedefs < 128)
        p->typedefs[p->ntypedefs++] = td;

    prog->typedefs = realloc(prog->typedefs, (prog->ntypedefs + 1) * sizeof(TypedefDecl *));
    prog->typedefs[prog->ntypedefs++] = td;
}

static void parse_main(Parser *p, Program *prog) {
    adv(p); /* consume 'main' */
    if (prog->mainfn) {
        fprintf(stderr, "[parser] duplicate main block\n");
        p->errors++;
        return;
    }
    MainFunc *mf = calloc(1, sizeof(MainFunc));
    mf->name = strdup(cur(p)->value);
    expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);
    expect(p, TOK_RPAREN);
    p->nlocals = 0;
    parse_body(p, &mf->stmts, &mf->nstmts);
    prog->mainfn = mf;
}

/*
 * parse_class: class Name [extends Parent] {
 *     public  { <fields and methods> }
 *     private { <fields and methods> }
 * }
 *
 * Each method is lowered to a FuncDecl:
 *   __class_ClassName_methodName(ClassName* self, ...)
 * Fields are lowered to a StructDecl named ClassName.
 * Inheritance prepends parent fields and shares parent method bodies.
 */
static void parse_class(Parser *p, Program *prog) {
    adv(p); /* consume 'class' */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "[parser] %d:%d: expected class name\n",
                cur(p)->line, cur(p)->col);
        p->errors++;
        return;
    }
    ClassDecl *cd = classdecl_new();
    cd->name = strdup(cur(p)->value);
    adv(p);

    /* optional: extends ParentName */
    if (check(p, TOK_EXTENDS)) {
        adv(p);
        cd->extends_name = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
    }

    /* also accept: class Name : Parent */
    if (check(p, TOK_COLON)) {
        adv(p);
        cd->extends_name = strdup(cur(p)->value);
        expect(p, TOK_IDENT);
    }

    expect(p, TOK_LBRACE);

    int field_cap = 16, method_cap = 16;
    cd->fields  = malloc(field_cap  * sizeof(ClassField));
    cd->methods = malloc(method_cap * sizeof(ClassMethod));

    /* Register class in parser so 'new ClassName' can find it */
    if (p->nclasses < 64)
        p->classes[p->nclasses++] = cd;
    prog->classes = realloc(prog->classes, (prog->nclasses+1)*sizeof(ClassDecl*));
    prog->classes[prog->nclasses++] = cd;

    /* Set cur_class so 'self' resolves inside method bodies */
    p->cur_class = cd;

    /* Parse public/private blocks */
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int is_private = 0;
        if (check(p, TOK_PUBLIC)) {
            adv(p);
        } else if (check(p, TOK_PRIVATE)) {
            adv(p);
            is_private = 1;
        } else {
            fprintf(stderr, "[parser] %d:%d: expected 'public' or 'private' in class body\n",
                    cur(p)->line, cur(p)->col);
            p->errors++;
            break;
        }
        expect(p, TOK_LBRACE);

        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            int meth_static = 0;
            if (check(p, TOK_STATIC)) { meth_static = 1; adv(p); }

            /* Parse return/field type */
            VarType vt = TYPE_VOID;
            char type_name_buf[256] = "";
            int is_struct_type = 0;
            if (check(p, TOK_STRUCT)) {
                adv(p);
                strncpy(type_name_buf, cur(p)->value, sizeof(type_name_buf)-1);
                expect(p, TOK_IDENT);
                vt = TYPE_STRUCT;
                is_struct_type = 1;
            } else if (!parse_type_kw(p, &vt)) {
                break;
            }

            /* pointer return/field type */
            if (check(p, TOK_STAR)) { adv(p); vt = TYPE_PTR; }

            char member_name[256];
            strncpy(member_name, cur(p)->value, sizeof(member_name)-1);
            expect(p, TOK_IDENT);

            if (check(p, TOK_LPAREN)) {
                /* ── Method ── */
                adv(p);

                if (cd->nmethods >= method_cap)
                    cd->methods = realloc(cd->methods, (method_cap*=2)*sizeof(ClassMethod));

                int pcap = 8, nparams = 0;
                FuncParam *params = malloc(pcap * sizeof(FuncParam));

                /* implicit first param: ClassName* self (skip for static methods) */
                if (!meth_static) {
                    params[nparams].type        = TYPE_PTR;
                    params[nparams].ptr_base    = TYPE_STRUCT;
                    params[nparams].name        = strdup("self");
                    params[nparams].struct_name = strdup(cd->name);
                    nparams++;
                }

                /* user params */
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    if (nparams >= pcap)
                        params = realloc(params, (pcap*=2)*sizeof(FuncParam));
                    VarType ptype = TYPE_INT;
                    char pstruct[256] = "";
                    if (check(p, TOK_STRUCT)) {
                        adv(p);
                        strncpy(pstruct, cur(p)->value, sizeof(pstruct)-1);
                        expect(p, TOK_IDENT);
                        ptype = TYPE_STRUCT;
                    } else {
                        if (cur(p)->type == TOK_IDENT)
                            strncpy(pstruct, cur(p)->value, sizeof(pstruct)-1);
                        if (!parse_type_kw(p, &ptype)) break;
                        if (ptype != TYPE_STRUCT) pstruct[0] = '\0';
                    }
                    if (check(p, TOK_STAR)) { adv(p); ptype = TYPE_PTR; }
                    params[nparams].type        = ptype;
                    params[nparams].ptr_base    = ptype;
                    params[nparams].struct_name = pstruct[0] ? strdup(pstruct) : NULL;
                    params[nparams].name        = strdup(cur(p)->value);
                    expect(p, TOK_IDENT);
                    nparams++;
                    if (!check(p, TOK_RPAREN)) match(p, TOK_COMMA);
                }
                expect(p, TOK_RPAREN);

                /* save locals state, parse body */
                p->nlocals = 0;
                for (int i = 0; i < nparams; i++)
                    parser_add_var(p, params[i].name);

                ClassMethod *cm = &cd->methods[cd->nmethods++];
                cm->name       = strdup(member_name);
                cm->rettype    = vt;
                cm->nparams    = nparams;
                cm->params     = params;
                cm->is_private = is_private;
                cm->is_static  = meth_static;
                parse_body(p, &cm->stmts, &cm->nstmts);

                /* lower to FuncDecl with mangled name */
                char mangled[512];
                snprintf(mangled, sizeof(mangled), "__class_%s_%s", cd->name, member_name);

                FuncDecl *fd  = funcdecl_new();
                fd->name      = strdup(mangled);
                fd->rettype   = vt;
                fd->params    = (FuncParam *)cm->params;
                fd->nparams   = nparams;
                fd->stmts     = cm->stmts;
                fd->nstmts    = cm->nstmts;
                fd->is_static = 1;
                prog->funcs = realloc(prog->funcs, (prog->nfuncs+1)*sizeof(FuncDecl*));
                prog->funcs[prog->nfuncs++] = fd;

            } else {
                /* ── Field ── */
                if (check(p, TOK_SEMICOLON)) adv(p);
                if (cd->nfields >= field_cap)
                    cd->fields = realloc(cd->fields, (field_cap*=2)*sizeof(ClassField));
                cd->fields[cd->nfields].type        = vt;
                cd->fields[cd->nfields].name        = strdup(member_name);
                cd->fields[cd->nfields].struct_name = (is_struct_type && type_name_buf[0])
                                                      ? strdup(type_name_buf) : NULL;
                cd->fields[cd->nfields].is_private  = is_private;
                cd->nfields++;
            }
        }
        expect(p, TOK_RBRACE); /* close public/private block */
    }
    expect(p, TOK_RBRACE); /* close class block */
    match(p, TOK_SEMICOLON);

    p->cur_class = NULL;

    /* ── Inheritance: prepend parent fields, inherit un-overridden methods ── */
    if (cd->extends_name) {
        ClassDecl *parent = NULL;
        for (int i = 0; i < p->nclasses - 1; i++)
            if (strcmp(p->classes[i]->name, cd->extends_name) == 0) { parent = p->classes[i]; break; }
        if (parent) {
            /* Prepend parent fields to child fields */
            int total = parent->nfields + cd->nfields;
            ClassField *merged = malloc(total * sizeof(ClassField));
            for (int i = 0; i < parent->nfields; i++) {
                merged[i].type        = parent->fields[i].type;
                merged[i].name        = strdup(parent->fields[i].name);
                merged[i].struct_name = parent->fields[i].struct_name
                                        ? strdup(parent->fields[i].struct_name) : NULL;
                merged[i].is_private  = parent->fields[i].is_private;
            }
            for (int i = 0; i < cd->nfields; i++)
                merged[parent->nfields + i] = cd->fields[i];
            free(cd->fields);
            cd->fields  = merged;
            cd->nfields = total;

            /* Inherit parent methods not overridden in child */
            for (int pi = 0; pi < parent->nmethods; pi++) {
                int found = 0;
                for (int ci = 0; ci < cd->nmethods; ci++)
                    if (strcmp(cd->methods[ci].name, parent->methods[pi].name) == 0)
                        { found = 1; break; }
                if (!found) {
                    char child_mangled[512], parent_mangled[512];
                    snprintf(child_mangled,  sizeof(child_mangled),
                             "__class_%s_%s", cd->name, parent->methods[pi].name);
                    snprintf(parent_mangled, sizeof(parent_mangled),
                             "__class_%s_%s", cd->extends_name, parent->methods[pi].name);

                    int already = 0;
                    for (int k = 0; k < prog->nfuncs; k++)
                        if (strcmp(prog->funcs[k]->name, child_mangled) == 0)
                            { already = 1; break; }
                    if (!already) {
                        FuncDecl *fd = funcdecl_new();
                        fd->name      = strdup(child_mangled);
                        fd->rettype   = parent->methods[pi].rettype;
                        /* Share params/stmts with parent — is_alias prevents double-free */
                        fd->params    = (FuncParam *)parent->methods[pi].params;
                        fd->nparams   = parent->methods[pi].nparams;
                        fd->stmts     = parent->methods[pi].stmts;
                        fd->nstmts    = parent->methods[pi].nstmts;
                        fd->is_static = 1;
                        fd->is_alias  = 1;  /* do NOT free stmts/params in program_free */
                        prog->funcs = realloc(prog->funcs, (prog->nfuncs+1)*sizeof(FuncDecl*));
                        prog->funcs[prog->nfuncs++] = fd;
                    }
                }
            }
        } else {
            fprintf(stderr, "[parser] warning: class '%s' extends unknown class '%s'\n",
                    cd->name, cd->extends_name);
        }
    }

    /* ── Lower class fields to a StructDecl for codegen layout ── */
    StructDecl *sd = structdecl_new();
    sd->name   = strdup(cd->name);
    sd->fields = malloc((cd->nfields > 0 ? cd->nfields : 1) * sizeof(StructField));
    int offset = 0;
    for (int i = 0; i < cd->nfields; i++) {
        sd->fields[i].type        = cd->fields[i].type;
        sd->fields[i].name        = strdup(cd->fields[i].name);
        sd->fields[i].struct_name = cd->fields[i].struct_name
                                    ? strdup(cd->fields[i].struct_name) : NULL;
        sd->fields[i].offset      = offset;
        sd->nfields++;
        offset += 8;
    }
    sd->total_size = offset > 0 ? offset : 8;
    prog->structs = realloc(prog->structs, (prog->nstructs+1)*sizeof(StructDecl*));
    prog->structs[prog->nstructs++] = sd;
}


static void parse_func(Parser *p, Program *prog, VarType rettype,
                        int is_static, int is_inline) {
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
        } else {
            /* Capture IDENT type name before parse_type_kw consumes it */
            if (cur(p)->type == TOK_IDENT)
                strncpy(pstruct, cur(p)->value, sizeof(pstruct)-1);
            if (!parse_type_kw(p, &ptype)) {
                fprintf(stderr, "[parser] %d:%d: expected type in parameter list\n",
                        cur(p)->line, cur(p)->col);
                p->errors++;
                break;
            }
            /* Resolve typedef alias → underlying struct name */
            if (ptype == TYPE_STRUCT && pstruct[0]) {
                for (int i = 0; i < p->ntypedefs; i++) {
                    if (strcmp(p->typedefs[i]->alias, pstruct) == 0
                            && p->typedefs[i]->base_type == TYPE_STRUCT
                            && p->typedefs[i]->base_name) {
                        strncpy(pstruct, p->typedefs[i]->base_name, sizeof(pstruct)-1);
                        break;
                    }
                }
            } else {
                pstruct[0] = '\0'; /* clear for non-struct types */
            }
        }
        VarType ptr_base = ptype;
        if (check(p, TOK_STAR)) { adv(p); ptype = TYPE_PTR; }
        /* array parameter: type[] name — treat as pointer to first elem */
        int param_is_array = 0;
        if (!check(p, TOK_STAR) && check(p, TOK_LBRACKET)) {
            adv(p); /* [ */
            if (check(p, TOK_RBRACKET)) adv(p); /* ] */
            param_is_array = 1;
        }
        params[nparams].type        = ptype;
        params[nparams].ptr_base    = ptr_base;
        params[nparams].struct_name = pstruct[0] ? strdup(pstruct) : NULL;
        params[nparams].name        = strdup(cur(p)->value);
        params[nparams].is_array    = param_is_array;
        expect(p, TOK_IDENT);
        nparams++;
        if (!check(p, TOK_RPAREN)) expect(p, TOK_COMMA);
    }
    expect(p, TOK_RPAREN);

    FuncDecl *fd  = funcdecl_new();
    fd->name      = fname;
    fd->rettype   = rettype;
    fd->params    = params;
    fd->nparams   = nparams;
    fd->is_static = is_static;
    fd->is_inline = is_inline;

    /* Forward declaration -- semicolon instead of body */
    if (check(p, TOK_SEMICOLON)) {
        adv(p);
        fd->is_extern = 1;
        prog->funcs = realloc(prog->funcs, (prog->nfuncs+1) * sizeof(FuncDecl *));
        prog->funcs[prog->nfuncs++] = fd;
        return;
    }

    /* reset locals and seed with parameter names so body can reference them */
    p->nlocals = 0;
    for (int i = 0; i < nparams; i++)
        parser_add_var(p, params[i].name);

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
        } else if (check(p, TOK_ENUM)) {
            parse_enum(p, prog);
        } else if (check(p, TOK_TYPEDEF)) {
            parse_typedef(p, prog);
        } else if (check(p, TOK_CLASS)) {
            parse_class(p, prog);
        } else if (check(p, TOK_MAIN)) {
            parse_main(p, prog);
        } else {
            /* Try to parse a user function.
             * Optional qualifiers: static, inline (any order, any combination) */
            int is_static = 0, is_inline = 0;
            while (check(p, TOK_STATIC) || check(p, TOK_INLINE)) {
                if (check(p, TOK_STATIC)) { is_static = 1; adv(p); }
                else                      { is_inline = 1; adv(p); }
            }

            /* Return type: handle struct TypeName funcName(...) */
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
            parse_func(p, prog, rettype, is_static, is_inline);
        }
    }
    return prog;
}