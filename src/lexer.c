#define _GNU_SOURCE
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void lexer_init(Lexer *l, const char *src) {
    l->src  = src;
    l->pos  = 0;
    l->line = 1;
    l->col  = 1;
}

static char peek(Lexer *l)  { return l->src[l->pos]; }
static char peek2(Lexer *l) { return l->src[l->pos] ? l->src[l->pos+1] : 0; }

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; }
    else            l->col++;
    return c;
}

static void skip_whitespace_comments(Lexer *l) {
    for (;;) {
        while (isspace((unsigned char)peek(l))) advance(l);
        if (peek(l) == '/' && peek2(l) == '/') {
            while (peek(l) && peek(l) != '\n') advance(l);
        } else break;
    }
}

static Token mktok(TokenType t, char *val, int line, int col) {
    Token tok; tok.type = t; tok.value = val; tok.line = line; tok.col = col;
    return tok;
}

static Token lex_string(Lexer *l, int line, int col) {
    advance(l); /* consume opening " */
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (peek(l) && peek(l) != '"') {
        char c = advance(l);
        if (c == '\\') {
            switch (advance(l)) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '0':  c = '\0'; break;
                default:   c = '?';
            }
        }
        if (len + 2 >= cap) buf = realloc(buf, cap *= 2);
        buf[len++] = c;
    }
    if (peek(l) == '"') advance(l);
    buf[len] = '\0';
    return mktok(TOK_STRING_LIT, buf, line, col);
}

static Token lex_char(Lexer *l, int line, int col) {
    advance(l); /* consume ' */
    char c = advance(l);
    if (c == '\\') {
        switch (advance(l)) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case '\'': c = '\''; break;
            case '\\': c = '\\'; break;
            case '0':  c = '\0'; break;
        }
    }
    if (peek(l) == '\'') advance(l);
    char *buf = malloc(2); buf[0] = c; buf[1] = '\0';
    return mktok(TOK_CHAR_LIT, buf, line, col);
}

static Token lex_number(Lexer *l, int line, int col) {
    size_t start = l->pos;
    while (isdigit((unsigned char)peek(l))) advance(l);
    /* check for float: digits '.' digits */
    if (peek(l) == '.' && isdigit((unsigned char)l->src[l->pos+1])) {
        advance(l); /* consume '.' */
        while (isdigit((unsigned char)peek(l))) advance(l);
        size_t len = l->pos - start;
        char *buf = malloc(len + 1);
        memcpy(buf, l->src + start, len);
        buf[len] = '\0';
        return mktok(TOK_FLOAT_LIT, buf, line, col);
    }
    size_t len = l->pos - start;
    char *buf = malloc(len + 1);
    memcpy(buf, l->src + start, len);
    buf[len] = '\0';
    return mktok(TOK_INT_LIT, buf, line, col);
}

static struct { const char *kw; TokenType t; } kwtab[] = {
    {"import", TOK_IMPORT}, {"using",  TOK_USING}, {"main",   TOK_MAIN},
    {"int",    TOK_INT},    {"char",   TOK_CHAR},  {"string", TOK_STRING},
    {"bool",   TOK_BOOL},   {"float",  TOK_FLOAT}, {"long",   TOK_LONG},
    {"byte",   TOK_BYTE},   {"uint",   TOK_UINT},  {"void",   TOK_VOID},
    {"struct", TOK_STRUCT},
    {"enum",   TOK_ENUM},
    {"typedef",TOK_TYPEDEF},
    {"static", TOK_STATIC},
    {"inline", TOK_INLINE},
    {"true",   TOK_TRUE},   {"false",  TOK_FALSE},
    {"return", TOK_RETURN},
    {"if",     TOK_IF},     {"else",   TOK_ELSE},
    {"const",  TOK_CONST},
    {"break",  TOK_BREAK},  {"continue", TOK_CONTINUE},
    {NULL, 0}
};

static Token lex_ident(Lexer *l, int line, int col) {
    size_t start = l->pos;
    while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    size_t len = l->pos - start;
    char *buf = malloc(len + 1);
    memcpy(buf, l->src + start, len);
    buf[len] = '\0';
    for (int i = 0; kwtab[i].kw; i++)
        if (strcmp(buf, kwtab[i].kw) == 0)
            return mktok(kwtab[i].t, buf, line, col);
    return mktok(TOK_IDENT, buf, line, col);
}

