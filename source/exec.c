/*
 * exec.c - AST Execution Engine for Mewo
 * 
 * Features:
 *   - Execute statements from parsed AST
 *   - Target/label resolution and execution
 *   - Conditional execution (#if/#else/#endif)
 *   - Variable interpolation in commands
 *   - goto (continues after target) / call (returns back) semantics
 *   - Inside labels: call other labels by name
 */

/* Note: This file is included from main.c which provides:
 *   - stdio.h, stdlib.h, string.h, stdbool.h
 *   - str_dup() from error.c
 *   - nob.h utilities
 *   - AST, Stmt types from parser.c
 *   - Variable types and functions from vars.c
 */

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getcwd _getcwd
#define chdir _chdir
#define getpid _getpid
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

typedef struct {
    AST* ast;
    bool dry_run;
    char* default_shell;
    size_t current_index;
    
    struct {
        char** names;
        size_t* indices;
        size_t count;
        size_t capacity;
    } labels;
    
    struct {
        size_t* return_indices;
        size_t count;
        size_t capacity;
    } call_stack;
    
    int current_label_index;
    
    struct {
        Stmt** attrs;
        size_t count;
        size_t capacity;
    } pending_attrs;
    
} ExecContext;

static bool exec_stmt(ExecContext* ctx, Stmt* stmt, size_t line_number);
static bool exec_label(ExecContext* ctx, const char* label_name, size_t caller_line);
static bool exec_command(ExecContext* ctx, const char* raw_cmd, size_t line_number);
static bool exec_top_level_except_calls_and_gotos(ExecContext* ctx);
static int find_label_index(ExecContext* ctx, const char* name);
static bool is_label_name(ExecContext* ctx, const char* name);

static void ctx_init(ExecContext* ctx, AST* ast, bool dry_run, const char* shell) {
    memset(ctx, 0, sizeof(ExecContext));
    ctx->ast = ast;
    ctx->dry_run = dry_run;
    ctx->default_shell = shell ? str_dup(shell) : NULL;
    ctx->current_label_index = -1;
}

static void ctx_free(ExecContext* ctx) {
    for (size_t i = 0; i < ctx->labels.count; i++) {
        free(ctx->labels.names[i]);
    }
    free(ctx->labels.names);
    free(ctx->labels.indices);
    free(ctx->call_stack.return_indices);
    free(ctx->pending_attrs.attrs);
    free(ctx->default_shell);
}

static bool ctx_register_label(ExecContext* ctx, const char* name, size_t index) {
    for (size_t i = 0; i < ctx->labels.count; i++) {
        if (strcmp(ctx->labels.names[i], name) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Duplicate label '%s'", name);
            set_error(ERROR_RUNTIME, msg, index + 1);
            return false;
        }
    }
    
    if (ctx->labels.count >= ctx->labels.capacity) {
        size_t new_cap = ctx->labels.capacity == 0 ? 8 : ctx->labels.capacity * 2;
        char** new_names = realloc(ctx->labels.names, new_cap * sizeof(char*));
        size_t* new_indices = realloc(ctx->labels.indices, new_cap * sizeof(size_t));
        if (!new_names || !new_indices) {
            free(new_names);
            free(new_indices);
            return false;
        }
        ctx->labels.names = new_names;
        ctx->labels.indices = new_indices;
        ctx->labels.capacity = new_cap;
    }
    
    ctx->labels.names[ctx->labels.count] = str_dup(name);
    ctx->labels.indices[ctx->labels.count] = index;
    ctx->labels.count++;
    return true;
}

static bool ctx_push_return(ExecContext* ctx, size_t return_index) {
    if (ctx->call_stack.count >= ctx->call_stack.capacity) {
        size_t new_cap = ctx->call_stack.capacity == 0 ? 16 : ctx->call_stack.capacity * 2;
        size_t* new_stack = realloc(ctx->call_stack.return_indices, new_cap * sizeof(size_t));
        if (!new_stack) return false;
        ctx->call_stack.return_indices = new_stack;
        ctx->call_stack.capacity = new_cap;
    }
    ctx->call_stack.return_indices[ctx->call_stack.count++] = return_index;
    return true;
}

static bool ctx_pop_return(ExecContext* ctx, size_t* out_index) {
    if (ctx->call_stack.count == 0) return false;
    *out_index = ctx->call_stack.return_indices[--ctx->call_stack.count];
    return true;
}

static void ctx_add_pending_attr(ExecContext* ctx, Stmt* attr) {
    if (ctx->pending_attrs.count >= ctx->pending_attrs.capacity) {
        size_t new_cap = ctx->pending_attrs.capacity == 0 ? 4 : ctx->pending_attrs.capacity * 2;
        Stmt** new_attrs = realloc(ctx->pending_attrs.attrs, new_cap * sizeof(Stmt*));
        if (!new_attrs) return;
        ctx->pending_attrs.attrs = new_attrs;
        ctx->pending_attrs.capacity = new_cap;
    }
    ctx->pending_attrs.attrs[ctx->pending_attrs.count++] = attr;
}

