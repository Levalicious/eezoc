#include <stdio.h>

/* A lexeme's length and a location's fields are 32 bits wide (they were 16, and a lexeme was capped at 255 bytes:
   a longer identifier or string literal was an error, and so was a program past 65535 lines) */
static const unsigned int MAX_LEXEME_LENGTH = 0xffffffffu;
static const unsigned int MAX_LINE = 0xffffffffu;
static const unsigned int MAX_COLUMN = 0xffffffffu;

typedef struct {
    unsigned int file, line, column;
} Location;

typedef struct {
    Location location;
    unsigned int length;
    const char* start;
} Lexeme;

extern const Lexeme EMPTY;

Lexeme newLexeme(const char* start, unsigned int length, Location location);
Lexeme newLiteralLexeme(const char* start, Location location);
unsigned int newFilename(const char* filename);
Location newLocation(unsigned int file,
    unsigned int line, unsigned int column);
bool isThisLexeme(Lexeme a, const char* b);
bool isSameLexeme(Lexeme a, Lexeme b);
void printLocation(Location location, FILE* stream);
void set_file_boundaries(int n, const char **filenames, const unsigned int *start_lines);
void set_file_boundaries_ex(int n, const char **filenames, const unsigned int *start_lines,
                            const unsigned int *import_counts);
