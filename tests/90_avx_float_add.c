// Test AVX 256-bit float add vectorization (-O3 -mavx)
// Expected: 36
// Requires -mavx flag to use 256-bit ymm registers

int main() {
    float a[8];
    float b[8];
    float c[8];
    int i;

    // Initialize arrays
    for (i = 0; i < 8; i = i + 1) {
        b[i] = (float)(i + 1);     // 1,2,3,4,5,6,7,8
        c[i] = (float)(i + 1);     // 1,2,3,4,5,6,7,8
    }

    // Vectorizable: a[i] = b[i] + c[i]
    for (i = 0; i < 8; i = i + 1) {
        a[i] = b[i] + c[i];
    }

    // Sum: 2+4+6+8+10+12+14+16 = 72
    // Return as int (truncated)
    float sum = 0.0f;
    for (i = 0; i < 8; i = i + 1) {
        sum = sum + a[i];
    }

    return (int)sum;  // 72
}