static void ctx_clear_pending_attrs(ExecContext* ctx) {
    ctx->pending_attrs.count = 0;
}

static int find_label_index(ExecContext* ctx, const char* name) {
    for (size_t i = 0; i < ctx->labels.count; i++) {
        if (strcmp(ctx->labels.names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_label_name(ExecContext* ctx, const char* name) {
    return find_label_index(ctx, name) >= 0;
}

static size_t find_label_end(ExecContext* ctx, size_t label_stmt_index) {
    Stmt* label_stmt = ctx->ast->stmts[label_stmt_index];
    int label_indent = label_stmt->indent_level;
    
    for (size_t i = label_stmt_index + 1; i < ctx->ast->stmts_count; i++) {
        Stmt* stmt = ctx->ast->stmts[i];
        if (stmt->indent_level <= label_indent) {
            return i;
        }
    }
    return ctx->ast->stmts_count;
}

static bool is_platform_windows(void) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__) || defined(_MSC_VER)
    return true;
#else
    return false;
#endif
}

static bool is_platform_linux(void) {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

static bool is_platform_macos(void) {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

static bool is_platform_unix(void) {
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    return true;
#else
    return false;
#endif
}

static const char* get_arch(void) {
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#elif defined(__riscv)
    return "riscv";
#else
    return "unknown";
#endif
}

static char* get_distro(void) {
#ifdef __linux__
    FILE* f = fopen("/etc/os-release", "r");
    if (!f) return str_dup("unknown");
    
    char line[256];
    char* distro = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ID=", 3) == 0) {
            char* start = line + 3;
            if (*start == '"') start++;
            size_t len = strlen(start);
            while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r' || start[len-1] == '"')) {
                len--;
            }
            distro = malloc(len + 1);
            if (distro) {
                memcpy(distro, start, len);
                distro[len] = '\0';
            }
            break;
        }
    }
    fclose(f);
    return distro ? distro : str_dup("unknown");
#else
    return str_dup("none");
#endif
}

static bool check_conditional_attr(Stmt* attr, size_t line_number) {
    const char* name = attr->attr.name;
    
    if (strcmp(name, "windows") == 0 || strcmp(name, "win32") == 0) {
        return is_platform_windows();
    }
    if (strcmp(name, "linux") == 0) {
        return is_platform_linux();
    }
    if (strcmp(name, "macos") == 0 || strcmp(name, "darwin") == 0) {
        return is_platform_macos();
    }
    if (strcmp(name, "unix") == 0) {
        return is_platform_unix();
    }

    if (strcmp(name, "arch") == 0) {
        if (attr->attr.param_count > 0) {
            const char* expected = attr->attr.parameters[0]->command.raw_line;
            const char* actual = get_arch();
            return strcmp(expected, actual) == 0;
        }
        return false;
    }
    
    if (strcmp(name, "distro") == 0) {
        if (attr->attr.param_count > 0) {
            const char* expected = attr->attr.parameters[0]->command.raw_line;
            char* actual = get_distro();
            bool match = strcmp(expected, actual) == 0;
            free(actual);
            return match;
        }
        return false;
    }
    
    if (strcmp(name, "feature") == 0) {
        if (attr->attr.param_count > 0) {
            return feature_exists(attr->attr.parameters[0]->command.raw_line);
        }
        return false;
    }
    
    if (strcmp(name, "env") == 0) {
        if (attr->attr.param_count > 0) {
            const char* env_name = attr->attr.parameters[0]->command.raw_line;
            const char* env_value = getenv(env_name);
            if (!env_value) return false;
            if (attr->attr.param_count > 1) {
                const char* expected = attr->attr.parameters[1]->command.raw_line;
                return strcmp(env_value, expected) == 0;
            }
            return true;
        }
        return false;
    }
    
    if (strcmp(name, "exists") == 0) {
        if (attr->attr.param_count > 0) {
            char* raw_param = attr->attr.parameters[0]->command.raw_line;
            char* path;
            
            if (raw_param[0] == '"' && raw_param[strlen(raw_param) - 1] == '"') {
                size_t len = strlen(raw_param) - 2;
                path = malloc(len + 1);
                memcpy(path, raw_param + 1, len);
                path[len] = '\0';
            } else {
                Variable* var = vars_get(raw_param);
                if (!var || var->type != VAR_STRING) {
                    path = interpolate(raw_param, line_number);
                    if (!path) return false;
                } else {
                    path = str_dup(var->string_value ? var->string_value : "");
                }
            }
            
            bool result = file_exists(path);
            free(path);
            return result;
        }
        return false;
    }
    
    return true;
}

static bool is_conditional_attr(const char* name) {
    return strcmp(name, "windows") == 0 ||
           strcmp(name, "win32") == 0 ||
           strcmp(name, "linux") == 0 ||
           strcmp(name, "macos") == 0 ||
           strcmp(name, "darwin") == 0 ||
           strcmp(name, "unix") == 0 ||
           strcmp(name, "arch") == 0 ||
           strcmp(name, "distro") == 0 ||
           strcmp(name, "feature") == 0 ||
           strcmp(name, "env") == 0 ||
           strcmp(name, "exists") == 0;
}

static bool check_pending_conditionals(ExecContext* ctx) {
    for (size_t i = 0; i < ctx->pending_attrs.count; i++) {
        Stmt* attr = ctx->pending_attrs.attrs[i];
        if (is_conditional_attr(attr->attr.name)) {
            if (!check_conditional_attr(attr, 0)) { // TODO: pass proper line number
                return false;
            }
        }
    }
    return true;
}

typedef struct {
    bool ignore_fail;
    int expect_code;
    bool has_expect;
    char* cwd;
    char* shell;
    int timeout_ms;
    bool once;
    char* save_stream;
    char* save_var;
    bool use_system_shell;
} CmdAttrs;

static void cmd_attrs_init(CmdAttrs* attrs) {
    memset(attrs, 0, sizeof(CmdAttrs));
    attrs->expect_code = 0;
}

static void cmd_attrs_free(CmdAttrs* attrs) {
    free(attrs->cwd);
    free(attrs->shell);
    free(attrs->save_stream);
    free(attrs->save_var);
}

static void apply_pending_attrs(ExecContext* ctx, CmdAttrs* attrs) {
    for (size_t i = 0; i < ctx->pending_attrs.count; i++) {
        Stmt* attr = ctx->pending_attrs.attrs[i];
        
        if (strcmp(attr->attr.name, "ignorefail") == 0) {
            attrs->ignore_fail = true;
        } else if (strcmp(attr->attr.name, "expect") == 0) {
            attrs->has_expect = true;
            if (attr->attr.param_count > 0) {
                attrs->expect_code = atoi(attr->attr.parameters[0]->command.raw_line);
            }
        } else if (strcmp(attr->attr.name, "cwd") == 0) {
            if (attr->attr.param_count > 0) {
                attrs->cwd = str_dup(attr->attr.parameters[0]->command.raw_line);
            }
        } else if (strcmp(attr->attr.name, "shell") == 0) {
            if (attr->attr.param_count > 0) {
                const char* shell_name = attr->attr.parameters[0]->command.raw_line;
                bool is_global = false;
                
                if (strcmp(shell_name, "default") == 0) {
                    attrs->use_system_shell = true;
                    if (attr->attr.param_count > 1) {
                        const char* mode = attr->attr.parameters[1]->command.raw_line;
                        if (strcmp(mode, "global") == 0) {
                            set_global_shell(NULL);
                        }
                    }
                } else {
                    if (attr->attr.param_count > 1) {
                        const char* mode = attr->attr.parameters[1]->command.raw_line;
                        if (strcmp(mode, "global") == 0) {
                            is_global = true;
                        }
                    }
                    
                    if (is_global) {
                        set_global_shell(shell_name);
                    } else {
                        attrs->shell = str_dup(shell_name);
                    }
                }
            } else {
#ifdef _WIN32
                attrs->shell = str_dup("cmd.exe");
#else
                attrs->shell = str_dup("/bin/sh");
#endif
            }
        } else if (strcmp(attr->attr.name, "timeout") == 0) {
            if (attr->attr.param_count > 0) {
                attrs->timeout_ms = atoi(attr->attr.parameters[0]->command.raw_line);
            }
        } else if (strcmp(attr->attr.name, "once") == 0) {
            attrs->once = true;
        } else if (strcmp(attr->attr.name, "save") == 0) {
            if (attr->attr.param_count >= 2) {
                attrs->save_stream = str_dup(attr->attr.parameters[0]->command.raw_line);
                attrs->save_var = str_dup(attr->attr.parameters[1]->command.raw_line);
            }
        }
    }
    ctx_clear_pending_attrs(ctx);
}

static bool exec_command(ExecContext* ctx, const char* raw_cmd, size_t line_number) {
    char* cmd = interpolate(raw_cmd, line_number);
    if (!cmd) {
        return false;
    }
    
    CmdAttrs attrs;
    cmd_attrs_init(&attrs);
    apply_pending_attrs(ctx, &attrs);
    
    if (ctx->dry_run) {
        printf("[dry-run] %s\n", cmd);
        free(cmd);
        cmd_attrs_free(&attrs);
        return true;
    }
    
    char* old_cwd = NULL;
    if (attrs.cwd) {
        old_cwd = malloc(4096);
        if (old_cwd) {
            getcwd(old_cwd, 4096);
            chdir(attrs.cwd);
        }
    }
    
    bool success = true;
    int exit_code = 0;
    
    const char* use_shell = NULL;
    if (!attrs.use_system_shell) {
        use_shell = attrs.shell ? attrs.shell : get_global_shell();
    }
    
    if (use_shell) {
        Cmd nob_cmd = {0};
        if (strstr(use_shell, "%s")) {
            char buffer[1024];
            snprintf(buffer, sizeof(buffer), use_shell, cmd);
            nob_cmd_append(&nob_cmd, buffer);
        } else {
            #ifdef _WIN32
                    nob_cmd_append(&nob_cmd, use_shell, "/c", cmd);
            #else
                    nob_cmd_append(&nob_cmd, use_shell, "-c", cmd);
            #endif
        }
        
        if (attrs.save_stream && attrs.save_var) {
            char temp_file[256];
#ifdef _WIN32
            snprintf(temp_file, sizeof(temp_file), "%s\\mewo_capture_%d.tmp", 
                     getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
#else
            snprintf(temp_file, sizeof(temp_file), "/tmp/mewo_capture_%d.tmp", getpid());
#endif
            
            Nob_Cmd_Opt opt = {0};
            opt.dont_reset = true;
            if (strcmp(attrs.save_stream, "stdout") == 0) {
                opt.stdout_path = temp_file;
            } else if (strcmp(attrs.save_stream, "stderr") == 0) {
                opt.stderr_path = temp_file;
            }
            
            success = nob_cmd_run_opt(&nob_cmd, opt);
            
            String_Builder sb = {0};
            if (read_entire_file(temp_file, &sb)) {
                sb_append_null(&sb);
                vars_set_string(attrs.save_var, sb.items ? sb.items : "");
                sb_free(sb);
            } else {
                vars_set_string(attrs.save_var, "");
            }
            nob_delete_file(temp_file);
        } else {
            Nob_Proc proc = nob_cmd_run_async(nob_cmd);
            success = nob_proc_wait(proc);
        }
        
        cmd_free(nob_cmd);
        exit_code = success ? 0 : 1;
    } else {
        exit_code = system(cmd);
#ifdef _WIN32
        success = (exit_code == 0);
#else
        success = (WIFEXITED(exit_code) && WEXITSTATUS(exit_code) == 0);
        exit_code = WIFEXITED(exit_code) ? WEXITSTATUS(exit_code) : -1;
#endif
    }
    
    set_last_exit_code(exit_code);
    
    if (attrs.has_expect) {
        success = (exit_code == attrs.expect_code);
        if (!success) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Expected exit code %d but got %d", 
                     attrs.expect_code, exit_code);
            set_error(ERROR_RUNTIME, msg, line_number);
        }
    } else if (!success && !attrs.ignore_fail) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Command failed with exit code %d", exit_code);
        set_error(ERROR_RUNTIME, msg, line_number);
    }
    
    if (attrs.ignore_fail) success = true;
    
    if (old_cwd) {
        chdir(old_cwd);
        free(old_cwd);
    }
    
    free(cmd);
    cmd_attrs_free(&attrs);
    
    return success;
}

