int main() {
    int a = 10;
    char c = (char)a;
    if (c != 10) return 1;

    int b = 257;
    char d = (char)b; // should be 1
    if (d != 1) return 2;

    double f = 3.14;
    int i = (int)f;
    if (i != 3) return 3;
    
    int j = 10;
    double g = (double)j;
    // float comparison might be tricky without near-equal helper, but exact int-to-float usually fine for small ints
    if (g < 9.9 || g > 10.1) return 4;

    int x = 65;
    char *p = (char *)&x;
    if (*p != 65) return 5; // Little endian check implicitly

    return 42;
}
