#include <libeezo/mem.h>
/*
 * main.c - Eezo compiler
 *
 * Compiles .eezo source files to BCL/Jot/Jomplement bytecode.
 * Use eezo (the evaluator) to run the compiled output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <libeezo/term.h>
#include <libeezo/bcl.h>
#include <libeezo/jomplement.h>
#include <libeezo/native.h>
#include "ast.h"
#include "bracket.h"
#include "compile.h"
#include "parse/tree.h"
#include "parse/array.h"
#include "parse/term.h"
#include "parse/parse.h"
#include "parse/opp/operator.h"

/* the installed library: the mkfile passes -DSTDLIB_DIR=$PREFIX/share/eezo/stdlib (where 'mk install' in the stdlib repository
   puts the modules), stringified here since mk quotes nothing; EEZO_STDLIB in the environment overrides it at run time */
#define STDLIB_STR_(x) #x
#define STDLIB_STR(x) STDLIB_STR_(x)
#ifdef STDLIB_DIR
#define STDLIB_PATH STDLIB_STR(STDLIB_DIR)
#else
#define STDLIB_PATH "/usr/local/share/eezo/stdlib"
#endif

/* Forward declaration - defined in parse/lexeme.c */
void set_file_boundaries_ex(int n, const char **filenames, const unsigned short *start_lines,
                            const unsigned short *import_counts);

bool TRACE = false;

static char *read_file_contents(const char *path) {
    FILE *f;
    int is_stdin = (strcmp(path, "-") == 0);
    
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(path, "r");
        if (!f) {
            perror(path);
            return NULL;
        }
    }
    
    /* Check if seekable (regular file) */
    if (!is_stdin && fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char *buf = rmalloc(size + 1);
        
        size_t nread = fread(buf, 1, size, f);
        buf[nread] = '\0';
        fclose(f);
        return buf;
    }
    
    /* Non-seekable (pipe, stdin): read in chunks */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = rmalloc(cap);
    
    size_t nread;
    while ((nread = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += nread;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = rrealloc(buf, cap);
        }
    }
    buf[len] = '\0';
    if (!is_stdin) fclose(f);
    return buf;
}

/* Cache for stdin content (can only read once) */
static char *stdin_cache = NULL;

static char *read_file_or_cache(const char *path) {
    if (strcmp(path, "-") == 0) {
        if (!stdin_cache) {
            stdin_cache = read_file_contents("-");
        }
        return stdin_cache ? rstrdup(stdin_cache) : NULL;
    }
    return read_file_contents(path);
}

/*
 * Import graph and toposort
 */

/* the graph on Stacks of the memory layer (libeezo/mem.h): any number of files, and of imports per file */
typedef struct {
    char *path;
    Stack imports;   /* char * */
    int visited;  /* 0=unvisited, 1=visiting, 2=done */
} FileNode;

static Stack files = { NULL, 0, 0, sizeof(FileNode) };
#define FILE_AT(i) STACK_AT(&files, FileNode, i)
static Stack sorted_files = { NULL, 0, 0, sizeof(char *) };

static int find_or_add_file(const char *path) {
    for (size_t i = 0; i < files.n; i++) {
        if (strcmp(FILE_AT(i).path, path) == 0) return (int)i;
    }
    FileNode *f = stack_push(&files);
    f->path = rstrdup(path);
    stack_init(&f->imports, sizeof(char *));
    f->visited = 0;
    return (int)files.n - 1;
}