static const char* starts_with_attr(const char* s, const char* prefix) {
    size_t len = strlen(prefix);
    if (strncmp(s, prefix, len) == 0) {
        return s + len;
    }
    return NULL;
}

static char* extract_attr_param(const char* start) {
    if (*start != '(') return NULL;
    start++;
    
    const char* end = start;
    int depth = 1;
    while (*end && depth > 0) {
        if (*end == '(') depth++;
        else if (*end == ')') depth--;
        if (depth > 0) end++;
    }
    
    size_t len = end - start;
    char* param = malloc(len + 1);
    if (!param) return NULL;
    memcpy(param, start, len);
    param[len] = '\0';
    return param;
}

static bool eval_condition(const char* condition, size_t line_number, bool* result) {
    const char* p = condition;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    const char* after;
    if ((after = starts_with_attr(p, "#feature"))) {
        char* param = extract_attr_param(after);
        if (param) {
            *result = feature_exists(param);
            free(param);
            return true;
        }
        set_error(ERROR_SYNTAX, "Invalid #feature syntax", line_number);
        return false;
    }
    
    if ((after = starts_with_attr(p, "#defined"))) {
        char* param = extract_attr_param(after);
        if (param) {
            *result = vars_exists(param);
            free(param);
            return true;
        }
        set_error(ERROR_SYNTAX, "Invalid #defined syntax", line_number);
        return false;
    }
    
    if ((after = starts_with_attr(p, "#len"))) {
        char* param = extract_attr_param(after);
        if (param) {
            size_t len = 0;
            if (strcmp(param, "argv") == 0) {
                len = argv_count();
            } else {
                Variable* var = vars_get(param);
                if (var) {
                    if (var->type == VAR_ARRAY) {
                        len = var_array_len(var);
                    } else if (var->type == VAR_STRING) {
                        len = strlen(var->string_value ? var->string_value : "");
                    } else {
                        len = 1;
                    }
                }
            }
            free(param);
            *result = (len > 0);
            return true;
        }
        set_error(ERROR_SYNTAX, "Invalid #len syntax", line_number);
        return false;
    }
    
#ifdef _WIN32
    if (strcmp(p, "#windows") == 0 || strcmp(p, "#win32") == 0) {
        *result = true;
        return true;
    }
    if (strcmp(p, "#linux") == 0 || strcmp(p, "#macos") == 0 || strcmp(p, "#unix") == 0) {
        *result = false;
        return true;
    }
#elif defined(__linux__)
    if (strcmp(p, "#linux") == 0 || strcmp(p, "#unix") == 0) {
        *result = true;
        return true;
    }
    if (strcmp(p, "#windows") == 0 || strcmp(p, "#win32") == 0 || strcmp(p, "#macos") == 0) {
        *result = false;
        return true;
    }
#elif defined(__APPLE__)
    if (strcmp(p, "#macos") == 0 || strcmp(p, "#unix") == 0) {
        *result = true;
        return true;
    }
    if (strcmp(p, "#windows") == 0 || strcmp(p, "#win32") == 0 || strcmp(p, "#linux") == 0) {
        *result = false;
        return true;
    }
#endif

    char* cond = interpolate(condition, line_number);
    if (!cond) return false;
    
    p = cond;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* is it truthy?
     * - "true" -> true
     * - "false" -> false  
     * - Non-zero number -> true
     * - Zero or empty -> false
     * - var exists and is truthy -> true
     */
    
    if (strcmp(p, "true") == 0) {
        *result = true;
    } else if (strcmp(p, "false") == 0) {
        *result = false;
    } else if (*p == '\0') {
        *result = false;
    } else {
        char* end;
        double num = strtod(p, &end);
        while (*end && (*end == ' ' || *end == '\t')) end++;
        
        if (*end == '\0') {
            *result = (num != 0.0);
        } else {
            *result = true;
        }
    }
    
    free(cond);
    return true;
}

