/*
 * vars.c - Variable storage and string interpolation for Mewo
 * 
 * Features:
 *   - Global variable storage with dynamic typing (number, string, bool, array)
 *   - String interpolation with ${var} syntax
 *   - Escape sequence $${} for literal ${
 *   - Nested interpolation ${${varname}}
 *   - Type coercion to string for interpolation
 */

/* Note: This file is included from main.c which provides:
 *   - stdio.h, stdlib.h, string.h, stdbool.h
 *   - str_dup() from error.c
 *   - nob.h utilities
 */

#include <math.h>

#ifdef _WIN32
  #define popen _popen
  #define pclose _pclose
#endif

typedef enum {
    VAR_NUMBER,
    VAR_STRING,
    VAR_BOOL,
    VAR_ARRAY,
} VariableType;

typedef struct Variable Variable;

struct Variable {
    VariableType type;
    union {
        double number_value;
        char* string_value;
        bool bool_value;
        struct {
            Variable** items;
            size_t count;
            size_t capacity;
        } array_value;
    };
};

typedef struct {
    char** keys;
    Variable** values;
    size_t count;
    size_t capacity;
} Variables;

static struct {
    char** items;
    size_t count;
} g_argv = {0};

static int g_last_exit_code = 0;

static char* g_global_shell = NULL;

void set_last_exit_code(int code) {
    g_last_exit_code = code;
}

int get_last_exit_code(void) {
    return g_last_exit_code;
}

void set_global_shell(const char* shell) {
    free(g_global_shell);
    g_global_shell = shell ? str_dup(shell) : NULL;
}

const char* get_global_shell(void) {
    return g_global_shell;
}

void argv_init(char** args, size_t count) {
    g_argv.items = args;
    g_argv.count = count;
}

size_t argv_count(void) {
    return g_argv.count;
}

const char* argv_get(size_t index) {
    if (index >= g_argv.count) return NULL;
    return g_argv.items[index];
}

typedef struct {
    char** names;
    size_t count;
    size_t capacity;
} Features;

static Features g_features = {0};

void features_init(void) {
    g_features.names = NULL;
    g_features.count = 0;
    g_features.capacity = 0;
}

void features_free(void) {
    for (size_t i = 0; i < g_features.count; i++) {
        free(g_features.names[i]);
    }
    free(g_features.names);
    g_features.names = NULL;
    g_features.count = 0;
    g_features.capacity = 0;
}

