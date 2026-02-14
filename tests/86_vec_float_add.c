/* Test 86: Vectorization — float array addition
 * At -O3, the loop should be vectorized using SSE packed float
 * instructions (addps), processing 4 floats at a time.
 * Expected: (int)a[0] + (int)a[7] = 2 + 16 = 18
 */
int main() {
    float a[8];
    float b[8];
    float c[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = (float)(i + 1);
        c[i] = (float)(i + 1);
    }

    for (i = 0; i < 8; i++) {
        a[i] = b[i] + c[i];
    }

    return (int)a[0] + (int)a[7];
}