/* Extract imports from file content - lines starting with "import " */
static void scan_imports(int idx) {
    char *content = read_file_or_cache(FILE_AT(idx).path);
    if (!content) return;
    
    char *line = content;
    while (*line) {
        /* Skip whitespace */
        while (*line == ' ' || *line == '\t') line++;
        
        /* Check for "#import " */
        if (strncmp(line, "#import ", 8) == 0) {
            line += 8;
            while (*line == ' ' || *line == '\t') line++;
            
            /* Extract module name until newline/space */
            char *start = line;
            while (*line && *line != '\n' && *line != ' ' && *line != '\t') line++;
            
            if (line > start) {
                size_t len = line - start;
                char *imp = rmalloc(len + 1);
                memcpy(imp, start, len);
                imp[len] = '\0';
                STACK_PUSH(&FILE_AT(idx).imports, char *, imp);
            }
        }
        
        /* Skip to next line */
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    free(content);
}

/* Resolve import name to file path */
static char *resolve_import(const char *base_path, const char *import_name) {
    /* Get directory of base file */
    char *dir = rstrdup(base_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = '\0';
    } else {
        free(dir);
        dir = rstrdup("./");
    }
    
    /* Try 1: relative to importing file */
    size_t len = strlen(dir) + strlen(import_name) + 6;
    char *path = rmalloc(len + 1);
    snprintf(path, len + 1, "%s%s.eezo", dir, import_name);
    
    if (access(path, F_OK) == 0) {
        free(dir);
        return path;
    }
    free(path);
    
    /* Try 2: in stdlib/ relative to importing file */
    len = strlen(dir) + 7 + strlen(import_name) + 6;
    path = rmalloc(len + 1);
    snprintf(path, len + 1, "%sstdlib/%s.eezo", dir, import_name);
    
    if (access(path, F_OK) == 0) {
        free(dir);
        return path;
    }
    free(path);
    
    /* Try 3: the installed library: $EEZO_STDLIB if set, else the STDLIB_PATH compiled in */
    const char *stdlib_path = getenv("EEZO_STDLIB"); if (!stdlib_path || !*stdlib_path) stdlib_path = STDLIB_PATH;
    len = strlen(stdlib_path) + 1 + strlen(import_name) + 6;
    path = rmalloc(len + 1);
    snprintf(path, len + 1, "%s/%s.eezo", stdlib_path, import_name);

    free(dir);
    return path;  /* Return this even if not found - will fail later with clear error */
}

/* Toposort via DFS - returns 0 on success, -1 on cycle */
static int toposort_visit(int idx, const char *base_path) {
    if (FILE_AT(idx).visited == 2) return 0;  /* Already done */
    if (FILE_AT(idx).visited == 1) {
        fprintf(stderr, "Circular import: %s\n", FILE_AT(idx).path);
        return -1;
    }
    
    FILE_AT(idx).visited = 1;  /* Visiting */
    
    /* Visit dependencies first */
    for (size_t i = 0; i < FILE_AT(idx).imports.n; i++) {
        char *dep_path = resolve_import(FILE_AT(idx).path, STACK_AT(&FILE_AT(idx).imports, char *, i));
        int dep_idx = find_or_add_file(dep_path);
        if (dep_idx < 0) {
            free(dep_path);
            return -1;
        }
        if (FILE_AT(dep_idx).imports.n == 0) {
            scan_imports(dep_idx);
        }
        if (toposort_visit(dep_idx, FILE_AT(idx).path) < 0) {
            free(dep_path);
            return -1;
        }
        free(dep_path);
    }
    
    FILE_AT(idx).visited = 2;  /* Done */
    STACK_PUSH(&sorted_files, char *, FILE_AT(idx).path);
    return 0;
}

/* Build import graph and return toposorted file list */
static char **resolve_imports(int nfiles, char **paths, int *out_count) {
    files.n = 0;
    sorted_files.n = 0;
    
    /* Add all input files */
    for (int i = 0; i < nfiles; i++) {
        int idx = find_or_add_file(paths[i]);
        scan_imports(idx);
    }
    
    /* Toposort from each input file */
    for (int i = 0; i < nfiles; i++) {
        int idx = find_or_add_file(paths[i]);
        if (toposort_visit(idx, paths[i]) < 0) return NULL;
    }
    
    *out_count = (int)sorted_files.n;
    return (char **)sorted_files.p;
}

/* Free import graph */
static void free_import_graph(void) {
    for (size_t i = 0; i < files.n; i++) {
        free(FILE_AT(i).path);
        for (size_t j = 0; j < FILE_AT(i).imports.n; j++) free(STACK_AT(&FILE_AT(i).imports, char *, j));
        stack_drop(&FILE_AT(i).imports);
    }
    files.n = 0;
    sorted_files.n = 0;
}

/* Concatenate files, stripping import lines */
static char *concat_files_strip_imports(int nfiles, char **paths) {
    /* Read all files first (handles stdin/pipes) */
    char **contents = rmalloc(nfiles * sizeof(char*));
    
    size_t total = 0;
    for (int i = 0; i < nfiles; i++) {
        contents[i] = read_file_or_cache(paths[i]);
        if (!contents[i]) {
            for (int j = 0; j < i; j++) free(contents[j]);
            free(contents);
            return NULL;
        }
        total += strlen(contents[i]) + 2;  /* +2 for newline and safety */
    }
    
    char *buf = rmalloc(total + 1);
    
    /* Track file boundaries for error reporting */
    /* one boundary per file (the parse keeps its own copy: set_file_boundaries_ex) */
    const char **boundary_files = rmalloc((nfiles + 1) * sizeof(char *));
    unsigned short *boundary_starts = rmalloc((nfiles + 1) * sizeof(unsigned short));
    unsigned short *boundary_import_counts = rmalloc((nfiles + 1) * sizeof(unsigned short));
    int num_bounds = 0;
    unsigned short current_line = 1;
    
    /* Process files, skip import lines */
    char *p = buf;
    for (int i = 0; i < nfiles; i++) {
        /* Record boundary start */
        int bound_idx = num_bounds;
        boundary_files[num_bounds] = rstrdup(paths[i]);
        boundary_starts[num_bounds] = current_line;
        boundary_import_counts[num_bounds] = 0;
        num_bounds++;
        
        char *line = contents[i];
        while (*line) {
            char *line_start = line;
            
            /* Skip leading whitespace for import check */
            char *check = line;
            while (*check == ' ' || *check == '\t') check++;
            
            /* Find end of line */
            while (*line && *line != '\n') line++;
            int has_newline = (*line == '\n');
            if (has_newline) line++;
            
            /* Skip #import lines */
            if (strncmp(check, "#import ", 8) != 0) {
                size_t len = line - line_start;
                memcpy(p, line_start, len);
                p += len;
                current_line++;
            } else {
                /* Count skipped imports */
                boundary_import_counts[bound_idx]++;
            }
        }
        free(contents[i]);
        *p++ = '\n';
        current_line++;
    }
    free(contents);
    *p = '\0';
    
    /* Set file boundaries for error reporting */
    set_file_boundaries_ex(num_bounds, boundary_files, boundary_starts, boundary_import_counts);
    free(boundary_starts); free(boundary_import_counts); free(boundary_files);   /* the names themselves stay: the parse holds them */
    
    return buf;
}

/* Output encoding */
typedef enum {
    EMIT_BCL,
    EMIT_JOT,
    EMIT_JOMPLEMENT,
    EMIT_XBCL,
} EmitMode;

/* Convert EmitMode to OutputFormat for native backend */
static OutputFormat emit_mode_to_output_format(EmitMode mode) {
    switch (mode) {
        case EMIT_BCL: return OUTPUT_BCL;
        case EMIT_JOT: return OUTPUT_JOT;
        case EMIT_JOMPLEMENT: return OUTPUT_JOMPLEMENT;
        case EMIT_XBCL: return OUTPUT_XBCL;
    }
    return OUTPUT_BCL;
}

/* Compile files with import resolution */
/* returns 0 on success, 1 on any failure: the exit status of the compiler (a failed compilation must not look like one) */
static int compile_files(int nfiles, char **files, AstPool *ap, SKIPool *tp, 
                          int verbose, EmitMode mode, int emit_elf, int nf_mode, u64 heap_size, int io_mode) {
    /* Resolve imports and toposort */
    int sorted_count;
    char **sorted = resolve_imports(nfiles, files, &sorted_count);
    if (!sorted) {
        fprintf(stderr, "Failed to resolve imports\n");
        return 1;
    }
    
    if (verbose) {
        fprintf(stderr, "Compilation order (%d files):\n", sorted_count);
        for (int i = 0; i < sorted_count; i++) {
            fprintf(stderr, "  %d: %s\n", i, sorted[i]);
        }
    }
    
    /* Concatenate in order, stripping import lines */
    char *source = concat_files_strip_imports(sorted_count, sorted);
    free_import_graph();
    
    if (!source) {
        fprintf(stderr, "Failed to read source files\n");
        return 1;
    }
    
    /* Compile */
    SKITerm *ski = compile_to_ski(ap, tp, source);
    if (ski) {
        if (verbose) {
            fprintf(stderr, "SKI: ");
            ski_fprint(stderr, ski);
            fprintf(stderr, "\n");
        }
        
        /* ELF mode - emit standalone executable */
        if (emit_elf) {
            /* Initialize and emit runtime with correct output format (the emitter owns its growable code buffer) */
            NativeEmit e;
            native_emit_init(&e, emit_mode_to_output_format(mode));
            e.nf_mode = nf_mode;
            e.io_mode = io_mode;
            native_emit_runtime(&e);
            
            /* Emit ELF */
            u8 *elf;
            u64 elf_size;
            /* The ELF evaluates the term as the chosen format would carry it: a pure format
             * spells B C T R as S K trees (and cannot spell words), XBCL keeps the leaves. */
            if (mode != EMIT_XBCL) {
                SKITerm *pure = ski_expand_pure(tp, ski);
                if (!pure) {
                    fprintf(stderr, "Error: the program uses machine words; emit it with -f xbcl\n");
                    free(source);
                    return 1;
                }
                ski_unref(tp, ski);
                ski = pure;
            }
            native_emit_elf(&e, &elf, &elf_size, ski, heap_size);
            
            if (elf) {
                /* Write binary to stdout */
                fwrite(elf, 1, elf_size, stdout);
                if (verbose) {
                    fprintf(stderr, "Emitted ELF (%llu bytes)\n", (unsigned long long)elf_size);
                }
                free(elf);
            } else {
                fprintf(stderr, "ELF emission failed\n");
                native_emit_drop(&e); free(source);
                return 1;
            }
            
            native_emit_drop(&e);
            free(source);
            return 0;
        }
        
        /* machine words have no pure S K spelling: they need the extended format or the ELF */
        if (mode != EMIT_XBCL && ski_uses_words(ski)) {
            fprintf(stderr, "Error: the program uses machine words; emit it with -f xbcl or -e\n");
            free(source);
            return 1;
        }
        
        /* Emit into the growable bit buffer (libeezo/bcl.h), then print it as ASCII */
        BclBuffer bb;
        bcl_buffer_init(&bb);
        bool ok = false;
        const char *fname = "BCL";
        switch (mode) {
            case EMIT_BCL: ok = bcl_emit(ski, &bb); fname = "BCL"; break;
            case EMIT_XBCL: ok = xbcl_emit(ski, &bb); fname = "XBCL"; break;
            case EMIT_JOT: ok = jot_emit(ski, &bb); fname = "Jot"; break;
            case EMIT_JOMPLEMENT: ok = jomplement_emit(ski, &bb); fname = "Jomplement"; break;
        }
        u64 bits = bcl_buffer_len(&bb);
        if (verbose) fprintf(stderr, "%s (%lld bits): ", fname, ok ? (long long)bits : -1LL);
        
        if (ok && bits > 0) {
            const u8 *buf = bcl_buffer_data(&bb);
            for (u64 b = 0; b < bits; b++) putchar('0' + ((buf[b / 8] >> (7 - b % 8)) & 1));
            printf("\n");
        } else {
            fprintf(stderr, "Emission failed\n");
            bcl_buffer_drop(&bb); free(source);
            return 1;
        }
        
        bcl_buffer_drop(&bb);
    } else {
        fprintf(stderr, "Compilation failed\n");
        free(source);
        return 1;
    }
    
    free(source);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] < input.eezo\n", prog);
    fprintf(stderr, "\nEezo compiler - compiles .eezo source to bytecode\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -v            Verbose (show SKI, compilation order)\n");
    fprintf(stderr, "  -f FORMAT     Output encoding: bcl (default), jot, jomplement, xbcl\n");
    fprintf(stderr, "                (xbcl carries machine words: a literal 5w and the word primitives wadd wsub wmul wand\n");
    fprintf(stderr, "                 wor wxor wshl wshr weq wlt waddc wsubb wmull wdivmod; the pure formats refuse them)\n");
    fprintf(stderr, "  -e            Emit standalone ELF executable instead of bytecode\n");
    fprintf(stderr, "  -N MODE       (with -e) normalization: nf (default) or whnf\n");
    fprintf(stderr, "  -H BYTES      (with -e) initial semispace size, default 16MiB; grows on demand\n");
    fprintf(stderr, "  -i            (with -e) stream I/O mode: the executable maps stdin to stdout\n");
    fprintf(stderr, "  -m            (with -e) monadic I/O mode: the program is run(m) of the stdlib's io.eezo;\n");
    fprintf(stderr, "                the executable performs its actions (putc, getc, exit)\n");
    fprintf(stderr, "  -h            Show this help\n");
    fprintf(stderr, "\nInput is read from stdin. Output bytecode written to stdout.\n");
    fprintf(stderr, "Use 'eezo' to evaluate the compiled output.\n");
}

