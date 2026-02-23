# Silica Compiler – Makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
TARGET  = build/silicac

SRCS    = src/main.c src/lexer.c src/parser.c src/ast.c src/codegen.c
OBJS    = $(patsubst src/%.c, build/%.o, $(SRCS))

.PHONY: all clean test install

all: $(TARGET)

# Link
$(TARGET): $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^

# Compile each .c from src/ into build/
build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -I src -c -o $@ $<

# Create build/ if it doesn't exist
build:
	mkdir -p build

# Explicit header dependencies (so changes to headers rebuild the right objects)
build/main.o:    src/main.c    src/lexer.h src/parser.h src/codegen.h src/ast.h
build/lexer.o:   src/lexer.c   src/lexer.h
build/parser.o:  src/parser.c  src/parser.h src/lexer.h src/ast.h
build/ast.o:     src/ast.c     src/ast.h
build/codegen.o: src/codegen.c src/codegen.h src/ast.h

# ── Tests ────────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo "=== Running Silica test suite ==="
	@passed=0; failed=0; \
	for f in tests/*.slc; do \
	    out=$$(./$(TARGET) "$$f" -o /tmp/__slc_test 2>&1); \
	    if echo "$$out" | grep -qE '\[error\]|Linker failed|parse error'; then \
	        echo "  FAIL $$f"; failed=$$((failed+1)); \
	    else \
	        echo "  PASS $$f"; passed=$$((passed+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "Results: $$passed passed, $$failed failed"

# ── Install ───────────────────────────────────────────────────────────────────
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/silicac
	@echo "Installed to /usr/local/bin/silicac"

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf build
	rm -f tests/*.s tests/*.o
