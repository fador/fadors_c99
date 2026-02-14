#include <ctype.h>
#include <stdio.h>

int test_char(const char *p) {
    if (isalpha(*p)) return 42;
    return 0;
}

int main() {
    const char *s = "hello";
    int r = test_char(s);
    return r;
}
