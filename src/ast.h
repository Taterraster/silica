#ifndef AST_H
#define AST_H

#include <stddef.h>

/* ── Variable types (declared early so Expr can reference them) ── */
typedef enum {
    TYPE_INT,     /* 64-bit signed integer   */
    TYPE_CHAR,    /* single byte character   */
    TYPE_STRING,  /* pointer + length pair   */
    TYPE_BOOL,    /* 0 = false, 1 = true     */
    TYPE_FLOAT,   /* 64-bit double           */
    TYPE_LONG,    /* alias for int (64-bit)  */
    TYPE_BYTE,    /* unsigned 8-bit          */
    TYPE_UINT,    /* 64-bit unsigned integer */
    TYPE_VOID,    /* no return value         */
    TYPE_PTR,     /* pointer (int*)          */
    TYPE_STRUCT,  /* struct type             */
    TYPE_VOID_PTR,/* void* generic pointer   */
} VarType;

/* ── Expression kinds ── */
typedef enum {
    EXPR_INT_LIT,       /* 42              */
    EXPR_FLOAT_LIT,     /* 3.14            */
    EXPR_BOOL_LIT,      /* true / false    */
    EXPR_CHAR_LIT,      /* 'x'             */
    EXPR_STRING_LIT,    /* "hello"         */
    EXPR_IDENT,         /* foo             */
    EXPR_FIELD,         /* a.b             */
    EXPR_CALL,          /* callee(args...) */
    EXPR_ASSIGN,        /* lhs = rhs       */
    EXPR_BINOP,         /* lhs op rhs      */
    EXPR_UNARY_NEG,     /* -expr           */
    EXPR_COMPARE,       /* lhs == rhs / != */
    EXPR_LOGICAL_AND,   /* lhs && rhs      */
    EXPR_LOGICAL_OR,    /* lhs || rhs      */
    EXPR_LOGICAL_NOT,   /* !expr           */
    EXPR_CAST,          /* (type)expr      */
    EXPR_INDEX,         /* arr[i]          */
    EXPR_ARRAY_LIT,     /* {1, 2, 3}       */
    EXPR_ADDROF,        /* &x              */
    EXPR_DEREF,         /* *p              */
    EXPR_PTR_FIELD,     /* p->field        */
} ExprKind;

typedef struct Expr Expr;
struct Expr {
    ExprKind  kind;
    long      ival;     /* INT_LIT / BOOL_LIT  */
    double    fval;     /* FLOAT_LIT           */
    char      cval;     /* CHAR_LIT            */
    char     *sval;     /* STRING/IDENT        */
    char      op;       /* BINOP: +,-,*,/,%    */
    Expr     *object;   /* FIELD: left         */
    char     *field;    /* FIELD: name         */
    Expr     *callee;   /* CALL                */
    Expr    **args;     /* CALL / ARRAY_LIT elements */
    int       argc;     /* CALL / ARRAY_LIT count    */
    Expr     *lhs;      /* ASSIGN / BINOP / INDEX base */
    Expr     *rhs;      /* ASSIGN / BINOP / INDEX idx / ADDROF / DEREF operand */
    int       cast_type; /* CAST: target VarType (int) */
    /* TYPE_PTR support */
    VarType   ptr_base;  /* base type for pointer declarations */
    char     *ptr_field; /* PTR_FIELD: field name after -> */
};

/* ── Statement kinds ── */
typedef enum {
    STMT_VAR_DECL,      /* int x; / char c='x'; / string s=".."; */
    STMT_EXPR,          /* any expression statement               */
    STMT_RETURN,        /* return <expr>;                         */
    STMT_IF,            /* if(cond) { body } [else { body }]      */
    STMT_WHILE,         /* loops.while(cond/n/true) { body }      */
    STMT_FOR,           /* loops.for(cond) { body }               */
    STMT_BREAK,         /* break;                                 */
    STMT_CONTINUE,      /* continue;                              */
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind  kind;
    VarType   vtype;
    char     *varname;
    int       is_const;   /* 1 if declared with const keyword             */
    int       is_array;   /* 1 if this is an array declaration            */
    int       is_ptr;     /* 1 if this is a pointer declaration (int* p)  */
    VarType   elem_type;  /* element type for arrays / pointed-to for ptr */
    char     *struct_name;/* for TYPE_STRUCT variables: name of struct    */
    Expr     *init;       /* VAR_DECL initialiser / RETURN value          */
    Expr     *expr;       /* STMT_EXPR                                    */
    /* control flow fields (IF / WHILE / FOR) */
    Expr     *cond;       /* condition expression                         */
    Stmt    **body;       /* then-branch / loop body                      */
    int       nbody;
    Stmt    **elsebody;   /* else-branch (IF only, may be NULL)           */
    int       nelsebody;
};

/* ── Struct field ── */
typedef struct {
    VarType  type;
    char    *name;
    char    *struct_name;  /* if type == TYPE_STRUCT, name of the struct type */
    int      offset;       /* byte offset within the struct */
} StructField;

/* ── Struct declaration ── */
typedef struct {
    char         *name;
    char         *typedef_name; /* alias from typedef struct Foo {} Bar;  NULL if none */
    StructField  *fields;
    int           nfields;
    int           total_size; /* total bytes */
} StructDecl;

/* ── Function parameter ── */
typedef struct {
    VarType  type;
    char    *name;
    char    *struct_name;  /* if type == TYPE_STRUCT or TYPE_PTR */
    VarType  ptr_base;     /* if type == TYPE_PTR: the pointed-to type */
} FuncParam;

/* ── User-defined function ── */
typedef struct {
    char       *name;
    VarType     rettype;
    FuncParam  *params;
    int         nparams;
    Stmt      **stmts;
    int         nstmts;
    int         is_extern;  /* 1 = declared but defined in another .o (don't emit body) */
    int         is_static;  /* 1 = local linkage, no .global directive                  */
    int         is_inline;  /* 1 = inline hint (implies static linkage)                 */
} FuncDecl;

/* ── Enum declaration ── */
typedef struct {
    char *name;           /* enum Name { ... }  */
    char **member_names;  /* array of member name strings */
    long *member_values;  /* array of integer values */
    int   nmembers;
} EnumDecl;

/* ── Typedef declaration ── */
typedef struct {
    char   *alias;        /* typedef int MyInt  → alias = "MyInt" */
    VarType base_type;    /* the underlying type */
    char   *base_name;    /* if base is struct/enum: its name */
} TypedefDecl;

/* ── Top-level declarations ── */
typedef struct { char *module; }  ImportDecl;
typedef struct { char *ns;     }  UsingDecl;

typedef struct {
    char   *name;
    Stmt  **stmts;
    int     nstmts;
} MainFunc;

typedef struct {
    ImportDecl  *imports;  int nimports;
    UsingDecl   *usings;   int nusings;
    StructDecl **structs;  int nstructs;
    EnumDecl   **enums;    int nenums;
    TypedefDecl **typedefs; int ntypedefs;
    MainFunc    *mainfn;   /* NULL if not present */
    FuncDecl   **funcs;    int nfuncs;
} Program;

/* Helpers */
Expr        *expr_new(ExprKind k);
Stmt        *stmt_new(StmtKind k);
FuncDecl    *funcdecl_new(void);
StructDecl  *structdecl_new(void);
EnumDecl    *enumdecl_new(void);
TypedefDecl *typedefdecl_new(void);
Program     *program_new(void);
void         program_free(Program *p);

#endif /* AST_H */
