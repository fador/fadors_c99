int printf(const char *fmt, ...);

int main() {
    const char *p = "int main() { return 42; }";
    int bol = 1;
    int iter = 0;
    int out_pos = 0;
    char output[256];
    printf("before loop\n");
    while (*p) {
        iter++;
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            bol = 1;
            continue;
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
        if (*p == '"') {
            bol = 0;
            output[out_pos++] = *p++;
            while (*p && *p != '"') {
                output[out_pos++] = *p++;
            }
            if (*p == '"') output[out_pos++] = *p++;
            continue;
        }

        if (bol && *p == '#') {
            bol = 0;
            p++;
        } else if (*p == '\n') {
            bol = 1;
            output[out_pos++] = *p++;
        } else {
            bol = 0;
            output[out_pos++] = *p++;
        }
    }
    output[out_pos] = 0;
    printf("done iter=%d out='%s'\n", iter, output);
    return iter;
}
