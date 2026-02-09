int main() {
    int arr[5];
    int *p = arr;
    int *q = p + 2;
    
    // Check pointer difference: q - p should be 2 (indices), not 16 (bytes)
    int diff = q - p;
    if (diff != 2) return diff;
    
    // Check scaling logic
    char *c = p;
    char *d = q;
    // q is p + 2 ints. int is 8 bytes in this compiler (based on type_int() in types.c).
    // So distance in bytes should be 16.
    if (d - c != 16) return 2;
    
    // Check ptr - int
    int *r = q - 1;
    if (r - p != 1) return 3;

    return 0;
}
