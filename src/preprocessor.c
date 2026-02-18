#include "preprocessor.h"
static int top_level = 1;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *name;
    char *value;
    char **params;
    int params_count;
    int is_func;
} Macro;

static Macro macros[4096];
static int macros_count = 0;

static char *include_paths[64];
static int include_path_count = 0;

void preprocess_add_include_path(const char *path) {
    if (include_path_count < 64) {
        // Avoid duplicates? For now just add.
        include_paths[include_path_count++] = strdup(path);
    }
}

typedef struct {
    int active;
    int has_processed;
} IfState;

static IfState if_stack[64];
static int if_ptr = -1;

static int find_macro(const char *name) {
    for (int i = 0; i < macros_count; i++) {
        if (strcmp(macros[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_macro(const char *name, const char *value, int is_func, char **params, int params_count) {
    int idx = find_macro(name);
    if (idx == -1) {
        idx = macros_count++;
    } else {
        // Redefinition
        // printf("DEBUG: Redefining macro %s\n", name);
        free(macros[idx].name);
        free(macros[idx].value);
        for (int i = 0; i < macros[idx].params_count; i++) free(macros[idx].params[i]);
        free(macros[idx].params);
    }
    macros[idx].name = strdup(name);
    macros[idx].value = value ? strdup(value) : strdup("");
    macros[idx].is_func = is_func;
    macros[idx].params_count = params_count;
    macros[idx].params = params;
}

static const char *get_macro_value(const char *name) {
    int idx = find_macro(name);
    if (idx != -1) return macros[idx].value;
    return NULL;
}

void preprocess_define(const char *name, const char *value) {
    add_macro(name, value ? value : "1", 0, NULL, 0);
}

void preprocess_reset(void) {
    int i, j;
    for (i = 0; i < macros_count; i++) {
        free(macros[i].name);
        free(macros[i].value);
        for (j = 0; j < macros[i].params_count; j++)
            free(macros[i].params[j]);
        free(macros[i].params);
    }
    macros_count = 0;
    if_ptr = -1;
    top_level = 1;
}

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

char *preprocess(const char *source, const char *filename) {
    int is_first_call = 0;
    if (top_level) {
        if_ptr = -1;
        top_level = 0;
        is_first_call = 1;
    }

    size_t out_size = strlen(source) * 2 + 1024; // Rough estimate
    char *output = malloc(out_size);
    if (!output) {
        exit(1);
    }
    size_t out_pos = 0;
    
    const char *p = source;
    int bol = 1;
    while (*p) {
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') { p++; }
            bol = 1; continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) {
                if (*p == '\n') bol = 1;
                p++;
            }
            if (*p) p += 2;
            continue;
        }
        // If inside a false #ifdef/#ifndef block, skip non-directive content
        if (if_ptr >= 0 && !if_stack[if_ptr].active) {
            if (bol && *p == '#') {
                // Fall through to directive handling below
            } else {
                if (*p == '\n') bol = 1;
                else if (!isspace(*p)) bol = 0;
                p++;
                continue;
            }
        }
        if (*p == '"') {
            bol = 0;
            if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
            { output[out_pos] = *p; out_pos++; p++; }
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1) != '\0') {
                    if (out_pos + 2 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                    { output[out_pos] = *p; out_pos++; p++; }
                    { output[out_pos] = *p; out_pos++; p++; }
                } else {
                    if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                    { output[out_pos] = *p; out_pos++; p++; }
                }
            }
            if (*p == '"') {
                if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                { output[out_pos] = *p; out_pos++; p++; }
            }
            continue;
        }
        if (*p == '\'') {
            bol = 0;
            if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
            { output[out_pos] = *p; out_pos++; p++; }
            while (*p && *p != '\'') {
                if (*p == '\\' && *(p+1) != '\0') {
                    if (out_pos + 2 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                    { output[out_pos] = *p; out_pos++; p++; }
                    { output[out_pos] = *p; out_pos++; p++; }
                } else {
                    if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                    { output[out_pos] = *p; out_pos++; p++; }
                }
            }
            if (*p == '\'') {
                if (out_pos + 1 >= out_size) { out_size *= 2; output = realloc(output, out_size); }
                { output[out_pos] = *p; out_pos++; p++; }
            }
            continue;
        }

        if (bol && *p == '#') {
            bol = 0;
            p++;
            while (isspace(*p)) p++;
            
            int skipping = (if_ptr >= 0 && !if_stack[if_ptr].active);
            
            if (strncmp(p, "include", 7) == 0) {
                if (skipping) { p += 7; goto skip_line; }
                p += 7;
                while (isspace(*p)) p++;
                if (*p == '"' || *p == '<') {
                    char open = *p;
                    char close = (open == '<') ? '>' : '"';
                    int is_system = (open == '<');
                    p++;
                    const char *start = p;
                    while (*p != close && *p != '\n' && *p != '\0') p++;
                    size_t len = p - start;
                    char *inc_filename = malloc(len + 1);
                    strncpy(inc_filename, start, len);
                    inc_filename[len] = '\0';
                    if (*p == close) p++;
                    
                    char *inc_source = NULL;
                    
                    char *found_path = NULL;
                    if (is_system) {
                        char path_buf[512];
                        int found = 0;
                        
                        // Try user-configured include paths first
                        for (int i = 0; i < include_path_count; i++) {
                            sprintf(path_buf, "%s/%s", include_paths[i], inc_filename);
                            inc_source = read_file(path_buf);
                            if (inc_source) {
                                found_path = strdup(path_buf);
                                found = 1;
                                break;
                            }
                        }
                        
                        if (!found && !inc_source) {
                            sprintf(path_buf, "include/%s", inc_filename);
                            inc_source = read_file(path_buf);
                            if (inc_source) found_path = strdup(path_buf);
                        }
                        
                        if (!inc_source) {
                            sprintf(path_buf, "../include/%s", inc_filename);
                            inc_source = read_file(path_buf);
                            if (inc_source) found_path = strdup(path_buf);
                        }
                    } else {
                        const char *last_sep = NULL;
                        const char *fp = filename;
                        while (*fp) {
                            if (*fp == '/' || *fp == '\\') last_sep = fp;
                            fp++;
                        }
                        if (last_sep) {
                            size_t dir_len = last_sep - filename + 1;
                            char *rel_path = malloc(dir_len + len + 1);
                            strncpy(rel_path, filename, dir_len);
                            strcpy(rel_path + dir_len, inc_filename);
                            inc_source = read_file(rel_path);
                            if (inc_source) {
                                found_path = rel_path;
                            } else {
                                free(rel_path);
                            }
                        }
                        if (!inc_source) {
                            inc_source = read_file(inc_filename);
                            if (inc_source) found_path = strdup(inc_filename);
                        }
                    }
                    
                    if (inc_source) {
                        int prev_top = top_level;
                        top_level = 0;
                        char *preprocessed_inc = preprocess(inc_source, found_path ? found_path : inc_filename);
                        top_level = prev_top;
                        size_t inc_len = strlen(preprocessed_inc);
                        if (out_pos + inc_len >= out_size) {
                            out_size = out_pos + inc_len + 1024;
                            output = realloc(output, out_size);
                        }
                        strcpy(output + out_pos, preprocessed_inc);
                        out_pos += inc_len;
                        free(preprocessed_inc);
                        free(inc_source);
                        bol = 1; // Ensure BOL is reset after an included file
                    } else {
                        // printf("Warning: Could not find include file '%s'\n", inc_filename);
                    }
                    if (found_path) free(found_path);
                    free(inc_filename);
                }
            } else if (strncmp(p, "define", 6) == 0) {
                if (skipping) { p += 6; goto skip_line; }
                p += 6;
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (isalnum(*p) || *p == '_') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';
                
                int is_func = 0;
                char **params = NULL;
                int params_count = 0;
                
                if (*p == '(') {
                    is_func = 1;
                    p++;
                    while (*p != ')' && *p != '\n' && *p != '\0') {
                        while (isspace(*p)) p++;
                        const char *p_start = p;
                        while (isalnum(*p) || *p == '_') p++;
                        if (p > p_start) {
                            params = realloc(params, sizeof(char *) * (params_count + 1));
                            params[params_count] = malloc(p - p_start + 1);
                            strncpy(params[params_count], p_start, p - p_start);
                            params[params_count][p - p_start] = '\0';
                            params_count++;
                        }
                        while (isspace(*p)) p++;
                        if (*p == ',') p++;
                    }
                    if (*p == ')') p++;
                }
                
                while (isspace(*p) && *p != '\n') p++;
                // Collect value, handling backslash-newline continuation
                size_t val_cap = 256;
                char *val = malloc(val_cap);
                size_t val_len = 0;
                while (*p != '\0') {
                    if (*p == '\n') break;
                    // Check for backslash-newline continuation
                    if (*p == '\\' && (*(p+1) == '\n' || (*(p+1) == '\r' && *(p+2) == '\n'))) {
                        p++; // skip backslash
                        if (*p == '\r') p++; // skip \r
                        if (*p == '\n') p++; // skip \n
                        // Add a space as separator
                        if (val_len + 1 >= val_cap) { val_cap *= 2; val = realloc(val, val_cap); }
                        { val[val_len] = ' '; val_len++; }
                        continue;
                    }
                    if (val_len + 1 >= val_cap) { val_cap *= 2; val = realloc(val, val_cap); }
                    { val[val_len] = *p; val_len++; p++; }
                }
                val[val_len] = '\0';
                
                add_macro(name, val, is_func, params, params_count);
                free(name);
                free(val);
            } else if (strncmp(p, "undef", 5) == 0) {
                if (skipping) { p += 5; goto skip_line; }
                p += 5;
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (isalnum(*p) || *p == '_') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';
                int idx = find_macro(name);
                if (idx != -1) {
                    free(macros[idx].name);
                    macros[idx].name = strdup(""); 
                }
                free(name);
            } else if (strncmp(p, "ifdef", 5) == 0 || strncmp(p, "ifndef", 6) == 0) {
                int is_ifndef = (p[2] == 'n');
                p += (is_ifndef ? 6 : 5);
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (!isspace(*p) && *p != '\n' && *p != '\0') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';
                
                int defined = find_macro(name) >= 0;
                int active = is_ifndef ? !defined : defined;
                
                if (if_ptr < 63) {
                    if_ptr++;
                    int parent_active = (if_ptr == 0) ? 1 : if_stack[if_ptr - 1].active;
                    if (!parent_active) {
                        if_stack[if_ptr].active = 0;
                        if_stack[if_ptr].has_processed = 1;
                    } else {
                        if_stack[if_ptr].active = active;
                        if_stack[if_ptr].has_processed = active;
                    }
                }
                free(name);
            } else if (strncmp(p, "elif", 4) == 0) {
                p += 4;
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (!isspace(*p) && *p != '\n' && *p != '\0') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';

                if (if_ptr >= 0) {
                    int parent_active = (if_ptr == 0) ? 1 : if_stack[if_ptr - 1].active;
                    if (parent_active && !if_stack[if_ptr].has_processed) {
                        int defined = find_macro(name) >= 0;
                        if_stack[if_ptr].active = defined;
                        if (defined) if_stack[if_ptr].has_processed = 1;
                    } else {
                        if_stack[if_ptr].active = 0;
                    }
                }
                free(name);
            } else if (strncmp(p, "else", 4) == 0) {
                p += 4;
                if (if_ptr >= 0) {
                    int parent_active = (if_ptr == 0) ? 1 : if_stack[if_ptr - 1].active;
                    if (parent_active && !if_stack[if_ptr].has_processed) {
                        if_stack[if_ptr].active = 1;
                        if_stack[if_ptr].has_processed = 1;
                    } else {
                        if_stack[if_ptr].active = 0;
                    }
                }
            } else if (strncmp(p, "endif", 5) == 0) {
                p += 5;
                if (if_ptr >= 0) if_ptr--;
            } else if (strncmp(p, "pragma", 6) == 0) {
                if (skipping) { p += 6; goto skip_line; }
                p += 6;
                while (isspace(*p) && *p != '\n') p++;
                if (strncmp(p, "pack", 4) == 0) {
                    p += 4;
                    while (isspace(*p) && *p != '\n') p++;
                    if (*p == '(') {
                        p++;
                        while (isspace(*p) && *p != '\n') p++;
                        if (strncmp(p, "push", 4) == 0) {
                            p += 4;
                            int val = 8;
                            while (isspace(*p) && *p != '\n') p++;
                            if (*p == ',') {
                                p++;
                                while (isspace(*p) && *p != '\n') p++;
                                if (isdigit(*p)) {
                                    val = atoi(p);
                                    while (isdigit(*p)) p++;
                                }
                            }
                            while (*p != ')' && *p != '\n' && *p != '\0') p++;
                            if (*p == ')') p++;
                            
                            char buf[64];
                            sprintf(buf, "__pragma_pack_push(%d)", val);
                            size_t blen = strlen(buf);
                            if (out_pos + blen >= out_size) {
                                out_size = out_pos + blen + 1024;
                                output = realloc(output, out_size);
                            }
                            strcpy(output + out_pos, buf);
                            out_pos += blen;
                        } else if (strncmp(p, "pop", 3) == 0) {
                            p += 3;
                            while (*p != ')' && *p != '\n' && *p != '\0') p++;
                            if (*p == ')') p++;
                            const char *buf = "__pragma_pack_pop()";
                            size_t blen = strlen(buf);
                            if (out_pos + blen >= out_size) {
                                out_size = out_pos + blen + 1024;
                                output = realloc(output, out_size);
                            }
                            strcpy(output + out_pos, buf);
                            out_pos += blen;
                        } else if (isdigit(*p)) {
                            int val = atoi(p);
                            while (isdigit(*p)) p++;
                            while (*p != ')' && *p != '\n' && *p != '\0') p++;
                            if (*p == ')') p++;
                            char buf[64];
                            sprintf(buf, "__pragma_pack(%d)", val);
                            size_t blen = strlen(buf);
                            if (out_pos + blen >= out_size) {
                                out_size = out_pos + blen + 1024;
                                output = realloc(output, out_size);
                            }
                            strcpy(output + out_pos, buf);
                            out_pos += blen;
                        }
                    }
                }
                goto skip_line;
            } else {
                while (*p != '\n' && *p != '\0') p++;
            }

skip_line:
            while (*p != '\n' && *p != '\0') p++;
            if (*p == '\n') bol = 1;
        } else if (isalpha(*p) || *p == '_') {
            bol = 0;
            const char *start = p;
            while (isalnum(*p) || *p == '_') p++;
            size_t len = p - start;
            char *name = malloc(len + 1);
            strncpy(name, start, len);
            name[len] = '\0';
            
            int m_idx = find_macro(name);
            if (m_idx != -1 && macros[m_idx].name[0] != '\0') {

                Macro *m = &macros[m_idx];
                if (m->is_func) {
                    while (isspace(*p)) p++;
                    if (*p == '(') {
                        p++;
                        char **args = NULL;
                        int args_count = 0;
                        while (*p != ')' && *p != '\0') {
                            while (isspace(*p)) p++;
                            const char *arg_start = p;
                            int depth = 0;
                            while ((*p != ',' && *p != ')' || depth > 0) && *p != '\0') {
                                if (*p == '(') depth++;
                                if (*p == ')') depth--;
                                p++;
                            }
                            args = realloc(args, sizeof(char *) * (args_count + 1));
                            args[args_count] = malloc(p - arg_start + 1);
                            strncpy(args[args_count], arg_start, p - arg_start);
                            args[args_count][p - arg_start] = '\0';
                            args_count++;
                            if (*p == ',') p++;
                        }
                        char *body = strdup(m->value);
                        for (int i = 0; i < m->params_count && i < args_count; i++) {
                            size_t p_len = strlen(m->params[i]);
                            size_t a_len = strlen(args[i]);
                            char *new_body = malloc(strlen(body) * 2 + a_len + 2); 
                            char *src = body;
                            char *dst = new_body;
                            while (*src) {
                                int prev_ok = (src == body || !(isalnum(src[-1]) || src[-1] == '_'));
                                if (prev_ok && strncmp(src, m->params[i], p_len) == 0 && !isalnum(src[p_len]) && src[p_len] != '_') {
                                    strcpy(dst, args[i]);
                                    dst += a_len;
                                    src += p_len;
                                } else {
                                    *dst++ = *src++;
                                }
                            }
                            *dst = '\0';
                            free(body);
                            body = new_body;
                        }
                        if (*p == ')') p++;
                        
                        // Recursive preprocess the result
                        int prev_top = top_level;
                        top_level = 0;
                        char *expanded = preprocess(body, "macro");
                        top_level = prev_top;

                        size_t exp_len = strlen(expanded);
                        if (out_pos + exp_len >= out_size) {
                            out_size = out_pos + exp_len + 1024;
                            output = realloc(output, out_size);
                        }
                        strcpy(output + out_pos, expanded);
                        out_pos += exp_len;
                        free(body);
                        free(expanded);
                        for (int i = 0; i < args_count; i++) free(args[i]);
                        free(args);
                    } else {
                        goto copy_normal;
                    }
                } else {
                    // Recursive preprocess the simple macro value

                    int prev_top = top_level;
                    top_level = 0;
                    char *expanded = preprocess(m->value, "macro");
                    top_level = prev_top;

                    size_t exp_len = strlen(expanded);

                    if (out_pos + exp_len >= out_size) {
                        out_size = out_pos + exp_len + 1024;
                        output = realloc(output, out_size);
                    }
                    strcpy(output + out_pos, expanded);
                    out_pos += exp_len;
                    free(expanded);
                }
            } else if (strcmp(name, "__LINE__") == 0) {
                // Calculate line number
                int line = 1;
                for (const char *it = source; it < start; it++) {
                    if (*it == '\n') line++;
                }
                char buf[32];
                sprintf(buf, "%d", line);
                size_t b_len = strlen(buf);
                if (out_pos + b_len >= out_size) {
                    out_size = out_pos + b_len + 1024;
                    output = realloc(output, out_size);
                }
                strcpy(output + out_pos, buf);
                out_pos += b_len;
            } else if (strcmp(name, "__FILE__") == 0) {
                size_t f_len = strlen(filename);
                if (out_pos + f_len + 4 >= out_size) { // +4 for quotes and null terminator
                    out_size = out_pos + f_len + 1024;
                    output = realloc(output, out_size);
                }
                { output[out_pos] = '"'; out_pos++; }
                strcpy(output + out_pos, filename);
                out_pos += f_len;
                { output[out_pos] = '"'; out_pos++; }
            } else {
copy_normal:
                if (out_pos + len >= out_size) {
                    out_size = out_pos + len + 1024;
                    output = realloc(output, out_size);
                }
                strncpy(output + out_pos, start, len);
                out_pos += len;
            }
            free(name);
        } else {
            // Simple text copy
            if (out_pos + 1 >= out_size) {
                out_size *= 2;
                output = realloc(output, out_size);
            }
            if (*p == '\n') bol = 1;
            else if (!isspace(*p)) bol = 0;
            { output[out_pos] = *p; out_pos++; p++; }
        }
    }
    output[out_pos] = '\0';
    if (is_first_call) top_level = 1;
    return output;
}
