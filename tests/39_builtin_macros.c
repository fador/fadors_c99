int strcmp(char *s1, char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1 = s1 + 1;
        s2 = s2 + 1;
    }
    return *s1 - *s2;
}

int main() {
    char *file = __FILE__;
    int line = __LINE__;

    if (line != 11) return 1;
    
    // Check if __LINE__ changes
    if (__LINE__ != 16) return 3;

    if (strcmp(file, "tests/39_builtin_macros.c") != 0) return 4;

    return 42;
}
