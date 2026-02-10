// Test hex literals, character literals, and long type

int main() {
    // Hex literals
    int hex1 = 0xFF;
    if (hex1 != 255) return 1;
    
    int hex2 = 0x10;
    if (hex2 != 16) return 2;
    
    int hex3 = 0xDEAD;
    if (hex3 != 57005) return 3;
    
    // Character literals
    char c1 = 'A';
    if (c1 != 65) return 4;
    
    char c2 = '0';
    if (c2 != 48) return 5;
    
    // Character escape sequences
    char newline = '\n';
    if (newline != 10) return 6;
    
    char null_char = '\0';
    if (null_char != 0) return 7;
    
    char tab = '\t';
    if (tab != 9) return 8;
    
    char backslash = '\\';
    if (backslash != 92) return 9;
    
    // Long type (treated as int)
    long x = 42;
    if (x != 42) return 10;
    
    long int y = 100;
    if (y != 100) return 11;

    // Hex with uppercase
    int hex4 = 0xABCD;
    if (hex4 != 43981) return 12;
    
    return 0;
}
