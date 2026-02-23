# Silica Standard Library

The standard library is split into modules. Each module must be explicitly imported before use — calling a stdlib function without its import is a compile-time error.

```silica
import std.io;
import std.math;
import std.str;
// etc.
```

`using std;` can be added after imports to allow short-form names (`io.print` instead of `std.io.print`). Short-form names work with or without `using std;`.

---

## std.io

**Import:** `import std.io;`

I/O functions. All output goes directly to stdout via `sys_write`. All input reads from stdin via `sys_read`.

### io.print(value)

Prints a value without a trailing newline. Accepts any type:

```silica
io.print("hello ");
io.print(42);
io.print(3.14);
io.print('A');
io.print(true);
```

### io.println(value)

Prints a value followed by a newline `\n`. Same type support as `io.print`:

```silica
io.println("Hello, World!");
io.println(100);
io.println(true);
```

### io.input(prompt, varname)

Prints `prompt` then reads a line from stdin into `varname` (stops at newline, does not include it):

```silica
string city;
io.input("Enter your city: ", city);
io.println(city);
```

### io.inputln(prompt, varname)

Same as `io.input` but also consumes the trailing newline:

```silica
string name;
io.inputln("Enter your name: ", name);
```

---

## std.math

**Import:** `import std.math;`

Mathematical functions and constants.

### Constants

| Name       | Value                  | Type    |
|------------|------------------------|---------|
| `math.pi`  | 3.14159265358979…      | `float` |
| `math.e`   | 2.71828182845904…      | `float` |
| `math.i`   | sqrt(-1) — imaginary   | special |

```silica
io.println(math.pi);    // 3.141593
float e = math.e;
```

### math.sqrt(n)

Returns the integer square root of `n`:

```silica
int s = math.sqrt(25);    // 5
int s2 = math.sqrt(144);  // 12
```

### math.root(root, n)

Returns the integer `root`-th root of `n`:

```silica
int r = math.root(3, 27);   // 3  (cube root of 27)
int r2 = math.root(4, 256); // 4  (4th root of 256)
```

### math.pwr(exp, base)

Returns `base` raised to the power `exp` (integer result):

```silica
int p = math.pwr(2, 5);   // 25  (5^2)
int p2 = math.pwr(3, 2);  // 8   (2^3)
```

Note: argument order is `(exponent, base)`.

### math.sin(degrees)

Returns the sine of `degrees` (degrees, not radians) as a `float`:

```silica
float s = math.sin(90);   // 1.0
io.println(math.sin(45)); // 0.707107
```

### math.cos(degrees)

Returns the cosine of `degrees` as a `float`:

```silica
float c = math.cos(0);    // 1.0
io.println(math.cos(60)); // 0.5
```

### math.log(n)

Returns the natural logarithm (ln) of `n` as a `float`:

```silica
float l = math.log(1);    // 0.0
io.println(math.log(10)); // 2.302585
```

### math.integral(a, b)

Numerically integrates x from `a` to `b` (i.e. computes `(b²-a²)/2`) as a `float`:

```silica
float area = math.integral(0, 10);  // 50.0
```

### math.sigma(start, end)

Returns the sum of all integers from `start` to `end` inclusive:

```silica
int s = math.sigma(1, 10);   // 55
int s2 = math.sigma(1, 100); // 5050
```

### math.random(seed)

Returns a pseudo-random integer derived from `seed`. Passing `0` uses a default internal seed:

```silica
int r = math.random(12345);
int r2 = math.random(0);    // uses default seed
```

---

## std.str

**Import:** `import std.str;`

String manipulation functions.

### str.length(s)

Returns the number of characters in `s`:

```silica
int n = str.length("Hello");   // 5
io.println(str.length(s));
```

### str.concat(a, b)

Returns a new string with `b` appended to `a`:

```silica
string s = str.concat("Hello", ", World");
io.println(s);  // Hello, World
```

### str.contains(s, sub)

Returns `1` if `sub` is found in `s`, `0` otherwise:

