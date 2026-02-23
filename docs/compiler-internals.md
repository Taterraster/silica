# Silica Compiler Internals

This document describes how the Silica compiler works internally. It is aimed at contributors who want to understand or modify the compiler.

---

## Pipeline overview

```
source text (.slc)
      │
      ▼
   [Lexer]          src/lexer.c
      │  token stream
      ▼
   [Parser]         src/parser.c
      │  AST (Program)
      ▼
   [Codegen]        src/codegen.c
      │  AT&T x86-64 assembly (.s)
      ▼
   [Assembler]      system `as` (via gcc -c)
      │  object file (.o)
      ▼
   [Linker]         system `ld`
      │
      ▼
  native binary (ELF, static, no libc)
```

The orchestration — argument parsing, import resolution, module linking, REPL — lives in `src/main.c`.

---

## Source files

| File           | Responsibility |
|----------------|----------------|
| `src/lexer.h/c`  | Tokenisation |
| `src/ast.h/c`    | AST node types and allocation helpers |
| `src/parser.h/c` | Recursive descent parser → AST |
| `src/codegen.h/c`| AST → x86-64 AT&T assembly |
| `src/main.c`     | Driver: CLI, import resolution, REPL |

---

## Lexer (`src/lexer.c`)

A simple single-pass lexer. `lexer_next()` returns one `Token` at a time. Tokens are allocated on the heap (`strdup` for values) and freed by the parser after consumption.

### Token types (selected)

```c
TOK_INT_LIT, TOK_FLOAT_LIT, TOK_CHAR_LIT, TOK_STRING_LIT
TOK_IDENT
TOK_INT, TOK_CHAR, TOK_STRING, TOK_BOOL, TOK_FLOAT,
TOK_LONG, TOK_BYTE, TOK_UINT, TOK_VOID, TOK_STRUCT
TOK_IF, TOK_ELSE, TOK_RETURN, TOK_CONST
TOK_IMPORT, TOK_USING, TOK_MAIN, TOK_BREAK, TOK_CONTINUE
TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT
TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LTE, TOK_GTE
TOK_AND, TOK_OR, TOK_BANG
TOK_AMP, TOK_ARROW
TOK_DOT, TOK_ASSIGN, TOK_SEMICOLON, TOK_COMMA
TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET
TOK_EOF, TOK_ERROR
```

`->` is a single `TOK_ARROW` token. `-` followed by anything other than `>` is `TOK_MINUS`. `&` followed by `&` is `TOK_AND`; alone it is `TOK_AMP`.

---

## AST (`src/ast.h`)

### VarType

```c
typedef enum {
    TYPE_INT, TYPE_CHAR, TYPE_STRING, TYPE_BOOL,
    TYPE_FLOAT, TYPE_LONG, TYPE_BYTE, TYPE_UINT,
    TYPE_VOID, TYPE_PTR, TYPE_STRUCT
} VarType;
```

### ExprKind

```c
typedef enum {
    EXPR_INT_LIT, EXPR_FLOAT_LIT, EXPR_BOOL_LIT,
    EXPR_CHAR_LIT, EXPR_STRING_LIT,
    EXPR_IDENT,       // foo
    EXPR_FIELD,       // a.b
    EXPR_CALL,        // f(args)
    EXPR_ASSIGN,      // lhs = rhs
    EXPR_BINOP,       // lhs op rhs
    EXPR_UNARY_NEG,   // -expr
    EXPR_COMPARE,     // lhs == rhs / != / < / > / <= / >=
    EXPR_LOGICAL_AND, // lhs && rhs
    EXPR_LOGICAL_OR,  // lhs || rhs
    EXPR_LOGICAL_NOT, // !expr
    EXPR_CAST,        // (type)expr
    EXPR_INDEX,       // arr[i]
    EXPR_ARRAY_LIT,   // {1, 2, 3}
    EXPR_ADDROF,      // &x
    EXPR_DEREF,       // *p
    EXPR_PTR_FIELD,   // p->field
} ExprKind;
```

### Expr

All expression nodes use a single `Expr` struct with union-like field reuse:

```c
struct Expr {
    ExprKind  kind;
    long      ival;      // INT_LIT / BOOL_LIT
    double    fval;      // FLOAT_LIT
    char      cval;      // CHAR_LIT
    char     *sval;      // STRING_LIT / IDENT name
    char      op;        // BINOP: +,-,*,/,%  COMPARE: =,!,<,>,L,G
    Expr     *object;    // FIELD: left side
    char     *field;     // FIELD: field name
    Expr     *callee;    // CALL
    Expr    **args;      // CALL / ARRAY_LIT
    int       argc;
    Expr     *lhs;       // ASSIGN / BINOP / INDEX / PTR_FIELD
    Expr     *rhs;       // ASSIGN / BINOP / INDEX / ADDROF / DEREF
    int       cast_type; // CAST: target VarType
    VarType   ptr_base;  // pointer base type
    char     *ptr_field; // PTR_FIELD: field name after ->
};
```