Token lexer_next(Lexer *l) {
    skip_whitespace_comments(l);
    int line = l->line, col = l->col;
    char c = peek(l);

    if (!c)          return mktok(TOK_EOF,  strdup("EOF"), line, col);
    if (c == '"')    return lex_string(l, line, col);
    if (c == '\'')   return lex_char(l, line, col);
    if (isdigit((unsigned char)c)) return lex_number(l, line, col);
    if (isalpha((unsigned char)c) || c == '_') return lex_ident(l, line, col);

    advance(l);
    switch (c) {
        case '(': return mktok(TOK_LPAREN,    strdup("("), line, col);
        case ')': return mktok(TOK_RPAREN,    strdup(")"), line, col);
        case '{': return mktok(TOK_LBRACE,    strdup("{"), line, col);
        case '}': return mktok(TOK_RBRACE,    strdup("}"), line, col);
        case '[': return mktok(TOK_LBRACKET,  strdup("["), line, col);
        case ']': return mktok(TOK_RBRACKET,  strdup("]"), line, col);
        case ';': return mktok(TOK_SEMICOLON, strdup(";"), line, col);
        case '.': return mktok(TOK_DOT,       strdup("."), line, col);
        case ',': return mktok(TOK_COMMA,     strdup(","), line, col);
        case '=':
            if (peek(l) == '=') { advance(l); return mktok(TOK_EQ,  strdup("=="), line, col); }
            return mktok(TOK_ASSIGN, strdup("="), line, col);
        case '!':
            if (peek(l) == '=') { advance(l); return mktok(TOK_NEQ, strdup("!="), line, col); }
            return mktok(TOK_BANG, strdup("!"), line, col);
        case '&':
            if (peek(l) == '&') { advance(l); return mktok(TOK_AND, strdup("&&"), line, col); }
            return mktok(TOK_AMP, strdup("&"), line, col);
        case '-':
            if (peek(l) == '>') { advance(l); return mktok(TOK_ARROW, strdup("->"), line, col); }
            return mktok(TOK_MINUS, strdup("-"), line, col);
        case '|':
            if (peek(l) == '|') { advance(l); return mktok(TOK_OR,  strdup("||"), line, col); }
            goto lex_error;
        case '<':
            if (peek(l) == '=') { advance(l); return mktok(TOK_LTE, strdup("<="), line, col); }
            return mktok(TOK_LT, strdup("<"), line, col);
        case '>':
            if (peek(l) == '=') { advance(l); return mktok(TOK_GTE, strdup(">="), line, col); }
            return mktok(TOK_GT, strdup(">"), line, col);
        case '+': return mktok(TOK_PLUS,      strdup("+"), line, col);
        case '*': return mktok(TOK_STAR,      strdup("*"), line, col);
        case '/': return mktok(TOK_SLASH,     strdup("/"), line, col);
        case '%': return mktok(TOK_PERCENT,   strdup("%"), line, col);
        default: lex_error: {
            char *buf = malloc(2); buf[0] = c; buf[1] = '\0';
            fprintf(stderr, "[lexer] unknown char '%c' at %d:%d\n", c, line, col);
            return mktok(TOK_ERROR, buf, line, col);
        }
    }
}

void token_free(Token *t) { free(t->value); t->value = NULL; }

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_INT_LIT:    return "INT_LIT";
        case TOK_FLOAT_LIT:  return "FLOAT_LIT";
        case TOK_CHAR_LIT:   return "CHAR_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_TRUE:       return "true";
        case TOK_FALSE:      return "false";
        case TOK_IMPORT:     return "import";
        case TOK_USING:      return "using";
        case TOK_MAIN:       return "main";
        case TOK_INT:        return "int";
        case TOK_CHAR:       return "char";
        case TOK_STRING:     return "string";
        case TOK_BOOL:       return "bool";
        case TOK_FLOAT:      return "float";
        case TOK_LONG:       return "long";
        case TOK_BYTE:       return "byte";
        case TOK_UINT:       return "uint";
        case TOK_VOID:       return "void";
        case TOK_RETURN:     return "return";
        case TOK_IF:         return "if";
        case TOK_ELSE:       return "else";
        case TOK_CONST:      return "const";
        case TOK_BREAK:      return "break";
        case TOK_CONTINUE:   return "continue";
        case TOK_IDENT:      return "IDENT";
        case TOK_LPAREN:     return "(";
        case TOK_RPAREN:     return ")";
        case TOK_LBRACE:     return "{";
        case TOK_RBRACE:     return "}";
        case TOK_LBRACKET:   return "[";
        case TOK_RBRACKET:   return "]";
        case TOK_SEMICOLON:  return ";";
        case TOK_DOT:        return ".";
        case TOK_COMMA:      return ",";
        case TOK_ASSIGN:     return "=";
        case TOK_PLUS:       return "+";
        case TOK_MINUS:      return "-";
        case TOK_STAR:       return "*";
        case TOK_SLASH:      return "/";
        case TOK_PERCENT:    return "%";
        case TOK_EQ:         return "==";
        case TOK_NEQ:        return "!=";
        case TOK_LT:         return "<";
        case TOK_GT:         return ">";
        case TOK_LTE:        return "<=";
        case TOK_GTE:        return ">=";
        case TOK_AND:        return "&&";
        case TOK_OR:         return "||";
        case TOK_BANG:       return "!";
        case TOK_AMP:        return "&";
        case TOK_ARROW:      return "->";
        case TOK_STRUCT:     return "struct";
        case TOK_ENUM:       return "enum";
        case TOK_TYPEDEF:    return "typedef";
        case TOK_STATIC:     return "static";
        case TOK_INLINE:     return "inline";
        case TOK_EOF:        return "EOF";
        default:             return "ERROR";
    }
}
