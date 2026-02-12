// Test: char pointer dereference + increment (string-processing pattern)
int main() {
    // --- Test 1: read chars with *p++ ---
    char str[6];
    str[0] = 'H';
    str[1] = 'e';
    str[2] = 'l';
    str[3] = 'l';
    str[4] = 'o';
    str[5] = 0;

    char *p = str;
    char c;

    c = *p++;
    if (c != 'H')  return 1;
    c = *p++;
    if (c != 'e')  return 2;
    c = *p++;
    if (c != 'l')  return 3;
    c = *p++;
    if (c != 'l')  return 4;
    c = *p++;
    if (c != 'o')  return 5;
    c = *p;
    if (c != 0)    return 6;

    // --- Test 2: copy string with *dst++ = *src++ pattern ---
    char src[4];
    src[0] = 'A';
    src[1] = 'B';
    src[2] = 'C';
    src[3] = 0;

    char dst[4];
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 0;

    char *s = src;
    char *d = dst;

    // Classic strcpy-style loop
    while (*s) {
        *d++ = *s++;
    }
    *d = 0;  // null terminate

    if (dst[0] != 'A') return 7;
    if (dst[1] != 'B') return 8;
    if (dst[2] != 'C') return 9;
    if (dst[3] != 0)   return 10;

    // --- Test 3: count length with pointer subtraction ---
    char *end = d;
    char *begin = dst;
    int len = end - begin;
    if (len != 3) return 11;

    // --- Test 4: walk backwards with *--p ---
    c = *--d;
    if (c != 'C') return 12;
    c = *--d;
    if (c != 'B') return 13;
    c = *--d;
    if (c != 'A') return 14;

    return 0;
}
