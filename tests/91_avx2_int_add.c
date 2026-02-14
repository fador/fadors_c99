// Test AVX2 256-bit integer add vectorization (-O3 -mavx2)
// Expected: 72
// Requires -mavx2 flag to use 256-bit ymm registers for integer ops

int main() {
    int a[8];
    int b[8];
    int c[8];
    int i;

    // Initialize arrays
    for (i = 0; i < 8; i = i + 1) {
        b[i] = i + 1;     // 1,2,3,4,5,6,7,8
        c[i] = i + 1;     // 1,2,3,4,5,6,7,8
    }

    // Vectorizable: a[i] = b[i] + c[i]
    for (i = 0; i < 8; i = i + 1) {
        a[i] = b[i] + c[i];
    }

    // Sum: 2+4+6+8+10+12+14+16 = 72
    int sum = 0;
    for (i = 0; i < 8; i = i + 1) {
        sum = sum + a[i];
    }

    return sum;  // 72
}
