/*
 * parser.c - Line-based AST parser for Mewo
 * 
 * Features:
 *   - Parse Mewofile into Abstract Syntax Tree (AST)
 *   - Statement types: variables, labels, commands, conditionals, control flow
 *   - Attribute parsing (#cwd, #ignorefail, #shell, #feature, etc.)
 *   - Index access/assign syntax (arr[idx], arr[idx] = value)
 *   - Comment stripping (;) and indent tracking
 *   - Proper handling of quoted strings with special characters
 */

/* Note: This file is included from main.c which provides:
 *   - stdio.h, stdlib.h, string.h, stdbool.h, ctype.h
 *   - str_dup() from error.c
 *   - set_error(), has_error() from error.c
 */

#include <ctype.h>

typedef enum {
    STMT_ATTR,
    STMT_VAR_ASSIGN,
    STMT_INDEX_ACCESS,
    STMT_INDEX_ASSIGN,
    STMT_LABEL,
    STMT_COMMAND,
    STMT_IF,
    STMT_ELSE,
    STMT_ENDIF,
    STMT_GOTO,
    STMT_CALL,
} StmtType;

typedef struct Stmt Stmt;

struct Stmt {
    StmtType type;
    int indent_level;
    size_t line_number;
    union {
        struct {
            char* name;
            Stmt* parameters[3];
            int param_count;
        } attr;
        struct {
            char* name;
            char* value;
        } var_assign;
        struct {
            char* name;
            char* index;
        } index_access;
        struct {
            char* name;
            char* index;
            char* value;
        } index_assign;
        struct {
            char* name;
        } label;
        struct {
            char* raw_line;
        } command;
        struct {
            char* condition;
        } if_stmt;
        struct {
            char* target;
        } goto_stmt;
        struct {
            char* target;
        } call_stmt;
    };
};

typedef struct {
    Stmt** stmts;
    size_t stmts_count;
    size_t stmts_capacity;
} AST;

static char* str_trim(const char* s) {
    while (*s && isspace(*s)) s++;
    const char* end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) end--;
    size_t len = end - s + 1;
    char* result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

static bool ends_with_continuation(const char *s) {
    size_t len = strlen(s);
    if (len == 0) return false;
    if (s[len - 1] != '\\') return false;
    if (len >= 2 && s[len - 2] == '\\') return false;
    return true;
}

static int count_indent(const char* line) {
    int count = 0;
    while (line[count] == ' ' || line[count] == '\t') {
        count += (line[count] == '\t') ? 4 : 1;
    }
    return count / 4;
}

static bool is_empty_or_comment(const char* line) {
    while (*line && isspace(*line)) line++;
    return *line == '\0' || *line == ';' || (*line == '/' && *(line + 1) == '/');
}

static char* strip_comment(const char* line) {
    bool in_string = false;
    const char* p = line;

    while (*p) {
        if (*p == '"' && (p == line || *(p - 1) != '\\')) {
            in_string = !in_string;
        }

        if (!in_string) {
            if (*p == ';') {
                break;
            }
            if (*p == '/' && *(p + 1) == '/') {
                break;
            }
        }

        p++;
    }

    size_t len = p - line;
    char* result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, line, len);
    result[len] = '\0';
    return result;
}

static bool starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static const char* find_unquoted_char(const char* str, char c) {
    bool in_string = false;
    char string_char = 0;
    
    while (*str) {
        if (!in_string) {
            if (*str == '"' || *str == '\'') {
                in_string = true;
                string_char = *str;
            } else if (*str == c) {
                return str;
            }
        } else {
            if (*str == string_char) {
                in_string = false;
            }
        }
        str++;
    }
    return NULL;
}

bool is_single_identifier_before_eq(const char *s) {
    const char *eq = strchr(s, '=');
    if (!eq) return false;

    const char *p = eq;
    while (p > s && isspace(*(p-1))) p--;

    const char *start = p;
    while (start > s && (isalnum(*(start-1)) || *(start-1) == '_')) start--;

    for (const char *c = s; c < start; c++) {
        if (!isspace(*c)) return false;
    }

    if (start == p) return false;

    return true;
}

