#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

/*
 * codegen_emit:
 *   Walks the AST and emits x86-64 Linux AT&T assembly (.s file).
 *   lib_mode=1: emit plain symbol names (no __silica_user_ prefix) for C/C++ interop.
 *   Returns 0 on success, 1 if a compile error was detected (e.g. missing import).
 */
int codegen_emit(Program *prog, FILE *out, int lib_mode);

#endif