### StmtKind / Stmt

```c
typedef enum {
    STMT_VAR_DECL, STMT_EXPR, STMT_RETURN,
    STMT_IF, STMT_WHILE, STMT_FOR,
    STMT_BREAK, STMT_CONTINUE
} StmtKind;

struct Stmt {
    StmtKind  kind;
    VarType   vtype;
    char     *varname;
    int       is_const;
    int       is_array;
    int       is_ptr;
    VarType   elem_type;    // array element type / pointer base type
    char     *struct_name;  // for TYPE_STRUCT variables
    Expr     *init;         // VAR_DECL initialiser / RETURN value
    Expr     *expr;         // STMT_EXPR
    Expr     *cond;         // IF / WHILE / FOR condition
    Stmt    **body;
    int       nbody;
    Stmt    **elsebody;
    int       nelsebody;
};
```

### Program

```c
typedef struct {
    ImportDecl  *imports;  int nimports;
    UsingDecl   *usings;   int nusings;
    StructDecl **structs;  int nstructs;
    MainFunc    *mainfn;
    FuncDecl   **funcs;    int nfuncs;
} Program;
```

### StructDecl

```c
typedef struct {
    char         *name;
    StructField  *fields;
    int           nfields;
    int           total_size;   // bytes
} StructDecl;

typedef struct {
    VarType  type;
    char    *name;
    char    *struct_name;  // nested struct type name
    int      offset;       // byte offset within the struct
} StructField;
```

---

## Parser (`src/parser.c`)

Recursive descent, two-token lookahead (`cur` and `nxt`).

### Key design decisions

**Struct type ambiguity:** An `IDENT` token at the start of a statement could be a struct type name (e.g. `Point p;`) or a function call / expression (e.g. `io.println(...)`). The parser resolves this by only treating an `IDENT` as a type name when the *next* token is another `IDENT` or `*` — otherwise it falls through to expression parsing.

**Redeclaration:** When a variable is declared that already exists in the current scope, the parser silently turns it into an assignment expression. This is intentional and allows the common `int i = i + 1;` update pattern inside loops.

**Pointer types:** `int*` is parsed as type `int` followed by `TOK_STAR`, setting `is_ptr = 1` and storing `ptr_base = TYPE_INT` in the statement. The variable's `vtype` becomes `TYPE_PTR`.

### Expression grammar (simplified)

```
expr    → assign
assign  → cmp ('=' assign)?
cmp     → add (('==' | '!=' | '<' | '>' | '<=' | '>=') add)*
add     → mul (('+' | '-') mul)*
mul     → primary (('*' | '/' | '%') primary)*
primary → INT_LIT | FLOAT_LIT | BOOL_LIT | CHAR_LIT | STRING_LIT
        | '!' primary
        | '-' primary
        | '&' primary        (ADDROF)
        | '*' primary        (DEREF)
        | '(' type ')' primary  (CAST)
        | '(' expr ')'
        | '{' expr, ... '}'  (ARRAY_LIT)
        | IDENT ('.' IDENT)* ('(' args ')' )? ('->' IDENT)* ('[' expr ']')*
```

---

## Code Generator (`src/codegen.c`)

Emits AT&T-syntax x86-64 assembly targeting Linux.

### Calling convention

System V AMD64 ABI:
- Integer/pointer args: `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`
- Float args/return: `%xmm0`–`%xmm7`
- Integer return: `%rax`
- `string` arguments use two registers: pointer in `arg[n]`, length in `arg[n+1]`
- Caller-saved: `%rax`, `%rcx`, `%rdx`, `%rsi`, `%rdi`, `%r8`–`%r11`
- Callee-saved: `%rbx`, `%rbp`, `%r12`–`%r15`

### Stack frame layout

```
[rbp + 16+]  stack-passed arguments (beyond 6 registers)
[rbp + 8]    return address
[rbp]        saved %rbp
[rbp - 8]    first local / first parameter spill
[rbp - N]    ...
[rsp]        bottom of frame
```

Frame size is computed by `prescan_stmts()` before emitting any code — it walks the AST counting bytes needed for all `STMT_VAR_DECL` nodes (including nested ones inside `if`/`while`/`for` branches). This allows deep recursion without per-frame waste.

```c
int frame_bytes = ((param_bytes + local_bytes + 128 + 15) & ~15);
```

The +128 provides scratch space for sub-expression temporaries that spill to the stack (division, push/pop pairs, etc.).

### Variable storage sizes

