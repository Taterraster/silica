#define _GNU_SOURCE
/*
 * silicac -- The Silica Language Compiler Snapshot v0.0.2-a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <ctype.h>

/* ---------- utilities ---------- */

static int has_slc_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;

    return strcasecmp(dot, ".slc") == 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fputs("OOM\n", stderr); exit(1); }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}
static char *read_file_req(const char *path) {
    char *s = read_file(path);
    if (!s) { perror(path); exit(1); }
    return s;
}

static void replace_ext(const char *src, const char *ext, char *out, int n) {
    strncpy(out, src, n-1); out[n-1]='\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strncat(out, ext, n - strlen(out) - 1);
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s+1 : path;
}

/* ---------- debug dumps ---------- */

static void dump_tokens(const char *src) {
    Lexer l; lexer_init(&l, src);
    printf("%-5s %-5s %-16s %s\n","LINE","COL","TYPE","VALUE");
    printf("%-5s %-5s %-16s %s\n","----","---","----","-----");
    Token t;
    do {
        t = lexer_next(&l);
        printf("%-5d %-5d %-16s %s\n", t.line, t.col,
               token_type_name(t.type), t.value ? t.value : "");
        int done = (t.type==TOK_EOF||t.type==TOK_ERROR);
        token_free(&t);
        if (done) break;
    } while(1);
}

static void do_indent(int d) { for(int i=0;i<d*2;i++) putchar(' '); }
static void dump_expr(Expr *e, int d) {
    if (!e) return; do_indent(d);
    switch(e->kind) {
        case EXPR_INT_LIT:    printf("INT(%ld)\n",    e->ival); break;
        case EXPR_CHAR_LIT:   printf("CHAR('%c')\n",  e->cval); break;
        case EXPR_STRING_LIT: printf("STR(\"%s\")\n", e->sval); break;
        case EXPR_IDENT:      printf("IDENT(%s)\n",   e->sval); break;
        case EXPR_FIELD:
            printf("FIELD(.%s)\n", e->field);
            dump_expr(e->object, d+1); break;
        case EXPR_CALL:
            printf("CALL\n");
            do_indent(d+1); printf("callee:\n"); dump_expr(e->callee, d+2);
            do_indent(d+1); printf("args (%d):\n", e->argc);
            for(int i=0;i<e->argc;i++) dump_expr(e->args[i], d+2); break;
        default: printf("EXPR(%d)\n", e->kind);
    }
}
static void dump_stmt(Stmt *s, int d) {
    if (!s) return; do_indent(d);
    switch(s->kind) {
        case STMT_VAR_DECL:
            printf("VAR_DECL %s\n", s->varname);
            if (s->init) { do_indent(d+1); printf("init:\n"); dump_expr(s->init,d+2); }
            break;
        case STMT_EXPR: printf("EXPR_STMT\n"); dump_expr(s->expr,d+1); break;
        default: printf("STMT(%d)\n", s->kind);
    }
}
static void dump_ast(Program *prog) {
    printf("Program:\n");
    for(int i=0;i<prog->nimports;i++) printf("  import %s\n",prog->imports[i].module);
    if(prog->mainfn) {
        printf("  main %s()\n",prog->mainfn->name);
        for(int i=0;i<prog->mainfn->nstmts;i++) dump_stmt(prog->mainfn->stmts[i],2);
    }
}

/* ==========================================================================
 * Module / library system
 * ==========================================================================
 *
 * For "import foo;" (where foo does NOT start with "std."):
 *   1. foo.slh exists -> parse as library, merge its functions into current prog
 *   2. foo.slc exists -> compile it to a .o, add to extra_objs for linking
 *
 * Search path: directory of the importing file, then current directory.
 */

static char *find_module(const char *base_dir, const char *mod,
                          const char *ext, char *out, int n) {
    snprintf(out, n, "%s/%s%s", base_dir, mod, ext);
    if (access(out, R_OK) == 0) return out;
    snprintf(out, n, "./%s%s", mod, ext);
    if (access(out, R_OK) == 0) return out;
    return NULL;
}

static void merge_funcs(Program *dst, Program *src) {
    for(int i=0;i<src->nfuncs;i++) {
        dst->funcs = realloc(dst->funcs, (dst->nfuncs+1)*sizeof(FuncDecl*));
        dst->funcs[dst->nfuncs++] = src->funcs[i];
        src->funcs[i] = NULL;
    }
    src->nfuncs = 0;
}

/* path to this executable, set in main() for self-invocation */
static char self_path[512] = "silicac";