```silica
io.println(str.contains("foobar", "oba"));  // 1
io.println(str.contains("foobar", "xyz"));  // 0
```

### str.slice(s, start, len)

Returns `len` characters from `s` starting at index `start` (zero-based):

```silica
string sub = str.slice("Hello World", 6, 5);
io.println(sub);  // World
```

### str.from_int(n)

Converts integer `n` to a string:

```silica
string s = str.from_int(12345);
io.println(s);  // "12345"
```

### str.from_float(f)

Converts float `f` to a string representation:

```silica
string s = str.from_float(3.14);
io.println(s);  // "3.14"
```

### str.to_int(s)

Parses `s` as a decimal integer. Handles negative numbers:

```silica
int n = str.to_int("42");    // 42
int neg = str.to_int("-99"); // -99
```

### str.upper(s)

Returns `s` with all lowercase letters converted to uppercase:

```silica
string up = str.upper("hello world");
io.println(up);  // HELLO WORLD
```

### str.lower(s)

Returns `s` with all uppercase letters converted to lowercase:

```silica
string lo = str.lower("HELLO WORLD");
io.println(lo);  // hello world
```

### str.trim(s)

Returns `s` with leading and trailing whitespace removed:

```silica
string t = str.trim("   hello   ");
io.println(t);  // "hello"
```

### str.repeat(s, n)

Returns `s` repeated `n` times:

```silica
string r = str.repeat("ab", 3);
io.println(r);  // ababab
```

### str.char_at(s, i)

Returns the ASCII code of the character at index `i` (zero-based):

```silica
int c = str.char_at("Hello", 1);  // 101 ('e')
```

### str.eq(a, b)

Returns `1` if strings `a` and `b` are equal, `0` otherwise:

```silica
io.println(str.eq("abc", "abc"));  // 1
io.println(str.eq("abc", "xyz"));  // 0
```

---

## std.fs

**Import:** `import std.fs;`

File system operations via Linux syscalls. All paths are standard Unix paths.

### fs.create(path)

Creates a file at `path`. If `path` ends with `/`, creates a directory instead:

```silica
fs.create("/tmp/myfile.txt");
fs.create("/tmp/mydir/");
```

### fs.open(path, mode) → `int`

Opens a file and returns a file descriptor. `mode`:
- `0` — read-only
- `1` — write (creates/truncates)
- `2` — append (creates if absent)

Returns a non-negative fd on success, negative on failure:

```silica
int fd = fs.open("/tmp/myfile.txt", 1);
if (fd < 0) { io.println("open failed"); }
```

### fs.close(fd)

Closes a file descriptor:

```silica
fs.close(fd);
```

### fs.write(fd, content) → `int`

Writes string `content` to file descriptor `fd`. Returns the number of bytes written:

```silica
int n = fs.write(fd, "Hello, file!\n");
io.println(n);   // 13
```

### fs.read_all(fd) → `string`

Reads the entire contents of a file descriptor into a heap-allocated string:

```silica
int fd = fs.open("/tmp/myfile.txt", 0);
string contents = fs.read_all(fd);
io.println(contents);
fs.close(fd);
```

### fs.size(path) → `int`

Returns the size of the file at `path` in bytes. Returns `-1` if the file doesn't exist:

```silica
int sz = fs.size("/tmp/myfile.txt");
io.println(sz);
```

### fs.append(path, content)

Appends `content` to the file at `path` (convenience wrapper — opens, writes, closes):

```silica
fs.append("/tmp/myfile.txt", "Hello, Silica!\n");
```

### fs.read(path, line) → `string`

Returns the `line`-th line of the file at `path` (1-based):

```silica
string first = fs.read("/tmp/myfile.txt", 1);
```

### fs.delete(path)

Deletes the file at `path`:

```silica
fs.delete("/tmp/myfile.txt");
```

---

## std.mem

**Import:** `import std.mem;`

Raw memory allocation via `sys_brk`.

### mem.alloc(n, unit) → `void*`

