/*
 * lexeme.c - Lexeme implementation
 */
#include "lexeme.h"
#include <string.h>
#include <stdlib.h>

const Lexeme EMPTY_LEXEME = { NULL, 0, {0, 0, 0} };

/* Filename table */
static const char *filenames[MAX_FILES];
static u16 filename_count = 0;

Lexeme lexeme_new(const char *start, u16 length, Location loc) {
    Lexeme l = { start, length, loc };
    return l;
}

Lexeme lexeme_literal(const char *str, Location loc) {
    return lexeme_new(str, (u16)strlen(str), loc);
}

u16 lexeme_add_file(const char *filename) {
    if (filename_count >= MAX_FILES) return 0;
    
    /* Check if already registered */
    for (u16 i = 1; i <= filename_count; i++) {
        if (strcmp(filenames[i], filename) == 0) return i;
    }
    
    /* Add new filename */
    filename_count++;
    filenames[filename_count] = filename;  /* Assumes filename is stable */
    return filename_count;
}

const char *lexeme_get_file(u16 index) {
    if (index == 0 || index > filename_count) return "<unknown>";
    return filenames[index];
}

Location location_new(u16 file, u16 line, u16 column) {
    Location l = { file, line, column };
    return l;
}

bool lexeme_eq(Lexeme a, Lexeme b) {
    if (a.length != b.length) return false;
    if (a.start == b.start) return true;
    if (a.length == 0) return true;
    return memcmp(a.start, b.start, a.length) == 0;
}

bool lexeme_eq_str(Lexeme a, const char *b) {
    size_t blen = strlen(b);
    if (a.length != blen) return false;
    if (a.length == 0) return true;
    return memcmp(a.start, b, a.length) == 0;
}

void location_print(Location loc, FILE *out) {
    if (loc.file == 0) {
        fprintf(out, "line %u, column %u", loc.line, loc.column);
    } else {
        fprintf(out, "%s:%u:%u", lexeme_get_file(loc.file), loc.line, loc.column);
    }
}

void lexeme_print(Lexeme lex, FILE *out) {
    if (lex.start && lex.length > 0) {
        fwrite(lex.start, 1, lex.length, out);
    }
}
