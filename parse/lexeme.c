#include <stdbool.h>
#include <string.h>
#include "util.h"   // fputll
#include "lexeme.h"

const Lexeme EMPTY = {.location={0}, .length=0, .start=""};
const char* FILENAMES[2048] = {0};
const unsigned int MAX_FILENAMES = sizeof(FILENAMES) / sizeof(const char*);
unsigned short FILE_COUNT = 0;

/* File boundary mapping for concatenated sources */
#define MAX_FILE_BOUNDARIES 256
static struct {
    const char *filename;
    unsigned short start_line;  /* First line of this file (1-based, in concat) */
    unsigned short import_count; /* Number of #import lines stripped */
} file_boundaries[MAX_FILE_BOUNDARIES];
static int num_boundaries = 0;

void set_file_boundaries(int n, const char **filenames, const unsigned short *start_lines) {
    num_boundaries = (n > MAX_FILE_BOUNDARIES) ? MAX_FILE_BOUNDARIES : n;
    for (int i = 0; i < num_boundaries; i++) {
        file_boundaries[i].filename = filenames[i];
        file_boundaries[i].start_line = start_lines[i];
        file_boundaries[i].import_count = 0;
    }
}

void set_file_boundaries_ex(int n, const char **filenames, const unsigned short *start_lines,
                            const unsigned short *import_counts) {
    num_boundaries = (n > MAX_FILE_BOUNDARIES) ? MAX_FILE_BOUNDARIES : n;
    for (int i = 0; i < num_boundaries; i++) {
        file_boundaries[i].filename = filenames[i];
        file_boundaries[i].start_line = start_lines[i];
        file_boundaries[i].import_count = import_counts[i];
    }
}

/* Map global line to (filename, local_line). Returns NULL if no mapping. */
static const char *map_line_to_file(unsigned short global_line, unsigned short *local_line) {
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

Lexeme newLexeme(const char* start, unsigned short length, Location location) {
    return (Lexeme){.location=location, .length=length, .start=start};
}

Lexeme newLiteralLexeme(const char* start, Location location) {
    return newLexeme(start, (unsigned short)strlen(start), location);
}

unsigned short newFilename(const char* filename) {
    if (FILE_COUNT >= MAX_FILENAMES - 1)
        return 0;
    FILENAMES[++FILE_COUNT] = filename;
    return FILE_COUNT;
}

Location newLocation(unsigned short file,
        unsigned short line, unsigned short column) {
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
    unsigned short line = location.line;
    const char *filename = NULL;
    
    if (location.file != 0) {
        filename = FILENAMES[location.file];
    } else {
        /* Try to map via file boundaries */
        unsigned short local_line;
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