static Stmt* parse_attr(const char* line, size_t line_number) {
    const char* p = line;
    while (*p && isspace(*p)) p++;
    
    if (*p != '#') return NULL;
    p++;
    
    const char* name_start = p;
    while (*p && *p != '(' && !isspace(*p)) p++;
    
    size_t name_len = p - name_start;
    if (name_len == 0) {
        set_error(ERROR_SYNTAX, "Expected attribute name after '#'", line_number);
        return NULL;
    }
    
    Stmt* stmt = calloc(1, sizeof(Stmt));
    stmt->type = STMT_ATTR;
    stmt->attr.name = malloc(name_len + 1);
    memcpy(stmt->attr.name, name_start, name_len);
    stmt->attr.name[name_len] = '\0';
    
    while (*p && isspace(*p)) p++;
    if (*p == '(') {
        p++;
        stmt->attr.param_count = 0;
        
        if (strcmp(stmt->attr.name, "features") == 0) {
            const char* content_start = p;
            int paren_depth = 1;
            while (*p && paren_depth > 0) {
                if (*p == '(') paren_depth++;
                if (*p == ')') paren_depth--;
                if (paren_depth > 0) p++;
            }
            
            size_t content_len = p - content_start;
            char* content = malloc(content_len + 1);
            memcpy(content, content_start, content_len);
            content[content_len] = '\0';
            
            Stmt* param = calloc(1, sizeof(Stmt));
            param->type = STMT_COMMAND;
            param->command.raw_line = str_trim(content);
            free(content);
            
            stmt->attr.parameters[0] = param;
            stmt->attr.param_count = 1;
        } else {
            while (*p && *p != ')' && stmt->attr.param_count < 3) {
                while (*p && isspace(*p)) p++;
                
                const char* param_start = p;
                int paren_depth = 0;
                
                while (*p && !((*p == ',' || *p == ')') && paren_depth == 0)) {
                    if (*p == '(') paren_depth++;
                    if (*p == ')') paren_depth--;
                    p++;
                }
                
                if (p > param_start) {
                    char* param_str = malloc(p - param_start + 1);
                    memcpy(param_str, param_start, p - param_start);
                    param_str[p - param_start] = '\0';
                    
                    Stmt* param = calloc(1, sizeof(Stmt));
                    param->type = STMT_COMMAND;
                    param->command.raw_line = str_trim(param_str);
                    free(param_str);
                    
                    stmt->attr.parameters[stmt->attr.param_count++] = param;
                }
                
                if (*p == ',') p++;
            }
        }
    }
    
    return stmt;
}

static Stmt* parse_var_assign(const char* line, size_t line_number) {
    const char* eq = strchr(line, '=');
    if (!eq) return NULL;
    
    const char* name_start = line;
    while (*name_start && isspace(*name_start)) name_start++;
    const char* name_end = eq - 1;
    while (name_end > name_start && isspace(*name_end)) name_end--;
    
    if (name_end < name_start) {
        return NULL;
    }
    
    size_t name_len = name_end - name_start + 1;
    
    const char* bracket_open = NULL;
    const char* bracket_close = NULL;
    for (const char* p = name_start; p <= name_end; p++) {
        if (*p == '[') bracket_open = p;
        else if (*p == ']') bracket_close = p;
    }
    
    const char* check_end = bracket_open ? bracket_open : (name_end + 1);
    const char* p = name_start;
    
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
        return NULL;
    }
    p++;
    
    while (p < check_end) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || 
              (*p >= '0' && *p <= '9') || *p == '_')) {
            return NULL;
        }
        p++;
    }
    
    const char* value_start = eq + 1;
    while (*value_start && isspace(*value_start)) value_start++;
    
    if (bracket_open && bracket_close && bracket_close > bracket_open) {
        size_t var_name_len = bracket_open - name_start;
        size_t idx_len = bracket_close - bracket_open - 1;
        
        Stmt* stmt = calloc(1, sizeof(Stmt));
        stmt->type = STMT_INDEX_ASSIGN;
        stmt->index_assign.name = malloc(var_name_len + 1);
        memcpy(stmt->index_assign.name, name_start, var_name_len);
        stmt->index_assign.name[var_name_len] = '\0';
        
        stmt->index_assign.index = malloc(idx_len + 1);
        memcpy(stmt->index_assign.index, bracket_open + 1, idx_len);
        stmt->index_assign.index[idx_len] = '\0';
        
        stmt->index_assign.value = str_dup(value_start);
        return stmt;
    }
    
    Stmt* stmt = calloc(1, sizeof(Stmt));
    stmt->type = STMT_VAR_ASSIGN;
    stmt->var_assign.name = malloc(name_len + 1);
    memcpy(stmt->var_assign.name, name_start, name_len);
    stmt->var_assign.name[name_len] = '\0';
    stmt->var_assign.value = str_dup(value_start);
    
    return stmt;
}

