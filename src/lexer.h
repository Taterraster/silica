#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
    TOK_INT_LIT, TOK_FLOAT_LIT, TOK_CHAR_LIT, TOK_STRING_LIT,
    TOK_TRUE, TOK_FALSE,
    TOK_IMPORT, TOK_USING, TOK_MAIN,
    TOK_INT, TOK_CHAR, TOK_STRING,
    TOK_BOOL, TOK_FLOAT, TOK_LONG, TOK_BYTE, TOK_UINT, TOK_VOID,
    TOK_RETURN,
    TOK_IF, TOK_ELSE,
    TOK_CONST,
    TOK_BREAK, TOK_CONTINUE,
    TOK_IDENT,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_TYPEDEF,
    TOK_STATIC,
    TOK_INLINE,
    TOK_FOR,
    TOK_WHILE,
    TOK_ASM,
    TOK_CLASS,
    TOK_PUBLIC,
    TOK_PRIVATE,
    TOK_NEW,
    TOK_EXTENDS,
    TOK_COLON,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMICOLON, TOK_DOT, TOK_COMMA, TOK_ASSIGN,
    TOK_AMP,    /* &  — address-of / bitwise-and */
    TOK_ARROW,  /* -> — pointer field access     */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NEQ,
    TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
    TOK_AND, TOK_OR, TOK_BANG,
    TOK_EOF, TOK_ERROR
} TokenType;

typedef struct {
    TokenType  type;
    char      *value;
    int        line, col;
} Token;

typedef struct {
    const char *src;
    size_t      pos;
    int         line, col;
} Lexer;

void        lexer_init(Lexer *l, const char *src);
Token       lexer_next(Lexer *l);
void        token_free(Token *t);
const char *token_type_name(TokenType t);

#endif