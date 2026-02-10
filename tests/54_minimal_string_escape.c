int main() {
    char *s = "A\nB";
    if (s[0] != 'A') return 1;
    if (s[1] != 10) return 2;
    if (s[2] != 'B') return 3;
    if (s[3] != 0) return 4;
    return 0;
}
