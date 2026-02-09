// Test compound assignment and ternary operators

int main() {
    // += 
    int a = 10;
    a += 5;
    if (a != 15) return 1;
    
    // -=
    a -= 3;
    if (a != 12) return 2;
    
    // *=
    a *= 2;
    if (a != 24) return 3;
    
    // /=
    a /= 4;
    if (a != 6) return 4;
    
    // %=
    a %= 5;
    if (a != 1) return 5;
    
    // |=
    int b = 0;
    b |= 3;
    if (b != 3) return 6;
    
    // &=
    b &= 2;
    if (b != 2) return 7;
    
    // ^=
    b ^= 7;
    if (b != 5) return 8;
    
    // <<=
    int c = 1;
    c <<= 3;
    if (c != 8) return 9;
    
    // >>=
    c >>= 1;
    if (c != 4) return 10;
    
    // Ternary operator
    int x = 1;
    int y = x ? 42 : 99;
    if (y != 42) return 11;
    
    int z = 0;
    int w = z ? 42 : 99;
    if (w != 99) return 12;
    
    return 0;
}
