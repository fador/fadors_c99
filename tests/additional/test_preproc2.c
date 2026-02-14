int printf(const char *fmt, ...);
int fflush(void *stream);
void *__acrt_iob_func(int idx);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
long long strlen(const char *s);
void *malloc(long long size);

int main() {
    const char *source = "int main() { return 42; }\n";
    
    printf("ENTER\n"); fflush(__acrt_iob_func(1));
    int is_first_call = 0;
    
    long long out_size = strlen(source) * 2 + 1024;
    char *output = malloc(out_size);
    long long out_pos = 0;
    
    const char *p = source;
    int bol = 1;
    printf("LOOP\n"); fflush(__acrt_iob_func(1));
    while (*p) {
        printf("ITER\n"); fflush(__acrt_iob_func(1));
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            bol = 1;
            continue;
        }
        printf("A\n"); fflush(__acrt_iob_func(1));
        if (*p == '/' && *(p+1) == '*') {
            p = p + 2;
            continue;
        }
        printf("B\n"); fflush(__acrt_iob_func(1));
        if (*p == '"') {
            output[out_pos] = *p;
            out_pos = out_pos + 1;
            p++;
            continue;
        }
        printf("C\n"); fflush(__acrt_iob_func(1));
        if (*p == '\'') {
            output[out_pos] = *p;
            out_pos = out_pos + 1;
            p++;
            continue;
        }
        printf("D\n"); fflush(__acrt_iob_func(1));
        if (bol && *p == '#') {
            p++;
            continue;
        } else if (isalpha(*p) || *p == '_') {
            bol = 0;
            const char *start = p;
            while (isalnum(*p) || *p == '_') p++;
            long long len = p - start;
            char *name = malloc(len + 1);
            printf("IDENT: %s\n", name);
        } else {
            if (*p == '\n') bol = 1;
            else if (!isspace(*p)) bol = 0;
            output[out_pos] = *p;
            out_pos = out_pos + 1;
            p++;
        }
    }
    output[out_pos] = 0;
    printf("RESULT: %s\n", output);
    return 0;
}