static bool exec_stmt(ExecContext* ctx, Stmt* stmt, size_t stmt_index) {
    size_t line_number = stmt->line_number;
    
    if (stmt->type != STMT_ATTR && ctx->pending_attrs.count > 0) {
        if (!check_pending_conditionals(ctx)) {
            ctx_clear_pending_attrs(ctx);
            return true;
        }
    }
    
    switch (stmt->type) {
        case STMT_ATTR: {
            if (strcmp(stmt->attr.name, "assert") == 0) {
                if (stmt->attr.param_count > 0) {
                    const char* condition = stmt->attr.parameters[0]->command.raw_line;
                    bool result = false;
                    if (!eval_condition(condition, line_number, &result)) {
                        return false;
                    }
                    if (!result) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "Assertion failed: %s", condition);
                        set_error(ERROR_RUNTIME, msg, line_number);
                        return false;
                    }
                } else {
                    set_error(ERROR_SYNTAX, "#assert requires a condition", line_number);
                    return false;
                }
                return true;
            }
            
            if (strcmp(stmt->attr.name, "features") == 0) {
                if (stmt->attr.param_count > 0) {
                    const char* list = stmt->attr.parameters[0]->command.raw_line;
                    const char* p = list;
                    while (*p) {
                        while (*p && isspace(*p)) p++;
                        const char* start = p;
                        while (*p && *p != ',') p++;
                        const char* end = p;
                        while (end > start && isspace(*(end-1))) end--;
                        
                        if (end > start) {
                            char* name = malloc(end - start + 1);
                            memcpy(name, start, end - start);
                            name[end - start] = '\0';
                            feature_enable(name);
                            free(name);
                        }
                        if (*p == ',') p++;
                    }
                }
                return true;
            }
            ctx_add_pending_attr(ctx, stmt);
            return true;
        }
            
        case STMT_VAR_ASSIGN: {
            char* interp_value = interpolate(stmt->var_assign.value, line_number);
            if (!interp_value) return false;
            
            Variable* val = parse_value(interp_value, line_number);
            free(interp_value);
            if (!val) return false;
            
            if (!vars_set(stmt->var_assign.name, val)) {
                set_error(ERROR_MEMORY, "Failed to set variable", line_number);
                return false;
            }
            ctx_clear_pending_attrs(ctx);
            return true;
        }
        
        case STMT_INDEX_ASSIGN: {
            char* interp_index = interpolate(stmt->index_assign.index, line_number);
            if (!interp_index) return false;
            
            char* interp_value = interpolate(stmt->index_assign.value, line_number);
            if (!interp_value) {
                free(interp_index);
                return false;
            }
            
            Variable* var = vars_get(stmt->index_assign.name);
            if (!var) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable: '%s'", stmt->index_assign.name);
                set_error(ERROR_RUNTIME, msg, line_number);
                free(interp_index);
                free(interp_value);
                return false;
            }
            
            size_t idx = (size_t)atoll(interp_index);
            free(interp_index);
            
            Variable* new_val = parse_value(interp_value, line_number);
            free(interp_value);
            if (!new_val) return false;
            
            if (var->type == VAR_ARRAY) {
                if (idx < var->array_value.count) {
                    var_free(var->array_value.items[idx]);
                    var->array_value.items[idx] = new_val;
                } else {
                    while (var->array_value.count <= idx) {
                        var_array_push(var, var_new_string(""));
                    }
                    var_free(var->array_value.items[idx]);
                    var->array_value.items[idx] = new_val;
                }
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Cannot index assign to non-array variable '%s'", stmt->index_assign.name);
                set_error(ERROR_RUNTIME, msg, line_number);
                var_free(new_val);
                return false;
            }
            ctx_clear_pending_attrs(ctx);
            return true;
        }
        
        case STMT_LABEL:
            ctx_clear_pending_attrs(ctx);
            return true;
        
        case STMT_LABEL_ALIAS:
            ctx_clear_pending_attrs(ctx);
            return true;
            
        case STMT_COMMAND: {
            const char* cmd = stmt->command.raw_line;
            
            if (ctx->current_label_index >= 0 && is_label_name(ctx, cmd)) {
                ctx_clear_pending_attrs(ctx);
                return exec_label(ctx, cmd, line_number);
            }
            
            return exec_command(ctx, cmd, line_number);
        }
        
        case STMT_GOTO: {
            int label_idx = find_label_index(ctx, stmt->goto_stmt.target);
            if (label_idx < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Unknown label '%s'", stmt->goto_stmt.target);
                set_error(ERROR_RUNTIME, msg, line_number);
                return false;
            }
            
            ctx->current_index = ctx->labels.indices[label_idx];
            ctx->current_label_index = label_idx;
            ctx_clear_pending_attrs(ctx);
            return true;
        }
        
        case STMT_CALL: {
            ctx_clear_pending_attrs(ctx);
            return exec_label(ctx, stmt->call_stmt.target, line_number);
        }

        case STMT_IF:
        case STMT_ELSE:
        case STMT_ENDIF:
            return true;
            
        default:
            ctx_clear_pending_attrs(ctx);
            return true;
    }
}

