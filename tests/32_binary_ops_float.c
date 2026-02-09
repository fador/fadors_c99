int main() {
    float f1 = 10.5f;
    float f2 = 2.0f;
    
    // Float Arithmetic
    if (f1 + f2 != 12.5f) return 1;
    if (f1 - f2 != 8.5f) return 2;
    if (f1 * f2 != 21.0f) return 3;
    if (f1 / f2 != 5.25f) return 4;
    
    double d1 = 20.0;
    double d2 = 8.0;
    
    // Double Arithmetic
    if (d1 + d2 != 28.0) return 5;
    if (d1 - d2 != 12.0) return 6;
    if (d1 * d2 != 160.0) return 7;
    if (d1 / d2 != 2.5) return 8;
    
    // Comparisons
    if (!(f1 > f2)) return 9;
    if (f1 < f2) return 10;
    if (!(f1 >= 10.5f)) return 11;
    if (!(f1 <= 10.5f)) return 12;
    if (f1 == f2) return 13;
    if (!(f1 != f2)) return 14;
    
    // Logical
    if (!(f1 && f2)) return 15;
    if (!(f1 || 0.0f)) return 16;
    if (0.0f || 0.0) return 17;
    
    return 42;
}