static Stmt* parse_label(const char* line, size_t line_number) {
    const char* colon = strchr(line, ':');
    if (!colon) return NULL;
    
    const char* name_start = line;
    while (*name_start && isspace(*name_start)) name_start++;
    
    const char* name_end = colon;
    while (name_end > name_start && isspace(*(name_end - 1))) name_end--;
    
    if (name_end <= name_start) {
        set_error(ERROR_SYNTAX, "missing label name", line_number);
        return NULL;
    }
    
    size_t name_len = name_end - name_start;
    
    Stmt* stmt = calloc(1, sizeof(Stmt));
    stmt->type = STMT_LABEL;
    stmt->label.name = malloc(name_len + 1);
    memcpy(stmt->label.name, name_start, name_len);
    stmt->label.name[name_len] = '\0';
    
    return stmt;
}

static Stmt* parse_conditional(const char* line, size_t line_number) {
    const char* p = line;
    while (*p && isspace(*p)) p++;
    
    if (!starts_with(p, "#if")) return NULL;
    
    p += 3;
    while (*p && isspace(*p)) p++;
    
    if (*p != '(') {
        set_error(ERROR_SYNTAX, "Expected '(' after '#if'", line_number);
        return NULL;
    }
    p++;
    
    const char* cond_start = p;
    int paren_depth = 1;
    
    while (*p && paren_depth > 0) {
        if (*p == '(') paren_depth++;
        if (*p == ')') paren_depth--;
        if (paren_depth > 0) p++;
    }

    if (*p == ':') {
        p++;
    }
    
    size_t cond_len = p - cond_start;
    
    Stmt* stmt = calloc(1, sizeof(Stmt));
    stmt->type = STMT_IF;
    stmt->if_stmt.condition = malloc(cond_len + 1);
    memcpy(stmt->if_stmt.condition, cond_start, cond_len);
    stmt->if_stmt.condition[cond_len] = '\0';
    
    return stmt;
}

static void add_stmt(AST* ast, Stmt* stmt) {
    if (ast->stmts_count >= ast->stmts_capacity) {
        ast->stmts_capacity = ast->stmts_capacity ? ast->stmts_capacity * 2 : 16;
        ast->stmts = realloc(ast->stmts, ast->stmts_capacity * sizeof(Stmt*));
    }
    ast->stmts[ast->stmts_count++] = stmt;
}

