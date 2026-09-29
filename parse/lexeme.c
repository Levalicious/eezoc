#include <stdbool.h>
#include <string.h>
#include <libeezo/mem.h>
#include "util.h"   // fputll
#include "lexeme.h"

const Lexeme EMPTY = {.location={0}, .length=0, .start=""};
/* the source files' names, by the index a Location carries (index 0: none): a Stack of the memory layer */
static Stack FILENAMES = { NULL, 0, 0, sizeof(const char *) };
unsigned int FILE_COUNT = 0;

/* File boundary mapping for concatenated sources: one per file, however many */
typedef struct {
    const char *filename;
    unsigned int start_line;  /* First line of this file (1-based, in concat) */
    unsigned int import_count; /* Number of #import lines stripped */
} FileBoundary;
static FileBoundary *file_boundaries = NULL;
static int num_boundaries = 0;
static void boundaries_room(int n) { file_boundaries = rrealloc(file_boundaries, (size_t)(n + 1) * sizeof(FileBoundary)); }

void set_file_boundaries(int n, const char **filenames, const unsigned int *start_lines) {
    boundaries_room(n); num_boundaries = n;
    for (int i = 0; i < num_boundaries; i++) {
        file_boundaries[i].filename = filenames[i];
        file_boundaries[i].start_line = start_lines[i];
        file_boundaries[i].import_count = 0;
    }
}

void set_file_boundaries_ex(int n, const char **filenames, const unsigned int *start_lines,
                            const unsigned int *import_counts) {
    boundaries_room(n); num_boundaries = n;
    for (int i = 0; i < num_boundaries; i++) {
        file_boundaries[i].filename = filenames[i];
        file_boundaries[i].start_line = start_lines[i];
        file_boundaries[i].import_count = import_counts[i];
    }
}

/* Map global line to (filename, local_line). Returns NULL if no mapping. */
static const char *map_line_to_file(unsigned int global_line, unsigned int *local_line) {
    if (num_boundaries == 0) return NULL;
    
    /* Find which file contains this line */
    for (int i = num_boundaries - 1; i >= 0; i--) {
        if (global_line >= file_boundaries[i].start_line) {
            /* local_line = position in concat file + skipped imports */
            *local_line = (global_line - file_boundaries[i].start_line + 1) 
                        + file_boundaries[i].import_count;
            return file_boundaries[i].filename;
        }
    }
    return NULL;
}

Lexeme newLexeme(const char* start, unsigned int length, Location location) {
    return (Lexeme){.location=location, .length=length, .start=start};
}

Lexeme newLiteralLexeme(const char* start, Location location) {
    return newLexeme(start, (unsigned int)strlen(start), location);
}

/* a file's index for its Locations (a Location's 32-bit field; past it a file gets 0, no name) */
unsigned int newFilename(const char* filename) {
    if (FILE_COUNT == 0xFFFFFFFFu) return 0;
    if (FILENAMES.n == 0) STACK_PUSH(&FILENAMES, const char *, NULL);   /* index 0: none */
    STACK_PUSH(&FILENAMES, const char *, filename);
    return ++FILE_COUNT;
}

Location newLocation(unsigned int file,
        unsigned int line, unsigned int column) {
    return (Location){.file=file, .line=line, .column=column};
}

bool isThisLexeme(Lexeme a, const char* b) {
    // strncmp(NULL, NULL, 0) is undefined behavior, so we check for 0 length
    return a.length == strlen(b) &&
        (a.length == 0 || strncmp(a.start, b, a.length) == 0);
}

bool isSameLexeme(Lexeme a, Lexeme b) {
    // strncmp(NULL, NULL, 0) is undefined behavior, so we check for 0 length
    return a.length == b.length &&
        (a.length == 0 || strncmp(a.start, b.start, a.length) == 0);
}

static void printLine(const char* line, FILE* stream) {
    size_t length = 0;
    for (; line[length] != '\0' && line[length] != '\n'; ++length);
    fwrite(line, sizeof(char), length, stream);
}

void printLocation(Location location, FILE* stream) {
    unsigned int line = location.line;
    const char *filename = NULL;
    
    if (location.file != 0) {
        filename = STACK_AT(&FILENAMES, const char *, location.file);
    } else {
        /* Try to map via file boundaries */
        unsigned int local_line;
        filename = map_line_to_file(location.line, &local_line);
        if (filename) line = local_line;
    }
    
    if (filename) {
        printLine(filename, stream);
        fputs(" ", stream);
    }
    fputs("line ", stream);
    fputll((long long)line, stream);
    fputs(" column ", stream);
    fputll((long long)location.column, stream);
}