static char *compile_module_obj(const char *slc_path) {
    const char *base = basename_of(slc_path);
    char obj[512];
    snprintf(obj, sizeof(obj), "/tmp/__silica_%s", base);
    char *dot = strrchr(obj, '.');
    if (dot) strcpy(dot, ".o"); else strncat(obj, ".o", sizeof(obj)-strlen(obj)-1);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" -c \"%s\" -o \"%s\"", self_path, slc_path, obj);
    if (system(cmd) != 0) {
        fprintf(stderr, "[silicac] Failed to compile module: %s\n", slc_path);
        return NULL;
    }
    return strdup(obj);
}

static int resolve_imports(Program *prog, const char *base_dir,
                            char **extra_objs, int *n_extra) {
    for(int i=0;i<prog->nimports;i++) {
        const char *mod = prog->imports[i].module;
        /* skip std.* and pseudo-imports */
        if (strncmp(mod,"std.",4)==0 || strcmp(mod,"std")==0 ||
            strcmp(mod,"std.main")==0 || strcmp(mod,"std.loops")==0) continue;

        char path[512];

        /* try .slh library */
        if (find_module(base_dir, mod, ".slh", path, sizeof(path))) {
            char *lib_src = read_file(path);
            if (!lib_src) { fprintf(stderr,"[silicac] Cannot read %s\n",path); return 1; }
            Parser lp; parser_init(&lp, lib_src);
            Program *lib = parser_parse(&lp);
            if (lp.errors) {
                fprintf(stderr,"[silicac] Errors in library %s\n",path);
                program_free(lib); free(lib_src); return 1;
            }
            if (lib->mainfn) {
                fprintf(stderr,"[silicac] %s: .slh files cannot have a main function\n",path);
                program_free(lib); free(lib_src); return 1;
            }
            merge_funcs(prog, lib);
            program_free(lib); free(lib_src);
            printf("[silicac] Imported library: %s\n", path);
            continue;
        }

        /* try .slc module */
        if (find_module(base_dir, mod, ".slc", path, sizeof(path))) {
            if (*n_extra >= 64) { fprintf(stderr,"[silicac] Too many modules\n"); return 1; }
            char *obj = compile_module_obj(path);
            if (!obj) return 1;
            extra_objs[(*n_extra)++] = obj;

            /* also parse it to extract extern function stubs for call resolution */
            char *mod_src = read_file(path);
            if (mod_src) {
                Parser mp; parser_init(&mp, mod_src);
                Program *mod_prog = parser_parse(&mp);
                if (!mp.errors) {
                    for (int j = 0; j < mod_prog->nfuncs; j++) {
                        FuncDecl *fd = mod_prog->nfuncs ? mod_prog->funcs[j] : NULL;
                        if (!fd) continue;
                        /* inject as extern stub (no stmts) */
                        fd->is_extern = 1;
                        fd->stmts = NULL;
                        fd->nstmts = 0;
                        prog->funcs = realloc(prog->funcs, (prog->nfuncs+1)*sizeof(FuncDecl*));
                        prog->funcs[prog->nfuncs++] = fd;
                        mod_prog->funcs[j] = NULL;
                    }
                    mod_prog->nfuncs = 0;
                }
                program_free(mod_prog);
                free(mod_src);
            }

            printf("[silicac] Compiled module: %s\n", path);
            continue;
        }

        fprintf(stderr,"[silicac] Cannot find module '%s' (tried %s.slh and %s.slc)\n",
                mod, mod, mod);
        return 1;
    }
    return 0;
}

/* ==========================================================================
 * REPL
 * ==========================================================================
 * Each line is wrapped in a minimal program, compiled to a temp binary, run.
 * Variable declarations are accumulated in a "preamble" so they persist
 * across turns.
 */

static const char *REPL_IMPORTS =
    "import std.io;\n"
    "import std.str;\n"
    "import std.math;\n"
    "import std.time;\n"
    "import std.env;\n"
    "import std.proc;\n"
    "import std.fs;\n"
    "import std.mem;\n";