static bool exec_range(ExecContext* ctx, size_t start, size_t end, int base_indent) {
    (void)base_indent;
    size_t i = start;
    
    while (i < end) {
        Stmt* stmt = ctx->ast->stmts[i];
        
        if (stmt->type == STMT_IF) {
            bool condition_result = false;
            if (!eval_condition(stmt->if_stmt.condition, i + 1, &condition_result)) {
                return false;
            }
            
            size_t else_idx = 0;
            size_t endif_idx = 0;
            int depth = 1;
            bool found_else = false;
            
            for (size_t j = i + 1; j < end && depth > 0; j++) {
                Stmt* s = ctx->ast->stmts[j];
                if (s->indent_level != stmt->indent_level) continue;
                
                if (s->type == STMT_IF) {
                    depth++;
                } else if (s->type == STMT_ELSE && depth == 1) {
                    else_idx = j;
                    found_else = true;
                } else if (s->type == STMT_ENDIF) {
                    depth--;
                    if (depth == 0) {
                        endif_idx = j;
                    }
                }
            }
            
            if (endif_idx == 0) {
                set_error(ERROR_SYNTAX, "Missing #endif for #if", i + 1);
                return false;
            }
            
            if (condition_result) {
                size_t branch_end = found_else ? else_idx : endif_idx;
                if (!exec_range(ctx, i + 1, branch_end, base_indent + 1)) {
                    return false;
                }
            } else if (found_else) {
                if (!exec_range(ctx, else_idx + 1, endif_idx, base_indent + 1)) {
                    return false;
                }
            }
            
            i = endif_idx + 1;
            continue;
        }
        
        if (stmt->type == STMT_ELSE || stmt->type == STMT_ENDIF) {
            i++;
            continue;
        }
        
        if (!exec_stmt(ctx, stmt, i)) {
            return false;
        }
        
        if (stmt->type == STMT_GOTO && ctx->current_index != i) {
            return true;
        }
        
        i++;
    }
    
    return true;
}

