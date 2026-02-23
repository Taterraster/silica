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
} Parser;

void     parser_init(Parser *p, const char *src);
Program *parser_parse(Parser *p);

#endif