int strcmp(char *s1, char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1 = s1 + 1;
        s2 = s2 + 1;
    }
    return *s1 - *s2;
}

int main() {
    char *s1 = "hello";
    char *s2 = "hello";
    char *s3 = "world";
    char *s4 = "hell";
    
    // Equal strings
    if (strcmp(s1, s2) != 0) return 1;
    
    // Different strings
    if (strcmp(s1, s3) == 0) return 2;
    
    // Prefix
    if (strcmp(s1, s4) == 0) return 3;
    
    // Manual check
    if (strcmp("A", "A") != 0) return 4;
    if (strcmp("A", "B") >= 0) return 5;
    if (strcmp("B", "A") <= 0) return 6;

    return 42;
}