| Type    | Stack bytes | Notes |
|---------|-------------|-------|
| `int`, `long`, `uint`, `bool` | 8 | qword |
| `char`, `byte` | 8 | low byte used, rest zero |
| `float` | 8 | IEEE 754 double via XMM |
| `string` | 16 | `[ptr:8][len:8]` |
| pointer | 8 | address as qword |
| struct  | `nfields * 8` | rounded up to 8-byte alignment |

### Struct layout

Fields are stored in declaration order at consecutive 8-byte slots. Given a variable `v` of struct type at `rbp - v->offset`, field `f` at struct offset `f->offset` is at:

```
rbp - (v->offset - f->offset)
```

This is because `v->offset` marks the *end* of the allocation (the stack grows down, so `alloc_var` increments `stack_used` first and uses that as the offset).

### Integer expression evaluation

`emit_int_expr(cg, e)` evaluates any integer-typed expression and leaves the result in `%rax`. For binary operations it uses `pushq %rax` / `popq %rax` to handle the two-operand case without clobbering:

```asm
; lhs + rhs
emit lhs      → %rax
pushq %rax
emit rhs      → %rax
movq %rax, %rcx    ; rcx = rhs
popq %rax          ; rax = lhs
addq %rcx, %rax
```

### Float expression evaluation

`emit_float_expr(cg, e)` leaves the result in `%xmm0`. Float literals are materialised via:

```c
unsigned long long bits;
memcpy(&bits, &d, 8);
fprintf(out, "movabsq $0x%llx, %%rax\n"
             "movq %%rax, -8(%%rsp)\n"
             "movsd -8(%%rsp), %%xmm0\n", bits);
```

(Uses the red zone — the 128 bytes below `%rsp` that the ABI guarantees won't be clobbered by signal handlers in leaf functions.)

### Stdlib dispatch

Stdlib calls are not real function calls — they are recognised by name in `emit_call()` and expanded inline to syscall sequences or arithmetic. For example `io.println(x)` emits a `sys_write` syscall directly; `math.sqrt(n)` emits a Newton's method loop.

`resolve_name()` canonicalises short forms (`io.X` → `std.io.X`) and enforces the import gate — calling a stdlib function without the corresponding `import` is caught here and sets `cg->had_error = 1`.

### String literal pool

String literals are collected in a global `str_pool[]` array and emitted to `.rodata` at the end of the file. Each gets a unique label `.LstrN`. The pool is shared across all function codegen passes via the `str_counter` field propagated through `parent_cg`.

### Function overloading

Overloaded user functions are resolved in `find_userfunc_overload()` with three passes:
1. Name + arity + type signature match (exact)
2. Name + arity match (ignore types)
3. Name only match (last resort)

Type signatures are single-character codes: `i` (int/long/uint/bool), `f` (float), `s` (string), `c` (char/byte). Overloaded functions get mangled labels: `__silica_user_greet_i` vs `__silica_user_greet_s`.

---

## Import and module system (`src/main.c`)

### stdlib imports (`std.*`)

Handled entirely inside codegen — `import std.io;` sets `cg->using_io = 1`. No files are looked up.

### `.slh` library imports

1. Search for `name.slh` in the source file's directory, then the current directory
2. Parse it as a `Program` (no `main`)
3. Merge its `FuncDecl` list directly into the calling program's `prog->funcs`
4. These functions are compiled inline — no separate object file

### `.slc` module imports

1. Search for `name.slc`
2. Invoke `silicac -c name.slc -o /tmp/__silica_name.o` (compile to object, no link)
3. Parse `name.slc` again to extract function signatures as `is_extern = 1` stubs
4. Add the `.o` to the link command
5. The stub functions are present in the codegen's function table so call sites can resolve — but their bodies are not emitted (guarded by `if (!fd->is_extern)`)

---

## Runtime helpers

Two helpers are emitted into the `.text` section when needed:

### `__silica_print_int` / `__silica_print_int_nl`

Converts the integer in `%rax` to decimal ASCII on the stack (handles negatives) and calls `sys_write`. Pure assembly — no libc.

### `__silica_print_float` / `__silica_print_float_nl`

Converts the double in `%xmm0` to a decimal string. Uses integer arithmetic to extract integer and fractional parts.

Both helpers are emitted at most once per compilation unit, gated by `cg->need_itoa`.

---

## Adding a new stdlib function

1. **Lexer:** no changes needed (stdlib names are parsed as qualified identifiers)
2. **Parser:** no changes needed
3. **Codegen:** add a branch in `emit_call()` that matches the resolved name and emits inline assembly
4. **Import gate:** add a flag to `CG`, set it in `codegen_emit()` when the import is present, check it in `resolve_name()`
5. **`io.println` dispatch:** if the function returns `string` or `float`, add its name to the appropriate `is_string` / `is_float` detection block
6. **Docs:** add to `docs/stdlib.md`
