# Silica Language Reference

Silica is a statically typed, compiled language targeting x86-64 Linux. It compiles directly to native binaries with no libc dependency — all I/O and system calls are done through raw Linux syscalls.

---

## Table of Contents

1. [Program Structure](#program-structure)
2. [Types](#types)
3. [Variables](#variables)
4. [Operators](#operators)
5. [Control Flow](#control-flow)
6. [Functions](#functions)
7. [Structs](#structs)
8. [Enums](#enums)
9. [Typedef](#typedef)
10. [Pointers](#pointers)
11. [Arrays](#arrays)
12. [Type Casting](#type-casting)
13. [Comments](#comments)

---

## Program Structure

Every executable Silica file must have a `main` block and import `std.main`.

```silica
import std.io;
import std.main;

main myapp() {
    io.println("Hello, World!");
    myapp.errorcode = 0;
}
```

- `import` statements go at the top, one per line, each terminated with `;`
- `using std;` can be added to allow unqualified access to stdlib namespaces (optional)
- The `main` block is named — `myapp` in the example above
- `name.errorcode = 0;` sets the process exit code — always set this at the end of main
- The binary exits with that code when main returns

### File layout

```
imports
[using declarations]
[struct / enum / typedef declarations]
[function declarations]
main block
```

---

## Types

| Type     | Size    | Description                        |
|----------|---------|------------------------------------|
| `int`    | 8 bytes | 64-bit signed integer              |
| `long`   | 8 bytes | Alias for `int`                    |
| `uint`   | 8 bytes | 64-bit unsigned integer            |
| `byte`   | 1 byte  | Unsigned 8-bit integer             |
| `char`   | 1 byte  | Single ASCII character             |
| `bool`   | 8 bytes | Boolean: `true` or `false`         |
| `float`  | 8 bytes | 64-bit IEEE 754 double             |
| `string` | 16 bytes| Pointer + length pair              |
| `void`   | —       | No return value (functions only)   |
| `void*`  | 8 bytes | Generic pointer (heap allocation)  |

### String internals

A `string` is stored as two consecutive 8-byte slots on the stack: a pointer to the character data and a length. String literals are placed in `.rodata`. There is no null terminator in the length-counted representation, but the data is null-terminated for compatibility.

---

## Variables

```silica
int x = 42;
float pi = 3.14159;
string msg = "hello";
char c = 'A';
bool flag = true;
```

Variables must be declared before use. A declaration without an initialiser zero-initialises the variable:

```silica
int n;       // n = 0
string s;    // s = empty string
```

### const

```silica
const int MAX = 100;
```

`const` variables cannot be reassigned after declaration. Attempting to do so is a compile-time error.

### Redeclaration

Inside a function body, redeclaring a variable that already exists in scope silently becomes an assignment:

```silica
int counter = 1;
int counter = 99;    // same as: counter = 99;
int counter = counter + 1;
```

---

## Operators

### Arithmetic

| Operator | Description    |
|----------|----------------|
| `+`      | Addition       |
| `-`      | Subtraction    |
| `*`      | Multiplication |
| `/`      | Integer division |
| `%`      | Modulo         |
| `-x`     | Unary negation |

### Comparison

| Operator | Description           |
|----------|-----------------------|
| `==`     | Equal                 |
| `!=`     | Not equal             |
| `<`      | Less than             |
| `>`      | Greater than          |
| `<=`     | Less than or equal    |
| `>=`     | Greater than or equal |

### Logical

| Operator | Description         |
|----------|---------------------|
| `&&`     | Logical AND (short-circuit) |
| `\|\|`   | Logical OR (short-circuit)  |
| `!`      | Logical NOT         |

### Address / Pointer

| Operator | Description                  |
|----------|------------------------------|
| `&x`     | Address of variable `x`      |
| `*p`     | Dereference pointer `p`      |
| `p->f`   | Field `f` of struct at `*p`  |

### Operator precedence (high to low)

1. Unary: `-x`, `!x`, `&x`, `*p`
2. `*`, `/`, `%`
3. `+`, `-`
4. `<`, `>`, `<=`, `>=`
5. `==`, `!=`
6. `&&`
7. `||`
8. `=` (assignment)

---

## Control Flow

### if / else if / else

```silica
if (x > 10) {
    io.println("big");
} else if (x > 5) {
    io.println("medium");
} else {
    io.println("small");
}
```

Braces are required. The condition must be an expression that evaluates to an integer or bool — zero is false, non-zero is true.

### loops.while

Three forms:

```silica
// 1. Counted — runs exactly N times
loops.while(5) {
    io.println("tick");
}

// 2. Condition — runs while expression is true
int i = 0;
loops.while(i < 10) {
    int i = i + 1;
}

// 3. Infinite — use break to exit
loops.while(true) {
    if (done) { break; }
}
```

### loops.for

```silica
// Counted — runs exactly N times
loops.for(4) {
    io.println("tick");
}

// Condition — runs while expression is truthy
int j = 3;
loops.for(j > 0) {
    int j = j - 1;
}
```

### break / continue

```silica
loops.while(true) {
    if (i == 3) { continue; }   // skip to next iteration
    if (i == 5) { break; }      // exit loop
}
```

Both `break` and `continue` work in both `loops.while` and `loops.for`.

---

## Functions

```silica
int add(int a, int b) {
    return a + b;
}

void greet(string name) {
    io.print("Hello, ");
    io.println(name);
}
```

- Return type comes first, then the function name
- `void` for functions that don't return a value
- Parameters are `type name` pairs separated by commas
- `return` exits the function, optionally with a value

### Forward declarations

A function can be declared without a body using a semicolon instead of a block. This lets you call a function before its definition appears, or declare functions that are defined in another module:

```silica
// Forward declarations
int add(int a, int b);
void greet(string name);

// Definitions follow later (or in another .slc module)
int add(int a, int b) {
    return a + b;
}
```

Forward declarations with qualifiers work the same way:

```silica
static int helper(int x);
inline int clamp(int v, int lo, int hi);
```

### Function qualifiers

Qualifiers appear before the return type and can be combined in any order.

**`static`** — gives the function local linkage. The symbol is not exported from the compilation unit (no `.global` directive in the generated assembly). Use for internal helper functions that should not be visible to other modules:

```silica
static int checksum(int* data, int len) {
    int sum = 0;
    // ...
    return sum;
}
```

**`inline`** — marks the function as an inline hint and implies local linkage. In the current compiler this is equivalent to `static` in terms of assembly output — the function is emitted once with no `.global`. The keyword is available for source compatibility with self-hosted code:

```silica
inline int min(int a, int b) {
    if (a < b) { return a; }
    return b;
}
```

**Combined:**

```silica
static inline int square(int n) {
    return n * n;
}
```

### Return types

All primitive types can be returned:

```silica
string getMessage() { return "hello"; }
bool   isEven(int n) { return n % 2 == 0; }
float  getPI()       { return 3.14; }
char   getGrade()    { return 'A'; }
```

### Function overloading

Multiple functions can share the same name if they have different parameter counts or types:

```silica
int describe(int n)         { return n * 2; }
int describe(int a, int b)  { return a + b; }

void greet(string name) { io.println(name); }
void greet(int n)       { io.println(n); }
```

The compiler selects the best match at the call site based on argument count and types.

### Recursion

Silica supports full recursion. Stack frames are sized dynamically based on the actual locals declared in each function, so deep recursion works without stack overflow from oversized frames:

```silica
int fib(int n) {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}

int gcd(int a, int b) {
    if (b == 0) { return a; }
    return gcd(b, a % b);
}
```

---

## Structs

Structs group named fields into a single value type. They are declared at the top level (before `main`) and variables are allocated on the stack.

### Declaration

```silica
struct Point {
    int x;
    int y;
}

struct Rect {
    int x;
    int y;
    int w;
    int h;
}
```

Each field is a `type name;` line. All fields are currently 8 bytes wide. The trailing semicolon after the closing `}` is optional.

### Typedef compound form

A struct and its type alias can be declared together in a single `typedef struct` block:

```silica
// Named struct with alias
typedef struct Vec2 {
    int x;
    int y;
} Vec2;

// Anonymous struct — only the alias name is usable
typedef struct {
    int r;
    int g;
    int b;
} Color;
```

When declared this way, the alias name can be used directly to declare variables (without the `struct` keyword):

```silica
Vec2 v;
v.x = 3;
v.y = 4;

Color c;
c.r = 255;
c.g = 128;
c.b = 0;
```

You can also create a forward-reference alias that separates the struct definition from the alias:

```silica
struct Point { int x; int y; }
typedef struct Point Point;   // alias 'Point' → struct 'Point'
```

### Creating and using struct variables

With the `struct` keyword:

```silica
main example() {
    struct Point p;    // zero-initialised
    p.x = 10;
    p.y = 20;

    io.println(p.x);   // 10
    io.println(p.y);   // 20
}
```

With a typedef alias:

```silica
main example() {
    Vec2 v;            // typedef alias — no 'struct' keyword needed
    v.x = 3;
    v.y = 4;
}
```

### Passing struct fields to functions

Struct fields are expressions and can be passed directly to functions or used in arithmetic:

```silica
int dist = add(p.x, p.y);
int perimeter = 2 * r.w + 2 * r.h;
```

---

## Pointers

Pointers hold the memory address of another variable. They are declared with `*` after the type and created with the `&` address-of operator.

### Basic pointer usage

```silica
int x = 42;
int* p = &x;        // p holds the address of x

io.println(*p);     // dereference: prints 42
*p = 99;            // write through pointer
io.println(x);      // x is now 99
```

### Pointers as function parameters

Pointers enable pass-by-reference semantics:

```silica
void increment(int* n) {
    *n = *n + 1;
}

void swap(int* a, int* b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

main example() {
    int val = 5;
    increment(&val);    // val is now 6

    int a = 100;
    int b = 200;
    swap(&a, &b);       // a=200, b=100
}
```

### Multiple output parameters

```silica
void minmax(int a, int b, int* lo, int* hi) {
    if (a < b) { *lo = a; *hi = b; }
    else        { *lo = b; *hi = a; }
}

main example() {
    int lo = 0;
    int hi = 0;
    minmax(73, 29, &lo, &hi);
    // lo = 29, hi = 73
}
```

### Pointer to struct (`->`)

```silica
struct Point p;
p.x = 5;
p.y = 10;

// Point a pointer at the struct... (advanced usage)
// p->field accesses a field through a pointer
```

---

## Enums

Enums declare a set of named integer constants at the top level. Members auto-increment from 0, or can be assigned explicit values.

```silica
enum Direction {
    NORTH = 0,
    EAST,
    SOUTH,
    WEST
}

enum Color {
    RED = 1,
    GREEN = 2,
    BLUE = 3
}
```

Enum members are integer constants usable anywhere an `int` is expected:

```silica
main example() {
    int d = NORTH;
    if (d == EAST) { io.println("going east"); }
    io.println(BLUE);   // 3
    example.errorcode = 0;
}
```

- Members are globally scoped — no `Direction.NORTH` syntax needed
- Comma between members is optional
- The trailing comma after the last member is also optional

### Typedef compound form

A `typedef enum` block declares the enum and creates a type alias in one step:

```silica
// Named enum with alias
typedef enum Status {
    OK  = 0,
    ERR = 1,
} Status;

// Anonymous enum — only the alias is usable as a type name
typedef enum {
    NODE_INT  = 0,
    NODE_STR,
    NODE_BOOL,
} NodeKind;
```

The alias resolves to `int` — enum values remain globally scoped integer constants either way.

---

## Typedef

`typedef` creates an alias for an existing type. The alias can be used anywhere the original type would appear — in variable declarations, function parameters, and return types.

### Primitive aliases

```silica
typedef int    i64;
typedef bool   flag_t;
typedef float  real_t;
typedef void*  Handle;

i64    x    = 42;
flag_t done = false;
real_t pi   = 3.14;

i64 add(i64 a, i64 b) {
    return a + b;
}
```

### Struct aliases — three forms

**Forward-reference alias** (struct defined separately):

```silica
struct Point { int x; int y; }
typedef struct Point Point;    // 'Point' is now an alias for 'struct Point'

Point p;
p.x = 10;
```

**Named typedef struct** (name and alias together):

```silica
typedef struct Vec2 {
    int x;
    int y;
} Vec2;

Vec2 v;
v.x = 3;
```

**Anonymous typedef struct** (no separate struct name):

```silica
typedef struct {
    int r;
    int g;
    int b;
} Color;

Color c;
c.r = 255;
```

### Enum aliases

```silica
typedef enum {
    PENDING = 0,
    RUNNING,
    DONE,
} TaskState;
```

### Pointer typedef

```silica
typedef int*  IntPtr;
typedef void* Ptr;

IntPtr p = &someint;
Ptr    buf = mem.alloc(256, 0);
```

---

## Pointers

Pointers hold the memory address of another variable. They are declared with `*` after the type and created with the `&` address-of operator.

### Basic pointer usage

```silica
int x = 42;
int* p = &x;        // p holds the address of x

io.println(*p);     // dereference: prints 42
*p = 99;            // write through pointer
io.println(x);      // x is now 99
```

### Pointers as function parameters

Pointers enable pass-by-reference semantics:

```silica
void increment(int* n) {
    *n = *n + 1;
}

void swap(int* a, int* b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

main example() {
    int val = 5;
    increment(&val);    // val is now 6

    int a = 100;
    int b = 200;
    swap(&a, &b);       // a=200, b=100
    example.errorcode = 0;
}
```

### Multiple output parameters

```silica
void minmax(int a, int b, int* lo, int* hi) {
    if (a < b) { *lo = a; *hi = b; }
    else        { *lo = b; *hi = a; }
}

main example() {
    int lo = 0;
    int hi = 0;
    minmax(73, 29, &lo, &hi);
    // lo = 29, hi = 73
}
```

### Pointer to struct (`->`)

```silica
struct Point p;
p.x = 5;
p.y = 10;

struct Point* pp = &p;
io.println(pp->x);   // 5
```

### `void*` — generic pointer

`void*` is a typeless pointer returned by heap allocation functions. It holds any address and can be compared, stored, and passed around, but must be cast to a typed pointer before dereferencing.

```silica
void* buf = mem.alloc(256, 0);     // allocate 256 bytes, returns void*
void* raw = mem.alloc_raw(1024);   // allocate exact bytes

if (buf > 0) {
    io.println("allocation succeeded");
}

// cast to typed pointer to use
int* iptr = buf;
*iptr = 42;
```

- `void*` variables occupy 8 bytes (a pointer-sized slot)
- Comparison operators work on `void*` values
- `mem.free(ptr)` accepts any pointer (currently a no-op — brk allocator)

---

## Arrays

Arrays are heap-allocated and accessed by index. They are declared with `[]` after the type.

```silica
int[] nums = {10, 20, 30, 40, 50};

io.println(nums[0]);   // 10
io.println(nums[2]);   // 30

nums[2] = 999;
io.println(nums[2]);   // 999
```

- Array literals use `{val, val, ...}` syntax
- Indices are zero-based
- All array elements are 8 bytes
- Bounds are not checked at runtime — out-of-bounds access is undefined behaviour

---

## Type Casting

Explicit casts use C-style `(type)` syntax:

```silica
int iv = 42;
float fv = (float)iv;   // int → float

float fv2 = 7.9;
int iv2 = (int)fv2;     // float → int, truncates toward zero
```

Supported casts: any numeric type to any other numeric type. String casting is handled through `str.from_int` and `str.from_float` in the stdlib.

---

## Comments

Single-line comments only:

```silica
// This is a comment
int x = 42; // end-of-line comment
```

There are no block comments.