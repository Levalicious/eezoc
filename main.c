/*
 * main.c - Eezo compiler test driver
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "lib/term.h"
#include "lib/bcl.h"
#include "lib/jomplement.h"
#include "lib/jit.h"
#include "ast.h"
#include "bracket.h"
#include "compile.h"
#include "parse/tree.h"
#include "parse/array.h"
#include "parse/term.h"
#include "parse/parse.h"
#include "parse/opp/operator.h"

#define STDLIB_PATH "/home/lev/.local/share/eezo/stdlib"

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
        
        char *buf = malloc(size + 1);
        if (!buf) {
            fclose(f);
            return NULL;
        }
        
        size_t nread = fread(buf, 1, size, f);
        buf[nread] = '\0';
        fclose(f);
        return buf;
    }
    
    /* Non-seekable (pipe, stdin): read in chunks */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        if (!is_stdin) fclose(f);
        return NULL;
    }
    
    size_t nread;
    while ((nread = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += nread;
        if (len + 1 >= cap) {
            cap *= 2;
            char *newbuf = realloc(buf, cap);
            if (!newbuf) {
                free(buf);
                if (!is_stdin) fclose(f);
                return NULL;
            }
            buf = newbuf;
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
        return stdin_cache ? strdup(stdin_cache) : NULL;
    }
    return read_file_contents(path);
}

/*
 * Import graph and toposort
 */

#define MAX_FILES 256
#define MAX_IMPORTS 32

typedef struct {
    char *path;
    char *imports[MAX_IMPORTS];
    int nimports;
    int visited;  /* 0=unvisited, 1=visiting, 2=done */
} FileNode;

static FileNode files[MAX_FILES];
static int nfiles_graph = 0;
static char *sorted_files[MAX_FILES];
static int nsorted = 0;

static int find_or_add_file(const char *path) {
    for (int i = 0; i < nfiles_graph; i++) {
        if (strcmp(files[i].path, path) == 0) return i;
    }
    if (nfiles_graph >= MAX_FILES) {
        fprintf(stderr, "Too many files\n");
        return -1;
    }
    int idx = nfiles_graph++;
    files[idx].path = strdup(path);
    files[idx].nimports = 0;
    files[idx].visited = 0;
    return idx;
}

/* Extract imports from file content - lines starting with "import " */
static void scan_imports(int idx) {
    char *content = read_file_or_cache(files[idx].path);
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
            
            if (line > start && files[idx].nimports < MAX_IMPORTS) {
                size_t len = line - start;
                char *imp = malloc(len + 1);
                memcpy(imp, start, len);
                imp[len] = '\0';
                files[idx].imports[files[idx].nimports++] = imp;
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
    char *dir = strdup(base_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = '\0';
    } else {
        free(dir);
        dir = strdup("./");
    }
    
    /* Try 1: relative to importing file */
    size_t len = strlen(dir) + strlen(import_name) + 6;
    char *path = malloc(len + 1);
    snprintf(path, len + 1, "%s%s.eezo", dir, import_name);
    
    if (access(path, F_OK) == 0) {
        free(dir);
        return path;
    }
    free(path);
    
    /* Try 2: in stdlib/ relative to importing file */
    len = strlen(dir) + 7 + strlen(import_name) + 6;
    path = malloc(len + 1);
    snprintf(path, len + 1, "%sstdlib/%s.eezo", dir, import_name);
    
    if (access(path, F_OK) == 0) {
        free(dir);
        return path;
    }
    free(path);
    
    /* Try 3: in absolute STDLIB_PATH */
    len = strlen(STDLIB_PATH) + 1 + strlen(import_name) + 6;
    path = malloc(len + 1);
    snprintf(path, len + 1, "%s/%s.eezo", STDLIB_PATH, import_name);

    free(dir);
    return path;  /* Return this even if not found - will fail later with clear error */
}

/* Toposort via DFS - returns 0 on success, -1 on cycle */
static int toposort_visit(int idx, const char *base_path) {
    if (files[idx].visited == 2) return 0;  /* Already done */
    if (files[idx].visited == 1) {
        fprintf(stderr, "Circular import: %s\n", files[idx].path);
        return -1;
    }
    
    files[idx].visited = 1;  /* Visiting */
    
    /* Visit dependencies first */
    for (int i = 0; i < files[idx].nimports; i++) {
        char *dep_path = resolve_import(files[idx].path, files[idx].imports[i]);
        int dep_idx = find_or_add_file(dep_path);
        if (dep_idx < 0) {
            free(dep_path);
            return -1;
        }
        if (files[dep_idx].nimports == 0) {
            scan_imports(dep_idx);
        }
        if (toposort_visit(dep_idx, files[idx].path) < 0) {
            free(dep_path);
            return -1;
        }
        free(dep_path);
    }
    
    files[idx].visited = 2;  /* Done */
    sorted_files[nsorted++] = files[idx].path;
    return 0;
}

/* Build import graph and return toposorted file list */
static char **resolve_imports(int nfiles, char **paths, int *out_count) {
    nfiles_graph = 0;
    nsorted = 0;
    
    /* Add all input files */
    for (int i = 0; i < nfiles; i++) {
        int idx = find_or_add_file(paths[i]);
        if (idx < 0) return NULL;
        scan_imports(idx);
    }
    
    /* Toposort from each input file */
    for (int i = 0; i < nfiles; i++) {
        int idx = find_or_add_file(paths[i]);
        if (toposort_visit(idx, paths[i]) < 0) return NULL;
    }
    
    *out_count = nsorted;
    return sorted_files;
}

/* Free import graph */
static void free_import_graph(void) {
    for (int i = 0; i < nfiles_graph; i++) {
        free(files[i].path);
        for (int j = 0; j < files[i].nimports; j++) {
            free(files[i].imports[j]);
        }
    }
    nfiles_graph = 0;
    nsorted = 0;
}

/* Concatenate files, stripping import lines */
static char *concat_files_strip_imports(int nfiles, char **paths) {
    /* Read all files first (handles stdin/pipes) */
    char **contents = malloc(nfiles * sizeof(char*));
    if (!contents) return NULL;
    
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
    
    char *buf = malloc(total + 1);
    if (!buf) {
        for (int i = 0; i < nfiles; i++) free(contents[i]);
        free(contents);
        return NULL;
    }
    
    /* Track file boundaries for error reporting */
    static const char *boundary_files[256];
    static unsigned short boundary_starts[256];
    static unsigned short boundary_import_counts[256];
    int num_bounds = 0;
    unsigned short current_line = 1;
    
    /* Process files, skip import lines */
    char *p = buf;
    for (int i = 0; i < nfiles; i++) {
        /* Record boundary start */
        int bound_idx = -1;
        if (num_bounds < 256) {
            bound_idx = num_bounds;
            boundary_files[num_bounds] = strdup(paths[i]);
            boundary_starts[num_bounds] = current_line;
            boundary_import_counts[num_bounds] = 0;
            num_bounds++;
        }
        
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
                if (bound_idx >= 0) boundary_import_counts[bound_idx]++;
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
    
    return buf;
}

/* Emit mode */
typedef enum {
    EMIT_BCL,
    EMIT_JOT,
    EMIT_JOMPLEMENT,
} EmitMode;

/* Compile files with import resolution */
static void compile_files(int nfiles, char **files, AstPool *ap, SKIPool *tp, 
                          int verbose, EmitMode mode) {
    /* Resolve imports and toposort */
    int sorted_count;
    char **sorted = resolve_imports(nfiles, files, &sorted_count);
    if (!sorted) {
        fprintf(stderr, "Failed to resolve imports\n");
        return;
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
        return;
    }
    
    /* Compile */
    SKITerm *ski = compile_to_ski(ap, tp, source);
    if (ski) {
        if (verbose) {
            fprintf(stderr, "SKI: ");
            ski_fprint(stderr, ski);
            fprintf(stderr, "\n");
        }
        
        /* Allocate buffer for emission */
        u64 size_bits;
        if (mode == EMIT_BCL) {
            size_bits = bcl_size(ski);
        } else {
            size_bits = jot_size(ski);
        }
        u32 buf_size = (size_bits + 7) / 8 + 8;  /* +8 for safety */
        u8 *buf = malloc(buf_size);
        if (!buf) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            return;
        }
        memset(buf, 0, buf_size);
        
        i32 bits = -1;
        
        switch (mode) {
            case EMIT_BCL: {
                BclBuffer bb;
                bcl_buffer_init(&bb, buf, buf_size * 8);
                if (bcl_emit(ski, &bb)) {
                    bits = (i32)bcl_buffer_len(&bb);
                }
                if (verbose) fprintf(stderr, "BCL (%d bits): ", bits);
                break;
            }
            case EMIT_JOT:
                bits = jot_emit(ski, buf, buf_size);
                if (verbose) fprintf(stderr, "Jot (%d bits): ", bits);
                break;
            case EMIT_JOMPLEMENT:
                bits = jomplement_emit(ski, buf, buf_size);
                if (verbose) fprintf(stderr, "Jomplement (%d bits): ", bits);
                break;
        }
        
        if (bits > 0) {
            for (int b = 0; b < bits; b++) {
                int byte_idx = b / 8;
                int bit_idx = 7 - (b % 8);
                printf("%d", (buf[byte_idx] >> bit_idx) & 1);
            }
            printf("\n");
        } else {
            fprintf(stderr, "Emission failed\n");
        }
        
        free(buf);
    } else {
        fprintf(stderr, "Compilation failed\n");
    }
    
    free(source);
}

/* Execute a pre-compiled BCL/Jot/Jomplement file (ASCII '0'/'1' format) */
static void execute_file(const char *path, SKIPool *tp, EmitMode format, int verbose, int use_jit, int use_native, int emit_elf) {
    FILE *f;
    int is_stdin = (strcmp(path, "-") == 0);
    
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "Cannot open %s\n", path);
            return;
        }
    }
    
    /* Read ASCII bits, ignoring whitespace and newlines */
    u8 *bits = malloc(65536);  /* Max ~64K bits */
    u64 nbits = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '0' || c == '1') {
            if (nbits >= 65536 * 8) {
                fprintf(stderr, "File too large: %s\n", path);
                free(bits);
                if (!is_stdin) fclose(f);
                return;
            }
            /* Pack into bytes, MSB first */
            u64 byte_idx = nbits / 8;
            int bit_idx = 7 - (nbits % 8);
            if (bit_idx == 7) bits[byte_idx] = 0;  /* Clear byte on first bit */
            if (c == '1') bits[byte_idx] |= (1 << bit_idx);
            nbits++;
        }
        /* Ignore whitespace, newlines, etc. */
    }
    if (!is_stdin) fclose(f);
    
    if (nbits == 0) {
        fprintf(stderr, "No bits in file: %s\n", path);
        free(bits);
        return;
    }
    
    SKITerm *term = NULL;
    
    if (format == EMIT_BCL) {
        BclStream s;
        bcl_stream_init(&s, bits, nbits);
        term = bcl_parse(tp, &s);
        if (verbose) {
            fprintf(stderr, "Parsed BCL (%llu bits)\n", (unsigned long long)bcl_stream_pos(&s));
        }
    } else if (format == EMIT_JOT) {
        BclStream s;
        bcl_stream_init(&s, bits, nbits);
        term = jot_parse(tp, &s);
        if (verbose) {
            fprintf(stderr, "Parsed Jot (%llu bits)\n", (unsigned long long)nbits);
        }
    } else if (format == EMIT_JOMPLEMENT) {
        BclStream s;
        bcl_stream_init(&s, bits, nbits);
        term = jomplement_parse(tp, &s);
        if (verbose) {
            fprintf(stderr, "Parsed Jomplement (%llu bits)\n", (unsigned long long)nbits);
        }
    }
    
    free(bits);
    
    if (!term) {
        fprintf(stderr, "Parse failed: %s\n", path);
        return;
    }
    
    if (verbose) {
        fprintf(stderr, "Parsed: ");
        ski_fprint(stderr, term);
        fprintf(stderr, "\n");
    }
    
    /* Execute: STG machine, native code, or standard interpreter */
    i64 steps;
    if (emit_elf) {
        if (verbose) {
            fprintf(stderr, "Emitting ELF executable...\n");
        }
        int rc = jit_emit_elf(STDOUT_FILENO, term);
        if (rc != 0) {
            fprintf(stderr, "ELF emission failed\n");
        } else if (verbose) {
            fprintf(stderr, "ELF written to stdout\n");
        }
        ski_unref(tp, term);
        return;
    } else if (use_jit && use_native) {
        if (verbose) {
            fprintf(stderr, "Using native x86-64 STG...\n");
        }
        SKITerm *result = jit_reduce_native(tp, term, &steps);
        ski_unref(tp, term);
        term = result;
        if (verbose) {
            fprintf(stderr, "Reduced (%lld steps)\n", (long long)steps);
        }
    } else if (use_jit) {
        if (verbose) {
            fprintf(stderr, "Using STG machine...\n");
        }
        SKITerm *result = jit_reduce(tp, term, &steps);
        ski_unref(tp, term);
        term = result;
        if (verbose) {
            fprintf(stderr, "Reduced (%lld steps)\n", (long long)steps);
        }
    } else {
        /* Standard interpreter */
        steps = ski_reduce(tp, &term, 0);  /* 0 = unlimited */
        if (verbose) {
            fprintf(stderr, "Reduced (%lld steps)\n", (long long)steps);
        }
    }
    
    if (verbose) {
        fprintf(stderr, "Result: ");
        ski_fprint(stderr, term);
        fprintf(stderr, "\n");
    }
    
    /* Emit result in same format */
    u64 size_bits;
    if (format == EMIT_BCL) {
        size_bits = bcl_size(term);
    } else {
        size_bits = jot_size(term);
    }
    u32 buf_size = (size_bits + 7) / 8 + 8;
    u8 *buf = malloc(buf_size);
    if (!buf) {
        fprintf(stderr, "Out of memory\n");
        ski_unref(tp, term);
        return;
    }
    memset(buf, 0, buf_size);
    
    i32 out_bits = -1;
    switch (format) {
        case EMIT_BCL: {
            BclBuffer bb;
            bcl_buffer_init(&bb, buf, buf_size * 8);
            if (bcl_emit(term, &bb)) {
                out_bits = (i32)bcl_buffer_len(&bb);
            }
            break;
        }
        case EMIT_JOT:
            out_bits = jot_emit(term, buf, buf_size);
            break;
        case EMIT_JOMPLEMENT:
            out_bits = jomplement_emit(term, buf, buf_size);
            break;
    }
    
    if (out_bits > 0) {
        for (int b = 0; b < out_bits; b++) {
            int byte_idx = b / 8;
            int bit_idx = 7 - (b % 8);
            printf("%d", (buf[byte_idx] >> bit_idx) & 1);
        }
        printf("\n");
    } else {
        fprintf(stderr, "Emission failed\n");
    }
    
    free(buf);
    ski_unref(tp, term);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] < input.eezo\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v            Verbose (show SKI, compilation order)\n");
    fprintf(stderr, "  -f FORMAT     Output format: bcl (default), jot, jomplement\n");
    fprintf(stderr, "  -x FORMAT     Execute pre-compiled input (bcl, jot, jomplement)\n");
    fprintf(stderr, "  -j            JIT compile (with -x)\n");
    fprintf(stderr, "  -n            Use native x86-64 backend (with -x -j)\n");
    fprintf(stderr, "  -e            Emit standalone ELF executable to stdout (with -x bcl)\n");
    fprintf(stderr, "\nInput is always read from stdin.\n");
}

