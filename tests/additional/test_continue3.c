int main() {
    char buf[8];
    buf[0] = 'a';
    buf[1] = '/';
    buf[2] = '/';
    buf[3] = 'x';
    buf[4] = '\n';
    buf[5] = 'c';
    buf[6] = '\0';

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
