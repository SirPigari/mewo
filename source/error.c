/*
 * error.c - Error handling system for Mewo
 * 
 * Features:
 *   - Global error state with type, message, and line number
 *   - Error types: SYNTAX, RUNTIME, MEMORY
 *   - Formatted error output with file:line:type:message format
 *   - Utility str_dup() function used throughout the codebase
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    ERROR_NONE = 0,
    ERROR_SYNTAX,
    ERROR_RUNTIME,
    ERROR_MEMORY,
} ErrorType;

typedef struct {
    ErrorType type;
    char* message;
    size_t line_number;
} Error;

static Error g_error = {0};

static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

void set_error(ErrorType type, const char* message, size_t line_number) {
    free(g_error.message);  // Free previous error message to avoid leak
    g_error.type = type;
    g_error.message = str_dup(message);
    g_error.line_number = line_number;
}

bool has_error() {
    return g_error.type != ERROR_NONE;
}

void print_error(const char* file, FILE* stream) {
    if (g_error.type == ERROR_NONE) return;

    const char* type_str = "Unknown";
    switch (g_error.type) {
        case ERROR_SYNTAX: type_str = "Syntax Error"; break;
        case ERROR_RUNTIME: type_str = "Runtime Error"; break;
        case ERROR_MEMORY: type_str = "Memory Error"; break;
        default: break;
    }

    fprintf(stream, "%s:%zu: %s: %s\n", file, g_error.line_number, type_str, g_error.message);
}

void clear_error(void) {
    free(g_error.message);
    g_error.type = ERROR_NONE;
    g_error.message = NULL;
    g_error.line_number = 0;
}