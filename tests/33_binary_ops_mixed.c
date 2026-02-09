int main() {
    int i = 10;
    float f = 2.5f;
    double d = 5.0;
    
    // Int and Float
    if (i + f != 12.5f) return 1;
    if (f * i != 25.0f) return 2;
    if (i / 4.0f != 2.5f) return 3;
    
    // Float and Double
    if (f + d != 7.5) return 4;
    if (d / f != 2.0) return 5;
    
    // Int and Double
    if (i * d != 50.0) return 6;
    if (d - i != -5.0) return 7;
    
    // Logical with mixed types
    if (!(i && f)) return 8;
    if (!(i || 0.0)) return 9;
    
    // Relational with mixed types
    if (!(i > f)) return 10;
    if (i < d) return 11; // 10 < 5.0 is false
    if (!(i == 10.0)) return 12;
    
    return 42;
}