int main(int argc, char **argv) {
    mem_init("eezoc", "EEZOC_MAX_ALLOC");   /* the one memory layer (libeezo/mem.h): its failure path and budget */
    SKIPool tp;
    ski_pool_init(&tp);
    
    AstPool ap;
    ast_pool_init(&ap);
    
    int verbose = 0;
    EmitMode mode = EMIT_BCL;
    int emit_elf = 0;
    int nf_mode = 1;
    int io_mode = 0;
    u64 heap_size = NATIVE_DEFAULT_HEAP_SIZE;
    
    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            emit_elf = 1;
        } else if (strcmp(argv[i], "-H") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for -H\n");
                return 1;
            }
            heap_size = (u64)strtoull(argv[i], NULL, 0);
            if (heap_size < 4096) {
                fprintf(stderr, "Heap size too small: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-i") == 0) {
            io_mode = 1;
        } else if (strcmp(argv[i], "-m") == 0) {
            io_mode = 2;
        } else if (strcmp(argv[i], "-N") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for -N\n");
                return 1;
            }
            if (strcmp(argv[i], "nf") == 0) {
                nf_mode = 1;
            } else if (strcmp(argv[i], "whnf") == 0) {
                nf_mode = 0;
            } else {
                fprintf(stderr, "Unknown normalization mode: %s (nf|whnf)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -f\n");
                usage(argv[0]);
                return 1;
            }
            i++;
            if (strcmp(argv[i], "bcl") == 0) {
                mode = EMIT_BCL;
            } else if (strcmp(argv[i], "jot") == 0) {
                mode = EMIT_JOT;
            } else if (strcmp(argv[i], "jomplement") == 0) {
                mode = EMIT_JOMPLEMENT;
            } else if (strcmp(argv[i], "xbcl") == 0) {
                mode = EMIT_XBCL;
            } else {
                fprintf(stderr, "Unknown format: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    
    /* Compile from stdin */
    char *stdin_path = "-";
    int rc = compile_files(1, &stdin_path, &ap, &tp, verbose, mode, emit_elf, nf_mode, heap_size, io_mode);
    
    ast_pool_free(&ap);
    ski_pool_drop(&tp);
    return rc;
}
