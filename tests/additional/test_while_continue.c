int printf(const char *fmt, ...);

int main() {
    char buf[64];
    buf[0] = 'a';
    buf[1] = 'b';
    buf[2] = '/';
    buf[3] = '/';
    buf[4] = 'x';
    buf[5] = '\n';
    buf[6] = 'c';
    buf[7] = '\0';

    char *p = buf;
    int count = 0;

    while (*p) {
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            count = count + 100;
            continue;
        }
        p++;
        count = count + 1;
    }

    return count;
}