static void run_repl(void) {
    printf("Silica REPL Snapshot v0.0.2-a  (Ctrl+D to exit)\n");
    char preamble[65536] = "";
    char extra_imports[4096] = "";
    char line[4096];
    int turn = 0;

    while (1) {
        printf("silica> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { printf("\n"); break; }

        /* trim newline */
        int len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        /* add semicolon if missing */
        if (line[len-1] != ';' && line[len-1] != '}')
            strncat(line, ";", sizeof(line)-len-1), len++;

        /* extra user import */
        if (strncmp(line, "import ", 7) == 0) {
            strncat(extra_imports, line, sizeof(extra_imports)-strlen(extra_imports)-2);
            strncat(extra_imports, "\n", sizeof(extra_imports)-strlen(extra_imports)-1);
            printf("(import registered)\n");
            continue;
        }

        /* classify input */
        int is_decl = strncmp(line,"int ",4)==0 || strncmp(line,"float ",6)==0 ||
                      strncmp(line,"string ",7)==0 || strncmp(line,"bool ",5)==0 ||
                      strncmp(line,"char ",5)==0 || strncmp(line,"const ",6)==0;
        int is_io   = strncmp(line,"io.",3)==0;
        int is_ctrl = strncmp(line,"if(",3)==0 || strncmp(line,"if (",4)==0 ||
                      strncmp(line,"loops.",6)==0;

        /* build body statement: auto-wrap bare expressions in io.println */
        char stmt[4200];
        if (!is_decl && !is_io && !is_ctrl) {
            /* strip trailing ; to embed as expression */
            char expr[4096]; strncpy(expr, line, sizeof(expr)-1);
            if (strlen(expr) > 0 && expr[strlen(expr)-1] == ';')
                expr[strlen(expr)-1] = '\0';
            snprintf(stmt, sizeof(stmt), "io.println(%s);", expr);
        } else {
            strncpy(stmt, line, sizeof(stmt)-1);
        }

        /* write temp .slc */
        char tmp_slc[64], tmp_bin[64];
        snprintf(tmp_slc, sizeof(tmp_slc), "/tmp/__repl%d.slc", turn);
        snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/__repl%d",     turn);

        FILE *f = fopen(tmp_slc, "w");
        if (!f) { perror(tmp_slc); continue; }
        fprintf(f, "%s%simport std.main;\nmain __repl%d(){\n%s\n%s\n__repl%d.errorcode=0;\n}\n",
                REPL_IMPORTS, extra_imports, turn,
                preamble, stmt, turn);
        fclose(f);

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "\"%s\" %s -o %s 2>/dev/null", self_path, tmp_slc, tmp_bin);
        if (system(cmd) != 0) {
            /* retry without auto-println */
            if (!is_decl && !is_io && !is_ctrl) {
                FILE *f2 = fopen(tmp_slc, "w");
                fprintf(f2, "%s%simport std.main;\nmain __repl%d(){\n%s\n%s\n__repl%d.errorcode=0;\n}\n",
                        REPL_IMPORTS, extra_imports, turn,
                        preamble, line, turn);
                fclose(f2);
                snprintf(cmd, sizeof(cmd), "\"%s\" %s -o %s 2>/dev/null", self_path, tmp_slc, tmp_bin);
            }
            if (system(cmd) != 0) {
                /* show actual error */
                snprintf(cmd, sizeof(cmd), "\"%s\" %s -o %s", self_path, tmp_slc, tmp_bin);
                system(cmd);
                remove(tmp_slc);
                continue;
            }
        }
        remove(tmp_slc);
        snprintf(cmd, sizeof(cmd), "%s", tmp_bin);
        system(cmd);
        remove(tmp_bin);

        /* accumulate declarations */
        if (is_decl) {
            strncat(preamble, line,  sizeof(preamble)-strlen(preamble)-2);
            strncat(preamble, "\n", sizeof(preamble)-strlen(preamble)-1);
        }
        turn++;
    }
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(int argc, char **argv) {
    /* record own path for module self-compilation */
    if (argc > 0) {
        strncpy(self_path, argv[0], sizeof(self_path)-1);
        self_path[sizeof(self_path)-1] = '\0';
    }
    if (argc < 2) {
        fprintf(stderr, "Silica Compiler Snapshot v0.0.2-a\nUsage: silicac [options] <source.slc>\n       silicac --repl\n       silicac --help\n");
        return 1;
    }

    int flag_tokens=0, flag_ast=0, flag_asm_only=0;
    int flag_obj_only=0, flag_help=0, flag_repl=0;
	int flag_version=0;
    const char *input=NULL, *output=NULL;

    for (int i=1;i<argc;i++) {
        if      (!strcmp(argv[i],"--tokens")) flag_tokens=1;
        else if (!strcmp(argv[i],"--ast"))    flag_ast=1;
        else if (!strcmp(argv[i],"-S"))       flag_asm_only=1;
        else if (!strcmp(argv[i],"-c"))       flag_obj_only=1;
        else if (!strcmp(argv[i],"--repl"))   flag_repl=1;
        else if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) flag_help=1;
		else if (strcmp(argv[i], "--version")   == 0 ||
                 strcmp(argv[i], "-v")       == 0 ||
		         strcmp(argv[i], "-version")   == 0 ||
                 strcmp(argv[i], "--v")       == 0) flag_version = 1;
        else if (!strcmp(argv[i],"-o")&&i+1<argc) output=argv[++i];
        else if (argv[i][0]!='-') input=argv[i];
    }

    if (flag_repl) { run_repl(); return 0; }
	if (flag_version){
		puts("silicac -- Silica Language Compiler Snapshot v0.0.2-a");
		return 0;
	}
    if (flag_help) {
        puts(
            "silicac -- Silica Language Compiler Snapshot v0.0.2-a\n"
            "\n"
            "Usage:\n"
            "  silicac <source.slc>               Compile to ./a.out\n"
            "  silicac <source.slc> -o <out>      Compile to named binary\n"
            "  silicac <source.slc> -c            Compile+assemble to .o\n"
            "  silicac <source.slc> -S            Emit assembly only\n"
            "  silicac --tokens <source.slc>      Dump token stream\n"
            "  silicac --ast    <source.slc>      Dump AST\n"
            "  silicac --repl                     Interactive REPL\n"
			"  silicac -v or --version            Show version\n"
			"  silicac -h or --help               Show this message"
			);
        return 0;
    }
	

	if (!has_slc_extension(input) && file_exists(input)) {
    	fprintf(stderr, "[silicac] Error: source file must have .slc extension\n");
    	return 1;
	}
	else if (!has_slc_extension(input) && !file_exists(input)) {
		fprintf(stderr, "[silicac] Error: source file doesnt exist\n");
    	return 1;
	}
	else if (has_slc_extension(input) && !file_exists(input)) {
		fprintf(stderr, "[silicac] Error: source file doesnt exist\n");
    	return 1;
	}
	else if (has_slc_extension(input) && file_exists(input)) {
		
	}
	char *src = read_file_req(input);

    if (flag_tokens) { dump_tokens(src); free(src); return 0; }

    Parser p; parser_init(&p, src);
    Program *prog = parser_parse(&p);

    if (flag_ast) { dump_ast(prog); program_free(prog); free(src); return 0; }

    if (p.errors) {
        fprintf(stderr,"[silicac] %d parse error(s). Aborting.\n",p.errors);
        program_free(prog); free(src); return 1;
    }

    /* resolve module imports */
    char base_dir[512]; strncpy(base_dir,input,sizeof(base_dir)-1);
    char *last_slash = strrchr(base_dir,'/');
    if (last_slash) *last_slash='\0'; else strcpy(base_dir,".");

    char *extra_objs[64]; int n_extra=0;
    if (resolve_imports(prog,base_dir,extra_objs,&n_extra)!=0) {
        program_free(prog); free(src); return 1;
    }

    /* code generation */
    char asm_path[512];
    if (flag_asm_only && output) strncpy(asm_path,output,sizeof(asm_path)-1);
    else replace_ext(input,".s",asm_path,sizeof(asm_path));

    FILE *asm_f = fopen(asm_path,"w");
    if (!asm_f) { perror(asm_path); return 1; }
    int cg_err = codegen_emit(prog,asm_f);
    fclose(asm_f);
    if (cg_err) {
        remove(asm_path);
        fprintf(stderr,"[silicac] Compilation failed.\n");
        program_free(prog); free(src); return 1;
    }

    if (flag_asm_only) {
        printf("[silicac] Assembly written to %s\n",asm_path);
        program_free(prog); free(src); return 0;
    }

    /* assemble */
    char obj_path[512];
    if (flag_obj_only && output) strncpy(obj_path,output,sizeof(obj_path)-1);
    else replace_ext(input,".o",obj_path,sizeof(obj_path));

    char cmd[2048];
    snprintf(cmd,sizeof(cmd),"as --64 -o %s %s",obj_path,asm_path);
    if (system(cmd)!=0) { fprintf(stderr,"[silicac] Assembler failed.\n"); return 1; }
    remove(asm_path);

    if (flag_obj_only) {
        printf("[silicac] Object written to %s\n",obj_path);
        program_free(prog); free(src); return 0;
    }

    /* link — include extra module .o files */
    const char *out_path = output ? output : "a.out";
    char obj_list[2048]; strncpy(obj_list,obj_path,sizeof(obj_list)-1);
    for (int i=0;i<n_extra;i++) {
        strncat(obj_list," ",sizeof(obj_list)-strlen(obj_list)-1);
        strncat(obj_list,extra_objs[i],sizeof(obj_list)-strlen(obj_list)-1);
    }

    snprintf(cmd,sizeof(cmd),"ld -o %s -static %s --entry _start -z noexecstack",
             out_path,obj_list);
    if (system(cmd)!=0) { fprintf(stderr,"[silicac] Linker failed.\n"); return 1; }
    chmod(out_path,0755);
    remove(obj_path);
    for (int i=0;i<n_extra;i++) { remove(extra_objs[i]); free(extra_objs[i]); }

    printf("[silicac] Compiled %s --> %s\n",input,out_path);
    program_free(prog); free(src);
    return 0;
}