AST* parse(const char** lines, size_t lines_count) {
    AST* ast = calloc(1, sizeof(AST));
    
    for (size_t i = 0; i < lines_count; i++) {
        const char* line = lines[i];
        
        if (is_empty_or_comment(line)) {
            continue;
        }
        
        char* line_no_comment = strip_comment(line);
        
        int indent = count_indent(line_no_comment);
        const char* trimmed = line_no_comment + (indent * 4);
        while (*trimmed && isspace(*trimmed)) trimmed++;
        
        Stmt* stmt = NULL;
        
        if (starts_with(trimmed, "#if(") || starts_with(trimmed, "#if ")) {
            stmt = parse_conditional(trimmed, i + 1);
            if (!stmt && has_error()) {
                free(line_no_comment);
                return ast;
            }
            if (stmt) {
                stmt->indent_level = indent;
                stmt->line_number = i + 1;
                add_stmt(ast, stmt);
                free(line_no_comment);
                continue;
            }
        } else if (starts_with(trimmed, "#else")) {
            stmt = calloc(1, sizeof(Stmt));
            stmt->type = STMT_ELSE;
            stmt->indent_level = indent;
            stmt->line_number = i + 1;
            add_stmt(ast, stmt);
            free(line_no_comment);
            continue;
        } else if (starts_with(trimmed, "#endif")) {
            stmt = calloc(1, sizeof(Stmt));
            stmt->type = STMT_ENDIF;
            stmt->indent_level = indent;
            stmt->line_number = i + 1;
            add_stmt(ast, stmt);
            free(line_no_comment);
            continue;
        }
        
        const char* after_attrs = trimmed;
        while (*after_attrs == '#') {
            Stmt* attr = parse_attr(after_attrs, i + 1);
            if (attr) {
                attr->indent_level = indent;
                add_stmt(ast, attr);
                
                after_attrs++;
                while (*after_attrs && *after_attrs != '(' && !isspace(*after_attrs)) after_attrs++;
                if (*after_attrs == '(') {
                    int depth = 1;
                    after_attrs++;
                    while (*after_attrs && depth > 0) {
                        if (*after_attrs == '(') depth++;
                        if (*after_attrs == ')') depth--;
                        after_attrs++;
                    }
                }
                if (*after_attrs == ':') after_attrs++;
                while (*after_attrs && isspace(*after_attrs)) after_attrs++;
            } else {
                break;
            }
        }
        
        if (*after_attrs == '\0') {
            free(line_no_comment);
            continue;
        }
        
        stmt = NULL;
        
        if (*after_attrs == '#') {
            if (starts_with(after_attrs, "#if(")) {
                stmt = parse_conditional(after_attrs, i + 1);
                if (!stmt && has_error()) {
                    free(line_no_comment);
                    return ast;
                }
            } else if (starts_with(after_attrs, "#else")) {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_ELSE;
            } else if (starts_with(after_attrs, "#endif")) {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_ENDIF;
            } else {
                set_error(ERROR_SYNTAX, "Unknown directive", i + 1);
                free(line_no_comment);
                return ast;
            }
        } else if (find_unquoted_char(after_attrs, ':') && indent == 0) {
            stmt = parse_label(after_attrs, i + 1);
            if (!stmt && has_error()) {
                free(line_no_comment);
                return ast;
            }
        } else if (find_unquoted_char(after_attrs, '=') && is_single_identifier_before_eq(after_attrs)) {
            stmt = parse_var_assign(after_attrs, i + 1);
            if (!stmt && has_error()) {
                free(line_no_comment);
                return ast;
            }
        } else if (starts_with(after_attrs, "goto ") || strcmp(after_attrs, "goto") == 0) {
            const char* target = after_attrs + 4;
            while (*target && isspace(*target)) target++;
            if (*target == '\0') {
                set_error(ERROR_SYNTAX, "Expected label name after 'goto'", i + 1);
                free(line_no_comment);
                return ast;
            }
            const char* p = target;
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_GOTO;
                stmt->goto_stmt.target = str_trim(target);
            } else {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_COMMAND;
                stmt->command.raw_line = str_dup(after_attrs);
            }
        } else if (starts_with(after_attrs, "call ") || strcmp(after_attrs, "call") == 0) {
            const char* target = after_attrs + 4;
            while (*target && isspace(*target)) target++;
            if (*target == '\0') {
                set_error(ERROR_SYNTAX, "Expected label name after 'call'", i + 1);
                free(line_no_comment);
                return ast;
            }
            const char* p = target;
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_CALL;
                stmt->call_stmt.target = str_trim(target);
            } else {
                stmt = calloc(1, sizeof(Stmt));
                stmt->type = STMT_COMMAND;
                stmt->command.raw_line = str_dup(after_attrs);
            }
        } else {
            char *acc = str_dup(after_attrs);

            while (ends_with_continuation(acc) && i + 1 < lines_count) {
                acc[strlen(acc) - 1] = '\0';

                i++;
                char *next = strip_comment(lines[i]);
                char *t = str_trim(next);

                size_t old_len = strlen(acc);
                size_t t_len = strlen(t);

                char *new_acc = malloc(old_len + 1 + t_len + 1);
                memcpy(new_acc, acc, old_len);
                new_acc[old_len] = ' ';
                memcpy(new_acc + old_len + 1, t, t_len + 1);

                free(acc);
                free(next);

                acc = new_acc;
            }

            stmt = calloc(1, sizeof(Stmt));
            stmt->type = STMT_COMMAND;
            stmt->command.raw_line = acc;
        }

        
        if (stmt) {
            stmt->indent_level = indent;
            stmt->line_number = i + 1;
            add_stmt(ast, stmt);
        }
        
        free(line_no_comment);
    }
    
    return ast;
}

void free_stmt(Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_ATTR:
            free(stmt->attr.name);
            for (int i = 0; i < stmt->attr.param_count; i++) {
                free_stmt(stmt->attr.parameters[i]);
            }
            break;
        case STMT_VAR_ASSIGN:
            free(stmt->var_assign.name);
            free(stmt->var_assign.value);
            break;
        case STMT_INDEX_ACCESS:
            free(stmt->index_access.name);
            free(stmt->index_access.index);
            break;
        case STMT_INDEX_ASSIGN:
            free(stmt->index_assign.name);
            free(stmt->index_assign.index);
            free(stmt->index_assign.value);
            break;
        case STMT_LABEL:
            free(stmt->label.name);
            break;
        case STMT_COMMAND:
            free(stmt->command.raw_line);
            break;
        case STMT_IF:
            free(stmt->if_stmt.condition);
            break;
        case STMT_GOTO:
            free(stmt->goto_stmt.target);
            break;
        case STMT_CALL:
            free(stmt->call_stmt.target);
            break;
        default:
            break;
    }
    
    free(stmt);
}

