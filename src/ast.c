#define _GNU_SOURCE
#include "ast.h"
#include <stdlib.h>
#include <string.h>

Expr *expr_new(ExprKind k) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = k;
    return e;
}

Stmt *stmt_new(StmtKind k) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = k;
    return s;
}

FuncDecl *funcdecl_new(void) {
    return calloc(1, sizeof(FuncDecl));
}

StructDecl *structdecl_new(void) {
    return calloc(1, sizeof(StructDecl));
}

EnumDecl *enumdecl_new(void) {
    return calloc(1, sizeof(EnumDecl));
}

TypedefDecl *typedefdecl_new(void) {
    return calloc(1, sizeof(TypedefDecl));
}

ClassDecl *classdecl_new(void) {
    return calloc(1, sizeof(ClassDecl));
}

Program *program_new(void) {
    return calloc(1, sizeof(Program));
}

static void expr_free(Expr *e) {
    if (!e) return;
    free(e->sval);
    free(e->field);
    free(e->ptr_field);
    expr_free(e->object);
    expr_free(e->callee);
    for (int i = 0; i < e->argc; i++) expr_free(e->args[i]);
    free(e->args);
    expr_free(e->lhs);
    expr_free(e->rhs);
    free(e);
}

static void stmt_free(Stmt *s) {
    if (!s) return;
    free(s->varname);
    free(s->struct_name);
    free(s->asm_code);
    free(s->class_name);
    expr_free(s->init);
    expr_free(s->expr);
    expr_free(s->cond);
    for (int i = 0; i < s->nbody;     i++) stmt_free(s->body[i]);
    for (int i = 0; i < s->nelsebody; i++) stmt_free(s->elsebody[i]);
    free(s->body);
    free(s->elsebody);
    free(s);
}

void program_free(Program *p) {
    if (!p) return;
    for (int i = 0; i < p->nimports; i++) free(p->imports[i].module);
    free(p->imports);
    for (int i = 0; i < p->nusings; i++) free(p->usings[i].ns);
    free(p->usings);
    for (int i = 0; i < p->nstructs; i++) {
        StructDecl *sd = p->structs[i];
        free(sd->name);
        for (int j = 0; j < sd->nfields; j++) {
            free(sd->fields[j].name);
            free(sd->fields[j].struct_name);
        }
        free(sd->fields);
        free(sd);
    }
    free(p->structs);
    for (int i = 0; i < p->nenums; i++) {
        EnumDecl *ed = p->enums[i];
        free(ed->name);
        for (int j = 0; j < ed->nmembers; j++) free(ed->member_names[j]);
        free(ed->member_names);
        free(ed->member_values);
        free(ed);
    }
    free(p->enums);
    for (int i = 0; i < p->ntypedefs; i++) {
        TypedefDecl *td = p->typedefs[i];
        free(td->alias);
        free(td->base_name);
        free(td);
    }
    free(p->typedefs);
    for (int i = 0; i < p->nclasses; i++) {
        ClassDecl *cd = p->classes[i];
        free(cd->name);
        free(cd->extends_name);
        for (int j = 0; j < cd->nfields; j++) {
            free(cd->fields[j].name);
            free(cd->fields[j].struct_name);
        }
        free(cd->fields);
        for (int j = 0; j < cd->nmethods; j++) {
            ClassMethod *cm = &cd->methods[j];
            free(cm->name);
            /* NOTE: cm->params and cm->stmts are shared with the corresponding
             * FuncDecl in prog->funcs, which owns and frees them.
             * Do NOT free them here to avoid double-free. */
        }
        free(cd->methods);
        free(cd);
    }
    free(p->classes);
    if (p->mainfn) {
        free(p->mainfn->name);
        for (int i = 0; i < p->mainfn->nstmts; i++) stmt_free(p->mainfn->stmts[i]);
        free(p->mainfn->stmts);
        free(p->mainfn);
    }
    for (int i = 0; i < p->nfuncs; i++) {
        FuncDecl *f = p->funcs[i];
        if (!f) continue;
        free(f->name);
        /* is_alias: stmts and params are shared with parent class — do NOT free them */
        if (!f->is_alias) {
            for (int j = 0; j < f->nparams; j++) {
                free(f->params[j].name);
                free(f->params[j].struct_name);
            }
            free(f->params);
            for (int j = 0; j < f->nstmts; j++) stmt_free(f->stmts[j]);
            free(f->stmts);
        }
        free(f);
    }
    free(p->funcs);
    free(p);
}