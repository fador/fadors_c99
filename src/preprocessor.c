#include "preprocessor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *name;
    char *value;
} Macro;

static Macro macros[100];
static int macros_count = 0;

static void add_macro(const char *name, const char *value) {
    macros[macros_count].name = strdup(name);
    macros[macros_count].value = value ? strdup(value) : strdup("");
    macros_count++;
}

static const char *get_macro(const char *name) {
    for (int i = 0; i < macros_count; i++) {
        if (strcmp(macros[i].name, name) == 0) {
            return macros[i].value;
        }
    }
    return NULL;
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
    (void)filename; // TODO: Use for relative includes
    
    // Very simple preprocessor that handles #include and #define
    // For now, it just concatenates includes and ignores other directives
    
    size_t out_size = strlen(source) * 2; // Rough estimate
    char *output = malloc(out_size);
    size_t out_pos = 0;
    
    const char *p = source;
    while (*p) {
        if (*p == '#') {
            p++;
            while (isspace(*p)) p++;
            
            if (strncmp(p, "include", 7) == 0) {
                p += 7;
                while (isspace(*p)) p++;
                if (*p == '"' || *p == '<') {
                    char quote = *p;
                    p++;
                    const char *start = p;
                    while (*p != quote && *p != '\n' && *p != '\0') p++;
                    size_t len = p - start;
                    char *inc_filename = malloc(len + 1);
                    strncpy(inc_filename, start, len);
                    inc_filename[len] = '\0';
                    if (*p == quote) p++;
                    
                    // Recursive preprocess
                    char *inc_source = read_file(inc_filename);
                    if (inc_source) {
                        char *preprocessed_inc = preprocess(inc_source, inc_filename);
                        size_t inc_len = strlen(preprocessed_inc);
                        if (out_pos + inc_len >= out_size) {
                            out_size = out_pos + inc_len + 1024;
                            output = realloc(output, out_size);
                        }
                        strcpy(output + out_pos, preprocessed_inc);
                        out_pos += inc_len;
                        free(preprocessed_inc);
                        free(inc_source);
                    }
                    free(inc_filename);
                }
            } else if (strncmp(p, "define", 6) == 0) {
                p += 6;
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (!isspace(*p) && *p != '\n' && *p != '\0') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';
                
                while (isspace(*p) && *p != '\n') p++;
                const char *val_start = p;
                while (*p != '\n' && *p != '\0') p++;
                size_t val_len = p - val_start;
                char *val = malloc(val_len + 1);
                strncpy(val, val_start, val_len);
                val[val_len] = '\0';
                
                add_macro(name, val);
                free(name);
                free(val);
            } else if (strncmp(p, "ifndef", 6) == 0) {
                p += 6;
                while (isspace(*p)) p++;
                const char *name_start = p;
                while (!isspace(*p) && *p != '\n' && *p != '\0') p++;
                size_t name_len = p - name_start;
                char *name = malloc(name_len + 1);
                strncpy(name, name_start, name_len);
                name[name_len] = '\0';
                
                if (get_macro(name)) {
                    // Skip until #endif or #else
                    int depth = 1;
                    while (depth > 0 && *p) {
                        if (*p == '#') {
                            p++;
                            while (isspace(*p)) p++;
                            if (strncmp(p, "ifndef", 6) == 0) depth++;
                            else if (strncmp(p, "endif", 5) == 0) depth--;
                        }
                        p++;
                    }
                }
                free(name);
            } else {
                // Ignore other directives for now
                while (*p != '\n' && *p != '\0') p++;
            }
        } else if (isalpha(*p) || *p == '_') {
            const char *start = p;
            while (isalnum(*p) || *p == '_') p++;
            size_t len = p - start;
            char *name = malloc(len + 1);
            strncpy(name, start, len);
            name[len] = '\0';
            
            const char *val = get_macro(name);
            if (val) {
                size_t val_len = strlen(val);
                if (out_pos + val_len >= out_size) {
                    out_size = out_pos + val_len + 1024;
                    output = realloc(output, out_size);
                }
                strcpy(output + out_pos, val);
                out_pos += val_len;
            } else {
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
            output[out_pos++] = *p++;
        }
    }
    output[out_pos] = '\0';
    return output;
}
