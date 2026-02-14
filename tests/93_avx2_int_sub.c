// Test AVX2 256-bit integer subtraction (-O3 -mavx2)
// Expected: 8

int main() {
    int a[8];
    int b[8];
    int c[8];
    int i;

    // Initialize arrays
    for (i = 0; i < 8; i = i + 1) {
        b[i] = (i + 1) * 3;   // 3,6,9,12,15,18,21,24
        c[i] = i + 1;         // 1,2,3,4,5,6,7,8
    }

    // Vectorizable: a[i] = b[i] - c[i]
    for (i = 0; i < 8; i = i + 1) {
        a[i] = b[i] - c[i];
    }

    // a = {2,4,6,8,10,12,14,16}
    // Return a[0] + a[3] = 2 + 8 = 10
    // Actually let's just return a[3]: 8
    return a[3];  // 8
}
