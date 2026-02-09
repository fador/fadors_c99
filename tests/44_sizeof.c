int main() {
    // Basic types
    if (sizeof(char) != 1) return 1;
    if (sizeof(int) != 8) return 2; // My compiler uses 64-bit ints
    
    int a = 10;
    if (sizeof(a) != 8) return 3;
    
    char b = 5;
    if (sizeof(b) != 1) return 4;
    
    // Pointers
    if (sizeof(&a) != 8) return 5; // x64 pointer
    int *p = &a;
    if (sizeof(p) != 8) return 6;
    if (sizeof(*p) != 8) return 7; // *p is int, so 8
    
    // Arrays
    int arr[10]; 
    if (sizeof(arr) != 80) return 8; // 10 * 8
    
    // Expression
    if (sizeof(a + 1) != 8) return 9;
    if (sizeof(b + 1) != 8) return 10; // Promoted to int (8)
    
    // Compile-time constant check
    int buffer[sizeof(int) * 2]; // Should be size 16 (8*2)
    if (sizeof(buffer) != 128) return 11; // 16 * 8 = 128
    
    return 0; // Success
}
