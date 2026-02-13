int main() {
    // Basic types
    if (sizeof(char) != 1) return 1;
    if (sizeof(int) != 4) return 2; // 4-byte ints (standard LP64/LLP64)
    
    int a = 10;
    if (sizeof(a) != 4) return 3;
    
    char b = 5;
    if (sizeof(b) != 1) return 4;
    
    // Pointers
    if (sizeof(&a) != 8) return 5; // x64 pointer
    int *p = &a;
    if (sizeof(p) != 8) return 6;
    if (sizeof(*p) != 4) return 7; // *p is int, so 4
    
    // Arrays
    int arr[10]; 
    if (sizeof(arr) != 40) return 8; // 10 * 4
    
    // Expression
    if (sizeof(a + 1) != 4) return 9;
    if (sizeof(b + 1) != 4) return 10; // Promoted to int (4)
    
    // Compile-time constant check
    int buffer[sizeof(int) * 2]; // Should be size 8 (4*2)
    if (sizeof(buffer) != 32) return 11; // 8 * 4 = 32
    
    return 0; // Success
}