Allocates `n * unit` bytes of heap memory and **returns a pointer** to the allocated region. `unit` is typically `0` (bytes), `1` (KB), `2` (MB), or `3` (GB):

```silica
void* buf  = mem.alloc(256, 0);    // 256 bytes
void* kb   = mem.alloc(4, 1);      // 4 KB
void* big  = mem.alloc(1, 2);      // 1 MB
```

The returned pointer is a valid heap address. It can be stored in a `void*` variable, compared, or cast to a typed pointer:

```silica
void* buf = mem.alloc(64, 0);
int* iptr = buf;
*iptr = 42;
```

### mem.alloc_raw(bytes) → `void*`

Allocates exactly `bytes` bytes and returns a pointer. More convenient when the byte count is a variable:

```silica
int size = 1024;
void* buf = mem.alloc_raw(size);
```

### mem.free(ptr)

Accepts any pointer. Currently a no-op — the `sys_brk` allocator does not support freeing individual regions. Included for forward compatibility:

```silica
void* buf = mem.alloc(256, 0);
// ... use buf ...
mem.free(buf);   // no-op, safe to call
```

---

## std.time

**Import:** `import std.time;`

Time functions via Linux `clock_gettime` and `nanosleep` syscalls.

### time.now()

Returns the current Unix timestamp in seconds:

```silica
int t = time.now();
io.println(t);  // e.g. 1708900000
```

### time.now_ms()

Returns the current Unix timestamp in milliseconds:

```silica
int ms = time.now_ms();
```

### time.mono()

Returns a monotonic clock value in seconds. Useful for measuring elapsed time without wall-clock drift:

```silica
int start = time.mono();
// ... do work ...
int end = time.mono();
```

### time.sleep(ms)

Sleeps for `ms` milliseconds:

```silica
time.sleep(500);   // sleep 500ms
time.sleep(1000);  // sleep 1 second
```

---

## std.net

**Import:** `import std.net;`

TCP networking via raw Linux socket syscalls.

### net.ip(a, b, c, d)

Packs an IPv4 address into a 32-bit integer:

```silica
int ip = net.ip(93, 184, 216, 34);   // 93.184.216.34
```

### net.connect(ip, port)

Opens a TCP connection to `ip:port`. Returns a file descriptor on success, or a negative value on failure:

```silica
int ip = net.ip(93, 184, 216, 34);
int fd = net.connect(ip, 80);
if (fd < 0) {
    io.println("Connection failed.");
}
```

### net.send(fd, data)

Sends string `data` over file descriptor `fd`. Returns the number of bytes sent:

```silica
int sent = net.send(fd, "GET / HTTP/1.0\r\n\r\n");
```

### net.recv(fd, maxbytes)

Receives up to `maxbytes` bytes from `fd`. Returns the data as a string:

```silica
string resp = net.recv(fd, 512);
io.println(resp);
```

### net.close(fd)

Closes file descriptor `fd`:

```silica
net.close(fd);
```

---

## std.env

**Import:** `import std.env;`

Access to process arguments and environment variables.

### env.argc()

Returns the number of command-line arguments (including the program name):

```silica
int n = env.argc();
io.println(n);
```

### env.argv(i)

Returns the `i`-th command-line argument as a string (zero-indexed; `argv(0)` is the program name):

```silica
string prog = env.argv(0);
string first_arg = env.argv(1);
```

### env.get(name)

Returns the value of environment variable `name`, or an empty string if not set:

```silica
string path = env.get("PATH");
string home = env.get("HOME");
```

---

## std.proc

**Import:** `import std.proc;`

Process information.

### proc.pid()

Returns the current process ID:

```silica
int pid = proc.pid();
io.print("PID: ");
io.println(pid);
```

### proc.exit(code)

Immediately exits the process with the given exit code. Prefer setting `name.errorcode` in the main block for normal exits:

```silica
proc.exit(1);   // exit with error code 1
```

---

## std.main

**Import:** `import std.main;`

This import is required in every executable Silica file that has a `main` block. It enables the `main` keyword and the `name.errorcode` exit mechanism. It has no callable functions.