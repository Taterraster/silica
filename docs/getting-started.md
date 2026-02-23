# Getting Started with Silica

## Installation

### Requirements

- Linux x86-64
- GCC
- GNU `ld` (binutils)
- GNU Make

### Build from source

```bash
git clone <repo>
cd silica
make
```

The compiler binary is placed at `build/silicac`.

### Install system-wide

```bash
make install   # copies build/silicac to /usr/local/bin/silicac
```

---

## Your first program

Create `hello.slc`:

```silica
import std.io;
import std.main;

main hello() {
    io.println("Hello, World!");
    hello.errorcode = 0;
}
```

Compile and run:

```bash
./build/silicac hello.slc -o hello
./hello
```

Output:
```
Hello, World!
```

---

## Compiler usage

```
silicac <source.slc> -o <output>   compile to binary
silicac --tokens <source.slc>      dump token stream (debug)
silicac --ast <source.slc>         dump AST (debug)
silicac --asm <source.slc>         dump generated assembly
silicac                            start interactive REPL
```

### Compilation flags

There are no optimisation flags — Silica always compiles with `-O2` equivalent register usage. The compiler calls the system `ld` linker directly to produce a static binary.

---

## Project structure

```
silica/
├── Makefile
├── src/           compiler source (C)
│   ├── main.c
│   ├── lexer.c / lexer.h
│   ├── parser.c / parser.h
│   ├── ast.c / ast.h
│   └── codegen.c / codegen.h
├── build/         compiler binary and object files (generated)
├── tests/         .slc test programs and .slh libraries
└── docs/          this documentation
```

---

## The REPL

Running `silicac` with no arguments starts an interactive session:

```
$ ./build/silicac
Silica REPL — type expressions or declarations, blank line to quit
silica> 2 + 2
4
silica> int x = 10
silica> x * 3
30
silica> str.from_int(x)
10
silica>
```

- Expressions are automatically printed
- Variable declarations persist for the session
- Stdlib imports are always available
- A blank line exits

---

## Modules and libraries

Silica supports two kinds of reusable code:

### `.slh` header libraries

A `.slh` file contains function definitions only (no `main`). Functions are merged directly into the calling program at compile time — zero link overhead.

**mathutils.slh:**
```silica
int abs_val(int n) {
    if (n < 0) { return n * -1; }
    return n;
}

int clamp(int n, int lo, int hi) {
    if (n < lo) { return lo; }
    if (n > hi) { return hi; }
    return n;
}
```

**Using it:**
```silica
import std.io;
import mathutils;    // finds mathutils.slh in same directory
import std.main;

main myapp() {
    io.println(abs_val(-42));   // 42
    io.println(clamp(150, 0, 100)); // 100
    myapp.errorcode = 0;
}
```

### `.slc` modules

A `.slc` module is a full Silica source file (no `main`). It is compiled to a `.o` object file and linked in. The compiler automatically extracts the function signatures so calls resolve correctly.

**Import resolution search order:**
1. Same directory as the source file
2. Current working directory
3. (stdlib `std.*` is always resolved internally)

---

## Common patterns

### Reading user input

```silica
import std.io;
import std.main;

main inputdemo() {
    string name;
    io.inputln("What is your name? ", name);
    io.print("Hello, ");
    io.println(name);
    inputdemo.errorcode = 0;
}
```

### Counting loop

```silica
import std.io;
import std.main;

main counter() {
    int i = 1;
    loops.while(i <= 10) {
        io.println(i);
        int i = i + 1;
    }
    counter.errorcode = 0;
}
```

### Working with strings

```silica
import std.io;
import std.str;
import std.main;

main strings() {
    string s = str.concat("Hello", ", World!");
    io.println(s);
    io.println(str.length(s));
    io.println(str.upper(s));
    io.println(str.slice(s, 0, 5));
    strings.errorcode = 0;
}
```

### Struct with functions

```silica
import std.io;
import std.main;

struct Point {
    int x;
    int y;
}

int manhattan(int ax, int ay, int bx, int by) {
    int dx = ax - bx;
    int dy = ay - by;
    if (dx < 0) { int dx = dx * -1; }
    if (dy < 0) { int dy = dy * -1; }
    return dx + dy;
}

main geo() {
    struct Point a;
    a.x = 1;
    a.y = 2;

    struct Point b;
    b.x = 4;
    b.y = 6;

    io.println(manhattan(a.x, a.y, b.x, b.y));  // 7
    geo.errorcode = 0;
}
```

### Pass-by-reference with pointers

```silica
import std.io;
import std.main;

void swap(int* a, int* b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

main ptrdemo() {
    int x = 10;
    int y = 20;
    swap(&x, &y);
    io.println(x);  // 20
    io.println(y);  // 10
    ptrdemo.errorcode = 0;
}
```

### File I/O

```silica
import std.io;
import std.fs;
import std.main;

main files() {
    fs.create("/tmp/notes.txt");
    fs.append("/tmp/notes.txt", "Line one\n");
    fs.append("/tmp/notes.txt", "Line two\n");

    string line1 = fs.read("/tmp/notes.txt", 1);
    io.println(line1);

    fs.delete("/tmp/notes.txt");
    files.errorcode = 0;
}
```

---

## Running the test suite

```bash
make test
```

Output:
```
=== Running Silica test suite ===
  PASS tests/all_types.slc
  PASS tests/control_flow.slc
  ...
Results: 21 passed, 0 failed
```

---

## Exit codes

Every `main` block should set its exit code explicitly:

```silica
main myapp() {
    // ...
    myapp.errorcode = 0;   // success
}
```

Use non-zero values to signal errors:

```silica
if (something_failed) {
    myapp.errorcode = 1;
}
```

The exit code is read by the shell: `echo $?` after running the binary.
