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

#define FLAG_PUSH_DASH_DASH_BACK
#define FLAG_IMPLEMENTATION
#include "../thirdparty/flag.h"
#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "../thirdparty/nob.h"

#include "error.c"
#include "vars.c"
#include "parser.c"
#include "exec.c"

static const int VERSION = 0x0100;

static void usage(FILE* stream) {
    fprintf(stream, "Usage: mewo [LABEL] [OPTIONS]\n");
    fprintf(stream, "OPTIONS:\n");
    flag_print_options(stream);
}

static void split_lines(const char* code, char*** out_lines, size_t* out_count) {
    size_t capacity = 16;
    size_t count = 0;
    char** lines = malloc(capacity * sizeof(char*));
    if (!lines) {
        *out_lines = NULL;
        *out_count = 0;
        return;
    }

    const char* start = code;
    for (const char* p = code; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = p - start;
            if (len > 0 && start[len - 1] == '\r') len--;
            
            char* line = malloc(len + 1);
            if (line) {
                memcpy(line, start, len);
                line[len] = '\0';
                
                if (count >= capacity) {
                    capacity *= 2;
                    char** new_lines = realloc(lines, capacity * sizeof(char*));
                    if (!new_lines) break;
                    lines = new_lines;
                }
                lines[count++] = line;
            }
            
            if (*p == '\0') break;
            start = p + 1;
        }
    }

    *out_lines = lines;
    *out_count = count;
}

static Nob_Log_Level current_log_level = NOB_INFO;

void mewo_log_handler(Nob_Log_Level level, const char *fmt, va_list args) {
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
        return 0;
    }

    nob_set_log_handler(mewo_log_handler);

    if (!(*debug)) {
        current_log_level = NOB_ERROR;
    } else {
        current_log_level = NOB_INFO;
    }

    int rest = flag_rest_argc();
    char **args = flag_rest_argv();

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

    String_Builder sb = {0};

    if (!nob_file_exists(*mewofile)) {
        fprintf(stderr, "Error: No Mewofile found in current directory\n");
        return 1;
    }

    if (!read_entire_file(*mewofile, &sb)) {
        fprintf(stderr, "Error: Failed to read Mewofile\n");
        return 1;
    }

    String_View sv = sb_to_sv(sb);
    const char* code = nob_temp_sv_to_cstr(sv);

    char** lines = NULL;
    size_t lines_count = 0;
    split_lines(code, &lines, &lines_count);
    if (!lines) {
        fprintf(stderr, "Error: Failed to split Mewofile into lines\n");
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
        return 1;
    }

    if (!execute_and_cleanup(ast, label, *dry_run, *shell && **shell ? *shell : NULL,
                             (const char**)features_enable->items, features_enable->count,
                             (const char**)features_disable->items, features_disable->count)) {
        if (has_error()) {
            print_error(*mewofile, stderr);
        }
        free_ast(ast);
        return 1;
    }

    free_ast(ast);
    return 0;
}
