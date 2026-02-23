# Silica Documentation

---

## [Getting Started](getting-started.md)

Start here if you're new to Silica.

- Building the compiler from source
- Writing and compiling your first program
- Compiler CLI flags (`--tokens`, `--ast`, `--asm`)
- Using the interactive REPL
- The module system — `.slh` libraries and `.slc` modules
- Common patterns: input, loops, strings, structs, pointers, file I/O
- Running the test suite
- Setting exit codes

---

## [Language Reference](language-reference.md)

Complete syntax and semantics reference.

- **Types** — `int`, `long`, `uint`, `byte`, `char`, `bool`, `float`, `string`
- **Variables** — declaration, initialisation, `const`, redeclaration rules
- **Operators** — arithmetic, comparison, logical, pointer (`&`, `*`, `->`), precedence table
- **Control flow** — `if`/`else if`/`else`, `loops.while`, `loops.for`, `break`, `continue`
- **Functions** — return types, parameters, overloading, recursion
- **Structs** — declaration, field access, field assignment
- **Pointers** — address-of, dereference, pass-by-reference, pointer to struct
- **Arrays** — literal initialisation, index access, element assignment
- **Type casting** — `(float)n`, `(int)f`
- **Comments**

---

## [Standard Library](stdlib.md)

Reference for all nine standard library modules. Each entry covers every function with its signature, description, and a short example.

| Module | Key functions |
|---|---|
| `std.io` | `print`, `println`, `input`, `inputln` |
| `std.math` | `sqrt`, `sin`, `cos`, `log`, `pwr`, `root`, `sigma`, `integral`, `random`, constants `pi`/`e` |
| `std.str` | `length`, `concat`, `contains`, `slice`, `upper`, `lower`, `trim`, `repeat`, `from_int`, `to_int`, `eq` |
| `std.fs` | `create`, `append`, `read`, `delete` |
| `std.mem` | `alloc` |
| `std.time` | `now`, `now_ms`, `mono`, `sleep` |
| `std.net` | `ip`, `connect`, `send`, `recv`, `close` |
| `std.env` | `argc`, `argv`, `get` |
| `std.proc` | `pid`, `exit` |

---

## [Compiler Internals](compiler-internals.md)

For contributors and anyone who wants to understand how Silica is implemented.

- Full pipeline diagram: source → lexer → parser → codegen → assembler → linker
- Source file map (`src/lexer.c`, `src/parser.c`, `src/ast.h`, `src/codegen.c`, `src/main.c`)
- Token types and lexer design
- AST node structs (`Expr`, `Stmt`, `FuncDecl`, `StructDecl`, `Program`)
- Parser design decisions — struct type ambiguity, redeclaration, pointer parsing
- x86-64 calling convention and stack frame layout
- Struct memory layout and field addressing
- Integer and float expression emission
- How stdlib calls are dispatched inline (no real function calls)
- String literal pool
- Function overload resolution
- Import and module system internals
- How to add a new stdlib function