int main(int argc, char **argv) {
    SKIPool tp;
    pool_init(&tp, 1000000);
    
    AstPool ap;
    ast_pool_init(&ap, 1000000);
    
    int verbose = 0;
    int use_jit = 0;
    int use_native = 0;
    int emit_elf = 0;
    EmitMode mode = EMIT_BCL;
    EmitMode exec_mode = -1;  /* -1 = not executing */
    
    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-x") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -x\n");
                usage(argv[0]);
                return 1;
            }
            i++;
            if (strcmp(argv[i], "bcl") == 0) {
                exec_mode = EMIT_BCL;
            } else if (strcmp(argv[i], "jot") == 0) {
                exec_mode = EMIT_JOT;
            } else if (strcmp(argv[i], "jomplement") == 0) {
                exec_mode = EMIT_JOMPLEMENT;
            } else {
                fprintf(stderr, "Unknown format: %s\n", argv[i]);
                usage(argv[0]);
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
            } else {
                fprintf(stderr, "Unknown format: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-j") == 0) {
            use_jit = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            use_native = 1;
            use_jit = 1;  /* -n implies -j */
        } else if (strcmp(argv[i], "-e") == 0) {
            emit_elf = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    
    if (exec_mode != (EmitMode)-1) {
        /* Execute pre-compiled input from stdin */
        execute_file("-", &tp, exec_mode, verbose, use_jit, use_native, emit_elf);
    } else {
        /* Compile from stdin */
        char *stdin_path = "-";
        compile_files(1, &stdin_path, &ap, &tp, verbose, mode);
    }
    
    ast_pool_free(&ap);
    pool_free(&tp);
    return 0;
}