static bool exec_label(ExecContext* ctx, const char* label_name, size_t caller_line) {
    int label_idx = find_label_index(ctx, label_name);
    if (label_idx < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown label '%s'", label_name);
        set_error(ERROR_RUNTIME, msg, caller_line);
        return false;
    }
    
    if (!exec_top_level_except_calls_and_gotos(ctx)) {
        return false;
    }
    
    size_t label_stmt_idx = ctx->labels.indices[label_idx];
    Stmt* label_stmt = ctx->ast->stmts[label_stmt_idx];
    
    if (label_stmt->type == STMT_LABEL_ALIAS) {
        bool success = true;
        for (size_t k = 0; k < label_stmt->label_alias.target_count; k++) {
            success = exec_label(ctx, label_stmt->label_alias.targets[k], caller_line);
            if (!success) break;
        }
        return success;
    } else {
        size_t label_end = find_label_end(ctx, label_stmt_idx);
        
        int prev_label = ctx->current_label_index;
        ctx->current_label_index = label_idx;
        
        bool success = exec_range(ctx, label_stmt_idx + 1, label_end, 1);
        
        ctx->current_label_index = prev_label;
        
        return success;
    }
}

static bool register_all_labels(ExecContext* ctx) {
    for (size_t i = 0; i < ctx->ast->stmts_count; i++) {
        Stmt* stmt = ctx->ast->stmts[i];
        Stmt* before_stmt = (i > 0) ? ctx->ast->stmts[i - 1] : NULL;
        if (before_stmt && before_stmt->type == STMT_ATTR) {
            if (is_conditional_attr(before_stmt->attr.name)) {
                bool cond_result = check_conditional_attr(before_stmt, i);
                if (!cond_result) {
                    continue;
                }
            }
        }
        if (stmt->type == STMT_LABEL && stmt->indent_level == 0) {
            if (stmt->label.name[0] != '\0') {
                if (!ctx_register_label(ctx, stmt->label.name, i)) {
                    return false;
                }
            }
        }
        if (stmt->type == STMT_LABEL_ALIAS && stmt->indent_level == 0) {
            if (!ctx_register_label(ctx, stmt->label_alias.name, i)) {
                return false;
            }
        }
    }
    return true;
}

