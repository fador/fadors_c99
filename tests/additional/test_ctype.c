int printf(const char *fmt, ...);
int fflush(void *stream);
void *__acrt_iob_func(int idx);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);

int main() {
    const char *p = "int main";
    int bol = 1;
    
    printf("START\n"); fflush(__acrt_iob_func(1));
    
    if (*p == '/' && *(p+1) == '/') {
        printf("LINE_CMT\n"); fflush(__acrt_iob_func(1));
    }
    printf("A\n"); fflush(__acrt_iob_func(1));
    if (*p == '/' && *(p+1) == '*') {
        printf("BLOCK_CMT\n"); fflush(__acrt_iob_func(1));
    }
    printf("B\n"); fflush(__acrt_iob_func(1));
    if (*p == '"') {
        printf("QUOTE\n"); fflush(__acrt_iob_func(1));
    }
    printf("C\n"); fflush(__acrt_iob_func(1));
    if (*p == '\'') {
        printf("SQUOTE\n"); fflush(__acrt_iob_func(1));
    }
    printf("D\n"); fflush(__acrt_iob_func(1));
    if (bol && *p == '#') {
        printf("HASH\n"); fflush(__acrt_iob_func(1));
    } else if (isalpha(*p) || *p == '_') {
        printf("ALPHA\n"); fflush(__acrt_iob_func(1));
    } else {
        printf("OTHER\n"); fflush(__acrt_iob_func(1));
    }
    printf("DONE\n"); fflush(__acrt_iob_func(1));
    return 0;
}
