int printf(const char *fmt, ...);

int main() {
    const char *p = "int main";
    int bol = 1;
    int iter = 0;
    printf("before loop\n");
    while (*p) {
        iter++;
        printf("iter %d ch=%d\n", iter, (int)*p);
        if (*p == '/') {
            printf("slash\n");
        } else if (*p == '"') {
            printf("quote\n");
        } else if (bol && *p == '#') {
            printf("hash\n");
        } else {
            if (*p == '\n') bol = 1;
            p++;
        }
    }
    printf("done iter=%d\n", iter);
    return iter;
}
