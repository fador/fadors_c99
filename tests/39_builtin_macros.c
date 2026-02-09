int main() {
    char *file = __FILE__;
    int line = __LINE__;

    if (line != 3) return 1;
    // We don't have string comparison yet, so we just check if it's non-null
    if (!file) return 2;
    
    // Check if __LINE__ changes
    if (__LINE__ != 10) return 3;

    return 42;
}
