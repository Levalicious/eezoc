/*
 * lexeme.h - Source text slice with location
 *
 * A Lexeme is a view into source text with location tracking.
 * Used by both the lexer (producing tokens) and parser (for error messages).
 */
#ifndef LEXEME_H
#define LEXEME_H

#include <stdbool.h>
#include <stdio.h>
#include <libeezo/types.h>

#define MAX_LEXEME_LENGTH 0xff
#define MAX_LINE 0xffff
#define MAX_COLUMN 0xffff
#define MAX_FILES 256

/* Source location: file + line + column */
typedef struct {
    u16 file;       /* Index into filename table */
    u16 line;       /* 1-based line number */
    u16 column;     /* 1-based column */
} Location;

/* Lexeme: slice of source text with location */
typedef struct {
    const char *start;  /* Pointer into source */
    u16 length;         /* Length in bytes */
    Location loc;       /* Where in source */
} Lexeme;

/* Empty lexeme sentinel */
extern const Lexeme EMPTY_LEXEME;

/* Create lexeme from pointer and length */
Lexeme lexeme_new(const char *start, u16 length, Location loc);

/* Create lexeme from null-terminated literal (for built-in operators) */
Lexeme lexeme_literal(const char *str, Location loc);

/* Register a filename, returns index (0 = unknown) */
u16 lexeme_add_file(const char *filename);

/* Get filename by index */
const char *lexeme_get_file(u16 index);

/* Create location */
Location location_new(u16 file, u16 line, u16 column);

/* Compare lexemes by content (not location) */
bool lexeme_eq(Lexeme a, Lexeme b);

/* Compare lexeme to null-terminated string */
bool lexeme_eq_str(Lexeme a, const char *b);

/* Print location for error messages */
void location_print(Location loc, FILE *out);

/* Print lexeme content */
void lexeme_print(Lexeme lex, FILE *out);

#endif /* LEXEME_H */
