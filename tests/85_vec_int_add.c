/* Test 85: Vectorization — integer array addition
 * At -O3, the loop should be vectorized using SSE2 packed integer
 * instructions (paddd), processing 4 elements at a time.
 * Expected: a[0] + a[7] = 2 + 16 = 18
 */
int main() {
    int a[8];
    int b[8];
    int c[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = i + 1;
        c[i] = i + 1;
    }

    for (i = 0; i < 8; i++) {
        a[i] = b[i] + c[i];
    }

    return a[0] + a[7];
}