static bool exec_top_level(ExecContext* ctx) {
    size_t i = 0;
    
    while (i < ctx->ast->stmts_count) {
        Stmt* stmt = ctx->ast->stmts[i];
        
        if ((stmt->type == STMT_LABEL || stmt->type == STMT_LABEL_ALIAS) && stmt->indent_level == 0) {
            if (stmt->type == STMT_LABEL && stmt->label.name[0] == '\0') {
                size_t end = find_label_end(ctx, i);
                if (!exec_range(ctx, i + 1, end, 1)) {
                    return false;
                }
            } else {
                ctx_clear_pending_attrs(ctx);
            }
            i = find_label_end(ctx, i);
            continue;
        }
        
        if (stmt->indent_level == 0) {
            ctx->current_index = i;
            
            if (stmt->type == STMT_IF) {
                bool condition_result = false;
                if (!eval_condition(stmt->if_stmt.condition, i + 1, &condition_result)) {
                    return false;
                }
                
                size_t else_idx = 0;
                size_t endif_idx = 0;
                int depth = 1;
                bool found_else = false;
                
                for (size_t j = i + 1; j < ctx->ast->stmts_count && depth > 0; j++) {
                    Stmt* s = ctx->ast->stmts[j];
                    if (s->indent_level != 0) continue;
                    
                    if (s->type == STMT_IF) {
                        depth++;
                    } else if (s->type == STMT_ELSE && depth == 1) {
                        else_idx = j;
                        found_else = true;
                    } else if (s->type == STMT_ENDIF) {
                        depth--;
                        if (depth == 0) {
                            endif_idx = j;
                        }
                    }
                }
                
                if (endif_idx == 0) {
                    set_error(ERROR_SYNTAX, "Missing #endif for #if", i + 1);
                    return false;
                }
                
                if (condition_result) {
                    size_t branch_end = found_else ? else_idx : endif_idx;
                    if (!exec_range(ctx, i + 1, branch_end, 1)) {
                        return false;
                    }
                } else if (found_else) {
                    if (!exec_range(ctx, else_idx + 1, endif_idx, 1)) {
                        return false;
                    }
                }
                
                i = endif_idx + 1;
                continue;
            }
            
            if (stmt->type == STMT_ELSE || stmt->type == STMT_ENDIF) {
                i++;
                continue;
            }
            
            if (!exec_stmt(ctx, stmt, i)) {
                return false;
            }
            
            if (stmt->type == STMT_GOTO && ctx->current_index != i) {
                i = ctx->current_index + 1;
                continue;
            }
        }
        
        i++;
    }
    
    return true;
}

