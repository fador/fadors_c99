int printf(const char *fmt, ...);
int fflush(void *stream);
void *__acrt_iob_func(int idx);
long long strlen(const char *s);
void *malloc(long long size);

int main() {
    const char *source = "hello world // comment\nline2";
    const char *filename = "test.c";
    
    printf("PP_ENTER\n"); fflush(__acrt_iob_func(1));
    int is_first_call = 0;
    
    printf("PP_STRLEN\n"); fflush(__acrt_iob_func(1));
    long long out_size = strlen(source) * 2 + 1024;
    printf("PP_MALLOC\n"); fflush(__acrt_iob_func(1));
    char *output = malloc(out_size);
    
    printf("PP_INIT\n"); fflush(__acrt_iob_func(1));
    long long out_pos = 0;
    
    const char *p = source;
    int bol = 1;
    printf("PP_LOOP\n"); fflush(__acrt_iob_func(1));
    while (*p) {
        printf("PP_ITER\n"); fflush(__acrt_iob_func(1));
        if (*p == '/' && *(p+1) == '/') {
            printf("PP_LINE_CMT\n"); fflush(__acrt_iob_func(1));
            while (*p && *p != '\n') p++;
            bol = 1;
            continue;
        }
        printf("PP_A\n"); fflush(__acrt_iob_func(1));
        if (*p == '/' && *(p+1) == '*') {
            p = p + 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) {
                if (*p == '\n') bol = 1;
                p++;
            }
            if (*p) p = p + 2;
            continue;
        }
        if (*p == '"') {
            bol = 0;
            output[out_pos] = *p;
            out_pos = out_pos + 1;
            p++;
            while (*p && *p != '"') {
                output[out_pos] = *p;
                out_pos = out_pos + 1;
                p++;
            }
            if (*p == '"') {
                output[out_pos] = *p;
                out_pos = out_pos + 1;
                p++;
            }
            continue;
        }
        output[out_pos] = *p;
        out_pos = out_pos + 1;
        p++;
        bol = (*p == '\n') ? 1 : 0;
    }
    output[out_pos] = 0;
    printf("RESULT: %s\n", output);
    return 0;
}
