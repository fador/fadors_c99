float negate(float x) {
    return -x;
}

double d_negate(double x) {
    return -x;
}

int is_zero(float x) {
    return !x;
}

int main() {
    float a = 1.5f;
    float b = negate(a);
    
    // Check if b is -1.5f
    if (b != -1.5f) {
        if (b == 1.5f) return 100; // Did not negate?
        if (b == 0.0f) return 101; // Returned zero?
        return 1; // Something else
    }
    
    if (d_negate(2.5) != -2.5) return 2;
    if (!is_zero(0.0f)) return 3;
    if (is_zero(1.0f)) return 4;
    
    float f5 = 5.0f;
    if (negate(f5) != -5.0f) return 5;
    
    return 42;
}