bool feature_exists(const char* name) {
    for (size_t i = 0; i < g_features.count; i++) {
        if (strcmp(g_features.names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool feature_enable(const char* name) {
    if (feature_exists(name)) return true;
    
    if (g_features.count >= g_features.capacity) {
        size_t new_cap = g_features.capacity == 0 ? 8 : g_features.capacity * 2;
        char** new_names = realloc(g_features.names, new_cap * sizeof(char*));
        if (!new_names) return false;
        g_features.names = new_names;
        g_features.capacity = new_cap;
    }
    
    g_features.names[g_features.count] = str_dup(name);
    if (!g_features.names[g_features.count]) return false;
    g_features.count++;
    return true;
}

bool feature_disable(const char* name) {
    for (size_t i = 0; i < g_features.count; i++) {
        if (strcmp(g_features.names[i], name) == 0) {
            free(g_features.names[i]);
            for (size_t j = i; j < g_features.count - 1; j++) {
                g_features.names[j] = g_features.names[j + 1];
            }
            g_features.count--;
            return true;
        }
    }
    return false;
}

void features_print_all(FILE* stream) {
    fprintf(stream, "Features (%zu):\n", g_features.count);
    for (size_t i = 0; i < g_features.count; i++) {
        fprintf(stream, "  %s\n", g_features.names[i]);
    }
}

static char* var_to_string(const Variable* var);
static void var_free(Variable* var);
static Variable* var_clone(const Variable* var);

static Variables g_variables = {0};
static bool g_vars_initialized = false;

void vars_init(void) {
    if (g_vars_initialized) return;
    g_vars_initialized = true;
    g_variables.keys = NULL;
    g_variables.values = NULL;
    g_variables.count = 0;
    g_variables.capacity = 0;
}

void vars_free(void) {
    for (size_t i = 0; i < g_variables.count; i++) {
        free(g_variables.keys[i]);
        var_free(g_variables.values[i]);
    }
    free(g_variables.keys);
    free(g_variables.values);
    g_variables.keys = NULL;
    g_variables.values = NULL;
    g_variables.count = 0;
    g_variables.capacity = 0;
}

static int vars_find_index(const char* name) {
    for (size_t i = 0; i < g_variables.count; i++) {
        if (strcmp(g_variables.keys[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

Variable* vars_get(const char* name) {
    int idx = vars_find_index(name);
    if (idx < 0) return NULL;
    return g_variables.values[idx];
}

bool vars_exists(const char* name) {
    return vars_find_index(name) >= 0;
}

bool vars_set(const char* name, Variable* value) {
    if (!name || !value) return false;
    
    int idx = vars_find_index(name);
    if (idx >= 0) {
        var_free(g_variables.values[idx]);
        g_variables.values[idx] = value;
        return true;
    }
    
    if (g_variables.count >= g_variables.capacity) {
        size_t new_cap = g_variables.capacity == 0 ? 8 : g_variables.capacity * 2;
        char** new_keys = realloc(g_variables.keys, new_cap * sizeof(char*));
        Variable** new_vals = realloc(g_variables.values, new_cap * sizeof(Variable*));
        if (!new_keys || !new_vals) {
            free(new_keys);
            free(new_vals);
            return false;
        }
        g_variables.keys = new_keys;
        g_variables.values = new_vals;
        g_variables.capacity = new_cap;
    }
    
    g_variables.keys[g_variables.count] = str_dup(name);
    if (!g_variables.keys[g_variables.count]) {
        return false;
    }
    g_variables.values[g_variables.count] = value;
    g_variables.count++;
    return true;
}

bool vars_delete(const char* name) {
    int idx = vars_find_index(name);
    if (idx < 0) return false;
    
    free(g_variables.keys[idx]);
    var_free(g_variables.values[idx]);
    
    for (size_t i = idx; i < g_variables.count - 1; i++) {
        g_variables.keys[i] = g_variables.keys[i + 1];
        g_variables.values[i] = g_variables.values[i + 1];
    }
    g_variables.count--;
    return true;
}

Variable* var_new_number(double value) {
    Variable* var = malloc(sizeof(Variable));
    if (!var) return NULL;
    var->type = VAR_NUMBER;
    var->number_value = value;
    return var;
}

Variable* var_new_string(const char* value) {
    Variable* var = malloc(sizeof(Variable));
    if (!var) return NULL;
    var->type = VAR_STRING;
    var->string_value = str_dup(value);
    if (!var->string_value) {
        free(var);
        return NULL;
    }
    return var;
}

Variable* var_new_bool(bool value) {
    Variable* var = malloc(sizeof(Variable));
    if (!var) return NULL;
    var->type = VAR_BOOL;
    var->bool_value = value;
    return var;
}

Variable* var_new_array(void) {
    Variable* var = malloc(sizeof(Variable));
    if (!var) return NULL;
    var->type = VAR_ARRAY;
    var->array_value.items = NULL;
    var->array_value.count = 0;
    var->array_value.capacity = 0;
    return var;
}

bool var_array_push(Variable* arr, const Variable* item) {
    if (!arr || arr->type != VAR_ARRAY || !item) return false;
    
    if (arr->array_value.count >= arr->array_value.capacity) {
        size_t new_cap = arr->array_value.capacity == 0 ? 4 : arr->array_value.capacity * 2;
        Variable** new_items = realloc(arr->array_value.items, new_cap * sizeof(Variable*));
        if (!new_items) return false;
        arr->array_value.items = new_items;
        arr->array_value.capacity = new_cap;
    }
    
    Variable* cloned = var_clone(item);
    if (!cloned) return false;
    
    arr->array_value.items[arr->array_value.count++] = cloned;
    return true;
}

size_t var_array_len(const Variable* arr) {
    if (!arr || arr->type != VAR_ARRAY) return 0;
    return arr->array_value.count;
}

Variable* var_array_get(const Variable* arr, size_t index) {
    if (!arr || arr->type != VAR_ARRAY) return NULL;
    if (index >= arr->array_value.count) return NULL;
    return arr->array_value.items[index];
}

static void var_free(Variable* var) {
    if (!var) return;
    
    switch (var->type) {
        case VAR_STRING:
            free(var->string_value);
            break;
        case VAR_ARRAY:
            for (size_t i = 0; i < var->array_value.count; i++) {
                var_free(var->array_value.items[i]);
            }
            free(var->array_value.items);
            break;
        default:
            break;
    }
    free(var);
}

static Variable* var_clone(const Variable* var) {
    if (!var) return NULL;
    
    switch (var->type) {
        case VAR_NUMBER:
            return var_new_number(var->number_value);
        case VAR_STRING:
            return var_new_string(var->string_value);
        case VAR_BOOL:
            return var_new_bool(var->bool_value);
        case VAR_ARRAY: {
            Variable* arr = var_new_array();
            if (!arr) return NULL;
            for (size_t i = 0; i < var->array_value.count; i++) {
                if (!var_array_push(arr, var->array_value.items[i])) {
                    var_free(arr);
                    return NULL;
                }
            }
            return arr;
        }
    }
    return NULL;
}

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} InterpBuilder;

static void ib_init(InterpBuilder* ib) {
    ib->data = NULL;
    ib->len = 0;
    ib->cap = 0;
}

static void ib_free(InterpBuilder* ib) {
    free(ib->data);
    ib->data = NULL;
    ib->len = 0;
    ib->cap = 0;
}

static bool ib_ensure(InterpBuilder* ib, size_t additional) {
    size_t needed = ib->len + additional + 1;
    if (needed <= ib->cap) return true;
    
    size_t new_cap = ib->cap == 0 ? 64 : ib->cap;
    while (new_cap < needed) new_cap *= 2;
    
    char* new_data = realloc(ib->data, new_cap);
    if (!new_data) return false;
    
    ib->data = new_data;
    ib->cap = new_cap;
    return true;
}

static bool ib_append_char(InterpBuilder* ib, char c) {
    if (!ib_ensure(ib, 1)) return false;
    ib->data[ib->len++] = c;
    ib->data[ib->len] = '\0';
    return true;
}

static bool ib_append_str(InterpBuilder* ib, const char* s) {
    if (!s) return true;
    size_t len = strlen(s);
    if (!ib_ensure(ib, len)) return false;
    memcpy(ib->data + ib->len, s, len);
    ib->len += len;
    ib->data[ib->len] = '\0';
    return true;
}

static bool ib_append_strn(InterpBuilder* ib, const char* s, size_t n) {
    if (!s || n == 0) return true;
    if (!ib_ensure(ib, n)) return false;
    memcpy(ib->data + ib->len, s, n);
    ib->len += n;
    ib->data[ib->len] = '\0';
    return true;
}

static char* ib_take(InterpBuilder* ib) {
    char* result = ib->data;
    if (!result) {
        result = str_dup("");
    }
    ib->data = NULL;
    ib->len = 0;
    ib->cap = 0;
    return result;
}

static char* var_to_string(const Variable* var) {
    if (!var) return str_dup("");
    
    switch (var->type) {
        case VAR_NUMBER: {
            char buf[64];
            if (floor(var->number_value) == var->number_value && 
                fabs(var->number_value) < 1e15) {
                snprintf(buf, sizeof(buf), "%.0f", var->number_value);
            } else {
                snprintf(buf, sizeof(buf), "%g", var->number_value);
            }
            return str_dup(buf);
        }
        case VAR_STRING:
            return str_dup(var->string_value ? var->string_value : "");
        case VAR_BOOL:
            return str_dup(var->bool_value ? "true" : "false");
        case VAR_ARRAY: {
            if (var->array_value.count == 0) return str_dup("");
            
            InterpBuilder ib;
            ib_init(&ib);
            
            for (size_t i = 0; i < var->array_value.count; i++) {
                if (i > 0 && !ib_append_char(&ib, ',')) {
                    ib_free(&ib);
                    return str_dup("");
                }
                char* part = var_to_string(var->array_value.items[i]);
                if (!part) {
                    ib_free(&ib);
                    return str_dup("");
                }
                bool ok = ib_append_str(&ib, part);
                free(part);
                if (!ok) {
                    ib_free(&ib);
                    return str_dup("");
                }
            }
            return ib_take(&ib);
        }
    }
    return str_dup("");
}

static char* interp_internal(const char* input, size_t line_number, bool* error);

static const char* extract_var_expr(const char* start, char** out_expr) {
    int brace_depth = 1;
    const char* p = start;
    
    while (*p && brace_depth > 0) {
        if (*p == '{') {
            brace_depth++;
        } else if (*p == '}') {
            brace_depth--;
        }
        if (brace_depth > 0) p++;
    }
    
    if (brace_depth != 0) {
        *out_expr = NULL;
        return NULL;
    }
    
    size_t len = p - start;
    *out_expr = malloc(len + 1);
    if (!*out_expr) return NULL;
    memcpy(*out_expr, start, len);
    (*out_expr)[len] = '\0';
    
    return p + 1;
}

static bool is_valid_identifier(const char* s) {
    if (!s || !*s) return false;
    
    if (!((*s >= 'a' && *s <= 'z') || 
          (*s >= 'A' && *s <= 'Z') || 
          *s == '_')) {
        return false;
    }
    
    for (const char* p = s + 1; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || 
              (*p >= 'A' && *p <= 'Z') || 
              (*p >= '0' && *p <= '9') || 
              *p == '_')) {
            return false;
        }
    }
    return true;
}

static char* interp_internal(const char* input, size_t line_number, bool* error) {
    if (!input) {
        *error = false;
        return str_dup("");
    }
    
    InterpBuilder ib;
    ib_init(&ib);
    
    const char* p = input;
    while (*p) {
        if (p[0] == '$' && p[1] == '$' && p[2] == '{') {
            if (!ib_append_str(&ib, "${")) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            p += 3;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                if (depth > 0) {
                    if (!ib_append_char(&ib, *p)) {
                        set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                        ib_free(&ib);
                        *error = true;
                        return NULL;
                    }
                }
                p++;
            }
            if (!ib_append_char(&ib, '}')) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            continue;
        }
        
        if (p[0] == '$' && p[1] == '$' && p[2] >= '0' && p[2] <= '9') {
            if (!ib_append_str(&ib, "$")) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            p += 2;
            if (!ib_append_char(&ib, *p)) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            p++;
            continue;
        }

        if (p[0] == '$' && p[1] >= '0' && p[1] <= '9') {
            p++;
            size_t idx = 0;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (*p - '0');
                p++;
            }
            const char* arg = argv_get(idx);
            if (arg) {
                if (!ib_append_str(&ib, arg)) {
                    set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
            }
            continue;
        }
        
        if (p[0] == '$' && p[1] == '?') {
            p += 2;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", get_last_exit_code());
            if (!ib_append_str(&ib, buf)) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            continue;
        }
        
        if (p[0] == '$' && p[1] == '{') {
            p += 2;
            
            char* expr = NULL;
            const char* after = extract_var_expr(p, &expr);
            
            if (!after || !expr) {
                set_error(ERROR_SYNTAX, "Unterminated ${} expression", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            bool inner_error = false;
            char* interpolated_expr = interp_internal(expr, line_number, &inner_error);
            free(expr);
            
            if (inner_error) {
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            bool is_numeric = true;
            for (const char* c = interpolated_expr; *c; c++) {
                if (*c < '0' || *c > '9') {
                    is_numeric = false;
                    break;
                }
            }
            
            if (is_numeric && *interpolated_expr) {
                size_t idx = (size_t)atoll(interpolated_expr);
                const char* arg = argv_get(idx);
                free(interpolated_expr);
                if (arg) {
                    if (!ib_append_str(&ib, arg)) {
                        set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                        ib_free(&ib);
                        *error = true;
                        return NULL;
                    }
                }
                p = after;
                continue;
            }
            
            if (strcmp(interpolated_expr, "argv") == 0) {
                free(interpolated_expr);
                for (size_t i = 0; i < argv_count(); i++) {
                    if (i > 0) {
                        if (!ib_append_char(&ib, ' ')) {
                            set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                            ib_free(&ib);
                            *error = true;
                            return NULL;
                        }
                    }
                    if (!ib_append_str(&ib, argv_get(i))) {
                        set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                        ib_free(&ib);
                        *error = true;
                        return NULL;
                    }
                }
                p = after;
                continue;
            }
            
            if (strncmp(interpolated_expr, "#len(", 5) == 0 && 
                interpolated_expr[strlen(interpolated_expr) - 1] == ')') {
                size_t param_len = strlen(interpolated_expr) - 6;
                char* param = malloc(param_len + 1);
                if (!param) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(interpolated_expr);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                memcpy(param, interpolated_expr + 5, param_len);
                param[param_len] = '\0';
                free(interpolated_expr);
                
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
                
                char buf[32];
                snprintf(buf, sizeof(buf), "%zu", len);
                if (!ib_append_str(&ib, buf)) {
                    set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                p = after;
                continue;
            }
            
            if (strncmp(interpolated_expr, "#env(", 5) == 0 && 
                interpolated_expr[strlen(interpolated_expr) - 1] == ')') {
                size_t content_len = strlen(interpolated_expr) - 6;
                char* content = malloc(content_len + 1);
                if (!content) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(interpolated_expr);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                memcpy(content, interpolated_expr + 5, content_len);
                content[content_len] = '\0';
                free(interpolated_expr);
                
                char* comma = strchr(content, ',');
                char* env_name = NULL;
                char* env_default = NULL;
                
                if (comma) {
                    size_t name_len = comma - content;
                    env_name = malloc(name_len + 1);
                    if (env_name) {
                        memcpy(env_name, content, name_len);
                        env_name[name_len] = '\0';
                        char* n = env_name;
                        while (*n == ' ' || *n == '\t') n++;
                        memmove(env_name, n, strlen(n) + 1);
                        size_t nl = strlen(env_name);
                        while (nl > 0 && (env_name[nl-1] == ' ' || env_name[nl-1] == '\t')) env_name[--nl] = '\0';
                    }
                    const char* def_start = comma + 1;
                    while (*def_start == ' ' || *def_start == '\t') def_start++;
                    env_default = str_dup(def_start);
                    if (env_default) {
                        size_t dl = strlen(env_default);
                        while (dl > 0 && (env_default[dl-1] == ' ' || env_default[dl-1] == '\t')) env_default[--dl] = '\0';
                    }
                } else {
                    env_name = str_dup(content);
                    if (env_name) {
                        char* n = env_name;
                        while (*n == ' ' || *n == '\t') n++;
                        memmove(env_name, n, strlen(n) + 1);
                        size_t nl = strlen(env_name);
                        while (nl > 0 && (env_name[nl-1] == ' ' || env_name[nl-1] == '\t')) env_name[--nl] = '\0';
                    }
                    env_default = str_dup("");
                }
                free(content);
                
                if (!env_name) {
                    free(env_default);
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                
                const char* env_value = getenv(env_name);
                const char* result_value = env_value ? env_value : (env_default ? env_default : "");
                
                if (!ib_append_str(&ib, result_value)) {
                    set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                    free(env_name);
                    free(env_default);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                free(env_name);
                free(env_default);
                p = after;
                continue;
            }
            
            if (strncmp(interpolated_expr, "#exec(", 6) == 0 &&
                interpolated_expr[strlen(interpolated_expr) - 1] == ')') {

                size_t content_len = strlen(interpolated_expr) - 7;
                char* content = malloc(content_len + 1);
                if (!content) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(interpolated_expr);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                memcpy(content, interpolated_expr + 6, content_len);
                content[content_len] = '\0';
                free(interpolated_expr);

                char* cmd = NULL;
                char* shell = NULL;

                char* content_ptr = content;
                while (*content_ptr == ' ' || *content_ptr == '\t') content_ptr++;
                if (*content_ptr != '"') {
                    set_error(ERROR_SYNTAX, "Expected quoted command in #exec()", line_number);
                    free(content);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                content_ptr++;
                char* cmd_start = content_ptr;
                size_t cmd_len = 0;
                int escaped = 0;
                while (*content_ptr) {
                    if (escaped) {
                        escaped = 0;
                        content_ptr++;
                        cmd_len++;
                        continue;
                    }
                    if (*content_ptr == '\\') {
                        escaped = 1;
                    } else if (*content_ptr == '"') {
                        break;
                    }
                    content_ptr++;
                    cmd_len++;
                }
                if (*content_ptr != '"') {
                    set_error(ERROR_SYNTAX, "Unterminated quoted command in #exec()", line_number);
                    free(content);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }

                cmd = malloc(cmd_len + 1);
                if (!cmd) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(content);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }

                char* dst = cmd;
                char* src = cmd_start;
                char* cmd_end = cmd_start + cmd_len;
                while (src < cmd_end) {
                    if (*src == '\\' && src + 1 < cmd_end) {
                        src++;
                        *dst++ = *src++;
                    } else {
                        *dst++ = *src++;
                    }
                }
                *dst = '\0';

                content_ptr++;
                while (*content_ptr == ' ' || *content_ptr == '\t') content_ptr++;

                if (*content_ptr == ',') {
                    content_ptr++;
                    while (*content_ptr == ' ' || *content_ptr == '\t') content_ptr++;
                    shell = content_ptr;
                    size_t l = strlen(shell);
                    while (l > 0 && (shell[l-1] == ' ' || shell[l-1] == '\t')) shell[--l] = '\0';
                    if (*shell == '\0') shell = NULL;
                } else {
                    shell = NULL;
                }

                char output_buf[1024] = {0};
                size_t out_len = 0;

                FILE* fp = NULL;
                if (shell) {
                    char* full_cmd = malloc(strlen(shell) + strlen(cmd) + 6);
                    if (!full_cmd) {
                        set_error(ERROR_MEMORY, "Out of memory", line_number);
                        free(cmd);
                        free(content);
                        ib_free(&ib);
                        *error = true;
                        return NULL;
                    }
                    sprintf(full_cmd, "%s -c \"%s\"", shell, cmd);
                    fp = popen(full_cmd, "r");
                    free(full_cmd);
                } else {
                    fp = popen(cmd, "r");
                }

                if (!fp) {
                    set_error(ERROR_RUNTIME, "Failed to execute command", line_number);
                    free(cmd);
                    free(content);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }

                while (fgets(output_buf + out_len, sizeof(output_buf) - out_len, fp)) {
                    out_len = strlen(output_buf);
                    if (out_len >= sizeof(output_buf) - 1) break;
                }

                pclose(fp);

                if (out_len > 0 && output_buf[out_len - 1] == '\n') output_buf[out_len - 1] = '\0';

                if (!ib_append_str(&ib, output_buf)) {
                    set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                    free(cmd);
                    free(content);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }

                free(cmd);
                free(content);
                p = after;
                continue;
            }

            char* bracket = strchr(interpolated_expr, '[');
            if (bracket && interpolated_expr[strlen(interpolated_expr) - 1] == ']') {
                size_t name_len = bracket - interpolated_expr;
                char* var_name = malloc(name_len + 1);
                if (!var_name) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(interpolated_expr);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                memcpy(var_name, interpolated_expr, name_len);
                var_name[name_len] = '\0';
                
                size_t idx_len = strlen(interpolated_expr) - name_len - 2;
                char* idx_str = malloc(idx_len + 1);
                if (!idx_str) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    free(var_name);
                    free(interpolated_expr);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                memcpy(idx_str, bracket + 1, idx_len);
                idx_str[idx_len] = '\0';
                free(interpolated_expr);
                
                size_t idx = (size_t)atoll(idx_str);
                free(idx_str);
                
                Variable* var = vars_get(var_name);
                if (!var) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "Undefined variable: '%s'", var_name);
                    set_error(ERROR_RUNTIME, err_msg, line_number);
                    free(var_name);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                free(var_name);
                
                char* value_str = NULL;
                if (var->type == VAR_ARRAY) {
                    if (idx < var->array_value.count) {
                        value_str = var_to_string(var->array_value.items[idx]);
                    } else {
                        value_str = str_dup("");
                    }
                } else if (var->type == VAR_STRING) {
                    if (var->string_value && idx < strlen(var->string_value)) {
                        char buf[2] = {var->string_value[idx], '\0'};
                        value_str = str_dup(buf);
                    } else {
                        value_str = str_dup("");
                    }
                } else {
                    value_str = var_to_string(var);
                }
                
                if (!value_str) {
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                
                if (!ib_append_str(&ib, value_str)) {
                    set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                    free(value_str);
                    ib_free(&ib);
                    *error = true;
                    return NULL;
                }
                free(value_str);
                p = after;
                continue;
            }
            
            if (!is_valid_identifier(interpolated_expr)) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Invalid variable name: '%s'", interpolated_expr);
                set_error(ERROR_SYNTAX, err_msg, line_number);
                free(interpolated_expr);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            Variable* var = vars_get(interpolated_expr);
            if (!var) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Undefined variable: '%s'", interpolated_expr);
                set_error(ERROR_RUNTIME, err_msg, line_number);
                free(interpolated_expr);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            free(interpolated_expr);
            
            char* value_str = var_to_string(var);
            if (!value_str) {
                set_error(ERROR_MEMORY, "Out of memory converting variable to string", line_number);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            if (!ib_append_str(&ib, value_str)) {
                set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
                free(value_str);
                ib_free(&ib);
                *error = true;
                return NULL;
            }
            
            free(value_str);
            p = after;
            continue;
        }
        
        if (!ib_append_char(&ib, *p)) {
            set_error(ERROR_MEMORY, "Out of memory during interpolation", line_number);
            ib_free(&ib);
            *error = true;
            return NULL;
        }
        p++;
    }
    
    *error = false;
    return ib_take(&ib);
}

char* interpolate(const char* input, size_t line_number) {
    bool error = false;
    return interp_internal(input, line_number, &error);
}

static const char* skip_ws(const char* s) {
    while (*s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

Variable* parse_value(const char* value_str, size_t line_number) {
    if (!value_str) return NULL;
    
    const char* p = skip_ws(value_str);
    
    if (!*p) {
        return var_new_string("");
    }
    
    {
        const char* scan = p;
        bool in_string = false;
        char string_char = 0;
        int bracket_depth = 0;
        bool has_comma = false;
        
        while (*scan) {
            if (!in_string) {
                if (*scan == '"' || *scan == '\'') {
                    in_string = true;
                    string_char = *scan;
                } else if (*scan == '[') {
                    bracket_depth++;
                } else if (*scan == ']') {
                    bracket_depth--;
                } else if (*scan == ',' && bracket_depth == 0) {
                    has_comma = true;
                    break;
                }
            } else {
                if (*scan == string_char) {
                    in_string = false;
                }
            }
            scan++;
        }
        
        if (has_comma) {
            size_t len = strlen(p);
            char* wrapped = malloc(len + 3);
            if (!wrapped) {
                set_error(ERROR_MEMORY, "Out of memory", line_number);
                return NULL;
            }
            wrapped[0] = '[';
            memcpy(wrapped + 1, p, len);
            wrapped[len + 1] = ']';
            wrapped[len + 2] = '\0';
            
            Variable* result = parse_value(wrapped, line_number);
            free(wrapped);
            return result;
        }
    }
    
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        const char* start = p;
        while (*p && *p != quote) p++;
        
        if (*p != quote) {
            set_error(ERROR_SYNTAX, "Unterminated string literal", line_number);
            return NULL;
        }
        
        size_t len = p - start;
        char* str_val = malloc(len + 1);
        if (!str_val) {
            set_error(ERROR_MEMORY, "Out of memory", line_number);
            return NULL;
        }
        memcpy(str_val, start, len);
        str_val[len] = '\0';
        
        Variable* var = var_new_string(str_val);
        free(str_val);
        return var;
    }
    
    if (strncmp(p, "true", 4) == 0 && 
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == ',' || p[4] == ']')) {
        return var_new_bool(true);
    }
    
    if (strncmp(p, "false", 5) == 0 && 
        (p[5] == '\0' || p[5] == ' ' || p[5] == '\t' || p[5] == ',' || p[5] == ']')) {
        return var_new_bool(false);
    }
    
    if (*p == '[') {
        p++;
        Variable* arr = var_new_array();
        if (!arr) {
            set_error(ERROR_MEMORY, "Out of memory", line_number);
            return NULL;
        }
        
        p = skip_ws(p);
        while (*p && *p != ']') {
            const char* elem_start = p;
            int depth = 0;
            bool in_string = false;
            char string_char = 0;
            
            while (*p) {
                if (!in_string) {
                    if (*p == '"' || *p == '\'') {
                        in_string = true;
                        string_char = *p;
                    } else if (*p == '[') {
                        depth++;
                    } else if (*p == ']') {
                        if (depth == 0) break;
                        depth--;
                    } else if (*p == ',' && depth == 0) {
                        break;
                    }
                } else {
                    if (*p == string_char) {
                        in_string = false;
                    }
                }
                p++;
            }
            
            size_t elem_len = p - elem_start;
            char* elem_str = malloc(elem_len + 1);
            if (!elem_str) {
                var_free(arr);
                set_error(ERROR_MEMORY, "Out of memory", line_number);
                return NULL;
            }
            memcpy(elem_str, elem_start, elem_len);
            elem_str[elem_len] = '\0';
            
            char* trimmed = (char*)skip_ws(elem_str);
            size_t trim_len = strlen(trimmed);
            while (trim_len > 0 && (trimmed[trim_len-1] == ' ' || trimmed[trim_len-1] == '\t')) {
                trimmed[--trim_len] = '\0';
            }
            
            if (trim_len > 0) {
                Variable* elem = parse_value(trimmed, line_number);
                free(elem_str);
                
                if (!elem) {
                    var_free(arr);
                    return NULL;
                }
                
                if (!var_array_push(arr, elem)) {
                    var_free(elem);
                    var_free(arr);
                    set_error(ERROR_MEMORY, "Out of memory", line_number);
                    return NULL;
                }
                var_free(elem);
            } else {
                free(elem_str);
            }
            
            if (*p == ',') p++;
            p = skip_ws(p);
        }
        
        if (*p != ']') {
            var_free(arr);
            set_error(ERROR_SYNTAX, "Unterminated array literal", line_number);
            return NULL;
        }
        
        return arr;
    }
    
    {
        const char* num_start = p;
        bool has_dot = false;
        bool has_digit = false;
        
        if (*p == '-' || *p == '+') p++;
        
        while (*p) {
            if (*p >= '0' && *p <= '9') {
                has_digit = true;
                p++;
            } else if (*p == '.' && !has_dot) {
                has_dot = true;
                p++;
            } else {
                break;
            }
        }
        
        const char* after_num = skip_ws(p);
        if (has_digit && (*after_num == '\0' || *after_num == ',' || *after_num == ']')) {
            char* num_str = malloc(p - num_start + 1);
            if (!num_str) {
                set_error(ERROR_MEMORY, "Out of memory", line_number);
                return NULL;
            }
            memcpy(num_str, num_start, p - num_start);
            num_str[p - num_start] = '\0';
            
            double num = strtod(num_str, NULL);
            free(num_str);
            return var_new_number(num);
        }
    }
    
    {
        const char* id_start = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || 
                      (*p >= 'A' && *p <= 'Z') || 
                      (*p >= '0' && *p <= '9') || 
                      *p == '_')) {
            p++;
        }
        
        const char* after_id = skip_ws(p);
        if (p > id_start && (*after_id == '\0' || *after_id == ',' || *after_id == ']')) {
            size_t id_len = p - id_start;
            char* id_name = malloc(id_len + 1);
            if (!id_name) {
                set_error(ERROR_MEMORY, "Out of memory", line_number);
                return NULL;
            }
            memcpy(id_name, id_start, id_len);
            id_name[id_len] = '\0';
            
            Variable* ref = vars_get(id_name);
            if (!ref) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Undefined variable: '%s'", id_name);
                set_error(ERROR_RUNTIME, err_msg, line_number);
                free(id_name);
                return NULL;
            }
            
            free(id_name);
            return var_clone(ref);
        }
    }
    
    set_error(ERROR_SYNTAX, "Invalid value", line_number);
    return NULL;
}

bool vars_set_number(const char* name, double value) {
    Variable* var = var_new_number(value);
    if (!var) return false;
    return vars_set(name, var);
}

bool vars_set_string(const char* name, const char* value) {
    Variable* var = var_new_string(value);
    if (!var) return false;
    return vars_set(name, var);
}

bool vars_set_bool(const char* name, bool value) {
    Variable* var = var_new_bool(value);
    if (!var) return false;
    return vars_set(name, var);
}

void vars_print_all(FILE* stream) {
    fprintf(stream, "Variables (%zu):\n", g_variables.count);
    for (size_t i = 0; i < g_variables.count; i++) {
        char* val_str = var_to_string(g_variables.values[i]);
        const char* type_str = "unknown";
        switch (g_variables.values[i]->type) {
            case VAR_NUMBER: type_str = "number"; break;
            case VAR_STRING: type_str = "string"; break;
            case VAR_BOOL: type_str = "bool"; break;
            case VAR_ARRAY: type_str = "array"; break;
        }
        fprintf(stream, "  %s = %s (%s)\n", g_variables.keys[i], val_str, type_str);
        free(val_str);
    }
}

const char* var_type_name(const Variable* var) {
    if (!var) return "null";
    switch (var->type) {
        case VAR_NUMBER: return "number";
        case VAR_STRING: return "string";
        case VAR_BOOL: return "bool";
        case VAR_ARRAY: return "array";
    }
    return "unknown";
}
