/*
 * main.c - Entry point for Mewo build system interpreter
 * 
 * Features:
 *   - Command-line argument parsing with flag.h
 *   - Mewofile loading and execution
 *   - Label invocation with optional arguments
 *   - Feature enable/disable flags (+F/-F)
 *   - Variable override flags (-D)
 *   - Dry-run mode for testing
 * 
 * Usage: mewo [LABEL] [OPTIONS] [-- ARGS]
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define FLAG_PUSH_DASH_DASH_BACK
#define FLAG_IMPLEMENTATION
#include "../thirdparty/flag.h"
#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#define NOBDEF static inline
#include "../thirdparty/nob.h"

#include "error.c"
#include "vars.c"
#include "parser.c"
#include "exec.c"

static const int VERSION = 0x0100;

#define BUILD_TIME __DATE__ " " __TIME__

static void usage(FILE* stream) {
    fprintf(stream, "Usage: mewo [LABEL] [OPTIONS]\n");
    fprintf(stream, "OPTIONS:\n");
    flag_print_options(stream);
}

static bool read_lines_entire_file(const char* path, char*** out_lines, size_t* out_count) {
    if (!out_lines || !out_count) return false;
    *out_lines = NULL;
    *out_count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        nob_log(NOB_ERROR, "Could not open file %s: %s", path, strerror(errno));
        return false;
    }

    size_t capacity = 256;
    size_t count = 0;
    char** lines = malloc(capacity * sizeof(char*));
    if (!lines) {
        fclose(f);
        nob_log(NOB_ERROR, "Out of memory allocating lines array for file %s", path);
        return false;
    }

    char* line = NULL;
    size_t len = 0;
    size_t read;

#ifdef _WIN32
    #define GETLINE_BUFFER 1024
    char temp_buf[GETLINE_BUFFER];
    while (fgets(temp_buf, GETLINE_BUFFER, f)) {
        size_t line_len = strlen(temp_buf);

        if (line_len > 0 && temp_buf[line_len - 1] == '\r') temp_buf[line_len - 1] = '\0';
        line_len = strlen(temp_buf);

        line = malloc(line_len + 1);
        if (!line) {
            for (size_t i = 0; i < count; i++) free(lines[i]);
            free(lines);
            fclose(f);
            nob_log(NOB_ERROR, "Out of memory reading line from file %s", path);
            return false;
        }
        memcpy(line, temp_buf, line_len + 1);

        if (count >= capacity) {
            capacity *= 2;
            char** tmp = realloc(lines, capacity * sizeof(char*));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) free(lines[i]);
                free(line);
                free(lines);
                fclose(f);
                nob_log(NOB_ERROR, "Out of memory expanding lines array for file %s", path);
                return false;
            }
            lines = tmp;
        }

        lines[count++] = line;
    }
#else
    while ((read = getline(&line, &len, f)) != -1) {
        if (read > 0 && line[read - 1] == '\n') line[read - 1] = '\0';
        if (read > 1 && line[read - 2] == '\r') line[read - 2] = '\0';

        if (count >= capacity) {
            capacity *= 2;
            char** tmp = realloc(lines, capacity * sizeof(char*));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) free(lines[i]);
                free(line);
                free(lines);
                fclose(f);
                nob_log(NOB_ERROR, "Out of memory expanding lines array for file %s", path);
                return false;
            }
            lines = tmp;
        }
        lines[count++] = line;
        line = NULL;
        len = 0;
    }
    free(line);
#endif

    fclose(f);
    *out_lines = lines;
    *out_count = count;
    return true;
}

static void free_lines(char** lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

static Nob_Log_Level current_log_level = NOB_INFO;

void mewo_log_handler(Nob_Log_Level level, const char* fmt, va_list args) {
    if (level < current_log_level) {
        return;
    }

    const char* level_str = "";
    switch (level) {
        case NOB_INFO:    level_str = "INFO   "; break;
        case NOB_WARNING: level_str = "WARNING"; break;
        case NOB_ERROR:   level_str = "ERROR  "; break;
        default:          level_str = "LOG    "; break;
    }
    fprintf(stderr, "[%s] ", level_str);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
}

int main(int argc, char** argv) {
    bool* help    = flag_bool("help", false, "Show help", .short_name='h');
    bool* version = flag_bool("version", false, "Show version", .short_name='v');
    Flag_List* overrides = flag_list("D", "Override variable (use -Dname=value or -D name=value)");
    Flag_List* features_enable  = flag_list("F", "Enable feature (use +Fname or +F name)", .plus_sign=true);
    Flag_List* features_disable = flag_list("F", "Disable feature (use -Fname or -F name)");
    char** shell   = flag_str("shell", "", "Default shell");
    char** mewofile = flag_str("mewofile", "Mewofile", "Path to Mewofile", .short_name='f', .alias="file");
    bool* debug   = flag_bool("debug", false, "Enable debug output", .short_name='d');
    bool* dry_run = flag_bool("dry-run", false, "Print commands without executing");

    if (!flag_parse(argc, argv)) {
        flag_print_error(stderr);
        return 1;
    }

    if (*help) {
        usage(stdout);
        return 0;
    }

    if (*version) {
        printf("mewo version %d.%d\n", VERSION >> 8, VERSION & 0xFF);
        printf("Copyright (c) 2026 Markofwitch\n");
        #ifdef MEWO_RELEASE
        printf("Release build at %s\n", BUILD_TIME);
        #else
        printf("Development build at %s\n", BUILD_TIME);
        #endif
        return 0;
    }

    nob_set_log_handler(mewo_log_handler);

    if (!(*debug)) {
        current_log_level = NOB_ERROR;
    } else {
        current_log_level = NOB_INFO;
    }

    int rest = flag_rest_argc();
    char** args = flag_rest_argv();

    char* label = NULL;
    if (rest > 0 && strcmp(args[0], "--") == 0) {
        flag_shift_args(&rest, &args);
    } else if (rest > 0) {
        label = flag_shift_args(&rest, &args);
        if (rest > 0 && strcmp(args[0], "--") == 0) {
            flag_shift_args(&rest, &args);
        }
    }

    argv_init(args, rest);
    vars_init();

    for (size_t i = 0; i < overrides->count; i++) {
        const char* override = overrides->items[i];
        const char* eq = strchr(override, '=');
        if (eq) {
            size_t name_len = eq - override;
            char* name = malloc(name_len + 1);
            memcpy(name, override, name_len);
            name[name_len] = '\0';
            const char* value = eq + 1;
            vars_set_string(name, value);
            free(name);
        } else {
            vars_set_string(override, "");
        }
    }

    if (!nob_file_exists(*mewofile)) {
        fprintf(stderr, "Error: No Mewofile found in current directory\n");
        return 1;
    }

    char** lines = NULL;
    size_t lines_count = 0;
    if (!read_lines_entire_file(*mewofile, &lines, &lines_count)) {
        nob_log(NOB_ERROR, "Failed to read Mewofile %s", *mewofile);
        free_lines(lines, lines_count);
        return 1;
    }

    AST* ast = parse((const char**)lines, lines_count);

    if (*debug) {
        if (label) {
            printf("Invoking label: %s\n", label);
            print_ast_label(ast, label);
        } else {
            printf("No label specified, executing default\n");
            print_ast(ast);
        }
    }

    if (has_error()) {
        print_error(*mewofile, stderr);
        free_lines(lines, lines_count);
        return 1;
    }

    if (!execute_and_cleanup(ast, label, *dry_run, *shell && **shell ? *shell : NULL,
                             (const char**)features_enable->items, features_enable->count,
                             (const char**)features_disable->items, features_disable->count)) {
        if (has_error()) {
            print_error(*mewofile, stderr);
        }
        free_ast(ast);
        free_lines(lines, lines_count);
        return 1;
    }

    free_ast(ast);
    free_lines(lines, lines_count);
    return 0;
}
