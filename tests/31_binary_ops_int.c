int main() {
    // Arithmetic
    if (10 + 5 != 15) return 1;
    if (10 - 5 != 5) return 2;
    if (10 * 5 != 50) return 3;
    if (10 / 5 != 2) return 4;
    if (10 % 3 != 1) return 5;
    
    // Bitwise
    if ((5 & 3) != 1) return 6;    // 101 & 011 = 001
    if ((5 | 3) != 7) return 7;    // 101 | 011 = 111
    if ((5 ^ 3) != 6) return 8;    // 101 ^ 011 = 110
    if ((1 << 3) != 8) return 9;
    if ((8 >> 2) != 2) return 10;
    
    // Logical
    if (!(1 && 1)) return 11;
    if (1 && 0) return 12;
    if (!(1 || 0)) return 13;
    if (0 || 0) return 14;
    
    // Short-circuiting verification (simulated by logic)
    int x = 0;
    // (1 || (x = 1)) should not set x to 1 if short-circuit works
    // But I don't have side-effect assignments inside expressions yet that I can easily test here
    // Let's just verify basic logic for now.
    
    if (5 == 5 && 2 < 3 && 4 >= 4 && 10 != 0) {
        return 42;
    }
    
    return 15;
}
