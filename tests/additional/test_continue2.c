int main() {
    char buf[8];
    buf[0] = 'a';
    buf[1] = 'b';
    buf[2] = '\n';
    buf[3] = 'c';
    buf[4] = '\0';

    char *p = buf;
    int count = 0;

    while (*p) {
        if (*p == '\n') {
            p++;
            count = count + 100;
            continue;
        }
        p++;
        count = count + 1;
    }

    return count;
}
