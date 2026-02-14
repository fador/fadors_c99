// Test AVX 256-bit float multiply with remainder (-O3 -mavx)
// Expected: 68
// Tests: 10 elements = 8 AVX + 2 scalar remainder

int main() {
    float a[10];
    float b[10];
    float c[10];
    int i;

    // Initialize arrays
    for (i = 0; i < 10; i = i + 1) {
        b[i] = (float)(i + 1);     // 1..10
        c[i] = 2.0f;               // all 2
    }

    // Vectorizable: a[i] = b[i] * c[i]
    for (i = 0; i < 10; i = i + 1) {
        a[i] = b[i] * c[i];
    }

    // a = {2,4,6,8,10,12,14,16,18,20}
    // Sum integer part: 2+4+6+8 = 20, 10+12+14+16 = 52, 18+20 = 38
    // Wait: 2+4+6+8+10+12+14+16+18+20 = 110
    // Return lower part to fit in exit code
    int sum = 0;
    for (i = 0; i < 5; i = i + 1) {
        sum = sum + (int)a[i];
    }

    // 2+4+6+8+10 = 30
    return sum + (int)a[9];  // 30 + 20 = 50
}