static bool exec_top_level_except_calls_and_gotos(ExecContext* ctx) {
    size_t i = 0;
    
    while (i < ctx->ast->stmts_count) {
        Stmt* stmt = ctx->ast->stmts[i];
        
        if ((stmt->type == STMT_LABEL || stmt->type == STMT_LABEL_ALIAS) && stmt->indent_level == 0) {
            if (stmt->type == STMT_LABEL && stmt->label.name[0] == '\0') {
                // Execute empty label body with pending attrs
                size_t end = find_label_end(ctx, i);
                if (!exec_range(ctx, i + 1, end, 1)) {
                    return false;
                }
            } else {
                ctx_clear_pending_attrs(ctx);
            }
            i = find_label_end(ctx, i);
            continue;
        }
        
        if (stmt->indent_level == 0) {
            ctx->current_index = i;
            
            if (stmt->type == STMT_IF) {
                bool condition_result = false;
                if (!eval_condition(stmt->if_stmt.condition, i + 1, &condition_result)) {
                    return false;
                }
                
                size_t else_idx = 0;
                size_t endif_idx = 0;
                int depth = 1;
                bool found_else = false;
                
                for (size_t j = i + 1; j < ctx->ast->stmts_count && depth > 0; j++) {
                    Stmt* s = ctx->ast->stmts[j];
                    if (s->indent_level != 0) continue;
                    
                    if (s->type == STMT_IF) {
                        depth++;
                    } else if (s->type == STMT_ELSE && depth == 1) {
                        else_idx = j;
                        found_else = true;
                    } else if (s->type == STMT_ENDIF) {
                        depth--;
                        if (depth == 0) {
                            endif_idx = j;
                        }
                    }
                }
                
                if (endif_idx == 0) {
                    set_error(ERROR_SYNTAX, "Missing #endif for #if", i + 1);
                    return false;
                }
                
                if (condition_result) {
                    size_t branch_end = found_else ? else_idx : endif_idx;
                    if (!exec_range(ctx, i + 1, branch_end, 1)) {
                        return false;
                    }
                } else if (found_else) {
                    if (!exec_range(ctx, else_idx + 1, endif_idx, 1)) {
                        return false;
                    }
                }
                
                i = endif_idx + 1;
                continue;
            }
            
            if (stmt->type == STMT_ELSE || stmt->type == STMT_ENDIF) {
                i++;
                continue;
            }
            
            if (stmt->type == STMT_CALL || stmt->type == STMT_GOTO) {
                i++;
                continue;
            }
            
            if (!exec_stmt(ctx, stmt, i)) {
                return false;
            }
            
            if (stmt->type == STMT_GOTO && ctx->current_index != i) {
                i = ctx->current_index + 1;
                continue;
            }
        }
        
        i++;
    }
    
    return true;
}

/*
 * Execute the AST
 * 
 * Parameters:
 *   ast              - The parsed AST
 *   label            - Label to execute, or NULL to execute top-level code
 *   dry_run          - If true, print commands without executing
 *   shell            - Default shell to use, or NULL for no shell
 *   enabled_features - Array of feature names to enable (from CLI +F)
 *   enabled_count    - Number of enabled features
 *   disabled_features- Array of feature names to disable (from CLI -F)
 *   disabled_count   - Number of disabled features
 *
 * Returns:
 *   true on success, false on error (check has_error() / print_error())
 */
bool execute(AST* ast, const char* label, bool dry_run, const char* shell,
             const char** enabled_features, size_t enabled_count,
             const char** disabled_features, size_t disabled_count) {
    if (!ast) {
        set_error(ERROR_RUNTIME, "NULL AST", 0);
        return false;
    }
    
    ExecContext ctx;
    ctx_init(&ctx, ast, dry_run, shell);
    
    vars_init();
    features_init();
    
    for (size_t i = 0; i < enabled_count; i++) {
        feature_enable(enabled_features[i]);
    }
    
    for (size_t i = 0; i < disabled_count; i++) {
        feature_disable(disabled_features[i]);
    }
    
    if (!register_all_labels(&ctx)) {
        ctx_free(&ctx);
        vars_free();
        features_free();
        return false;
    }
    
    bool success;
    
    if (label) {
        success = exec_label(&ctx, label, 0);
    } else {
        success = exec_top_level(&ctx);
    }
    
    ctx_free(&ctx);
    
    return success;
}

bool execute_and_cleanup(AST* ast, const char* label, bool dry_run, const char* shell,
                         const char** enabled_features, size_t enabled_count,
                         const char** disabled_features, size_t disabled_count) {
    bool result = execute(ast, label, dry_run, shell, 
                          enabled_features, enabled_count,
                          disabled_features, disabled_count);
    vars_free();
    features_free();
    return result;
}

