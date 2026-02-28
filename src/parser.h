#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer       lexer;
    Token       cur;
    Token       nxt;
    int         errors;
    const char *src;
    char    *locals[256];
    int      nlocals;
    TypedefDecl *typedefs[128];
    int          ntypedefs;
    EnumDecl    *enums[128];
    int          nenums;
    ClassDecl   *classes[64];
    int          nclasses;
    char        *inst_names[256];
    char        *inst_classes[256];
    int          ninst;
    ClassDecl   *cur_class;
} Parser;

void     parser_init(Parser *p, const char *src);
Program *parser_parse(Parser *p);

#endif