#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer    lexer;
    Token    cur;
    Token    nxt;
    int      errors;
	const char *src;
    char    *locals[256];
    int      nlocals;
} Parser;

void     parser_init(Parser *p, const char *src);
Program *parser_parse(Parser *p);

#endif