void free_ast(AST* ast) {
    if (!ast) return;
    
    for (size_t i = 0; i < ast->stmts_count; i++) {
        free_stmt(ast->stmts[i]);
    }
    
    free(ast->stmts);
    free(ast);
}

static const char* str_trim_ws(const char* s) {
    while (*s && isspace(*s)) s++;
    return s;
}

void print_ast_label(AST* ast, const char* target_label) {
    bool in_label_block = false;

    for (size_t i = 0; i < ast->stmts_count; i++) {
        Stmt* stmt = ast->stmts[i];

        if (stmt->type == STMT_LABEL) {
            if (strcmp(stmt->label.name, target_label) == 0) {
                in_label_block = true;
            } else {
                in_label_block = false;
                continue;
            }
        }

        if (!in_label_block) continue;

        for (int j = 0; j < stmt->indent_level; j++) printf("    ");

        switch (stmt->type) {
            case STMT_LABEL:
                printf("%s:\n", stmt->label.name);
                break;
            case STMT_COMMAND: {
                const char* cmd = str_trim_ws(stmt->command.raw_line);
                printf("%s\n", stmt->command.raw_line);

                for (size_t k = 0; k < ast->stmts_count; k++) {
                    Stmt* possible_label = ast->stmts[k];
                    if (possible_label->type == STMT_LABEL &&
                        strcmp(possible_label->label.name, cmd) == 0) {
                        print_ast_label(ast, cmd);
                    }
                }
            } break;
            case STMT_VAR_ASSIGN:
                printf("%s = %s\n", stmt->var_assign.name, stmt->var_assign.value);
                break;
            case STMT_ATTR:
                printf("#%s", stmt->attr.name);
                if (stmt->attr.param_count > 0) {
                    printf("(");
                    for (int j = 0; j < stmt->attr.param_count; j++) {
                        if (j > 0) printf(", ");
                        printf("%s", stmt->attr.parameters[j]->command.raw_line);
                    }
                    printf(")");
                }
                printf("\n");
                break;
            case STMT_IF:
                printf("#if(%s)\n", stmt->if_stmt.condition);
                break;
            case STMT_ELSE:
                printf("#else\n");
                break;
            case STMT_ENDIF:
                printf("#endif\n");
                break;
            case STMT_GOTO:
                printf("goto %s\n", stmt->goto_stmt.target);
                break;
            case STMT_CALL:
                printf("call %s\n", stmt->call_stmt.target);
                break;
            default:
                printf("Unknown statement type\n");
                break;
        }
    }
}


void print_ast(AST* ast) {
    for (size_t i = 0; i < ast->stmts_count; i++) {
        Stmt* stmt = ast->stmts[i];
        for (int j = 0; j < stmt->indent_level; j++) printf("    ");
        
        switch (stmt->type) {
            case STMT_ATTR:
                printf("#%s", stmt->attr.name);
                if (stmt->attr.param_count > 0) {
                    printf("(");
                    for (int j = 0; j < stmt->attr.param_count; j++) {
                        if (j > 0) printf(", ");
                        printf("%s", stmt->attr.parameters[j]->command.raw_line);
                    }
                    printf(")");
                }
                printf("\n");
                break;
            case STMT_VAR_ASSIGN:
                printf("%s = %s\n", stmt->var_assign.name, stmt->var_assign.value);
                break;
            case STMT_INDEX_ACCESS:
                printf("INDEX_ACCESS\n");
                break;
            case STMT_INDEX_ASSIGN:
                printf("INDEX_ASSIGN\n");
                break;
            case STMT_LABEL:
                printf("%s:\n", stmt->label.name);
                break;
            case STMT_COMMAND:
                printf("%s\n", stmt->command.raw_line);
                break;
            case STMT_IF:
                printf("#if(%s)\n", stmt->if_stmt.condition);
                break;
            case STMT_ELSE:
                printf("#else\n");
                break;
            case STMT_ENDIF:
                printf("#endif\n");
                break;
            case STMT_GOTO:
                printf("goto %s\n", stmt->goto_stmt.target);
                break;
            case STMT_CALL:
                printf("call %s\n", stmt->call_stmt.target);
                break;
            default:
                printf("Unknown statement type\n");
                break;
        }
    }
}