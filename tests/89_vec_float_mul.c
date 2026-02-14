/* Test 89: Vectorization — float array multiply
 * Tests packed float multiplication (mulps).
 * Expected: (int)a[0] + (int)a[3] = 2 + 32 = 34
 */
int main() {
    float a[4];
    float b[4];
    float c[4];
    int i;

    b[0] = 1.0f; b[1] = 2.0f; b[2] = 3.0f; b[3] = 4.0f;
    c[0] = 2.0f; c[1] = 3.0f; c[2] = 4.0f; c[3] = 8.0f;

    for (i = 0; i < 4; i++) {
        a[i] = b[i] * c[i];
    }

    return (int)a[0] + (int)a[3];
}
