/* Test 87: Vectorization — non-multiple-of-4 array size (remainder)
 * 7 elements = 4 vectorized (1 vector iteration) + 3 scalar remainder.
 * Tests that the scalar remainder loop handles leftover elements.
 * Expected: a[0] + a[6] = 11 + 17 = 28
 */
int main() {
    int a[7];
    int b[7];
    int c[7];
    int i;

    for (i = 0; i < 7; i++) {
        b[i] = i + 1;
        c[i] = 10;
    }

    for (i = 0; i < 7; i++) {
        a[i] = b[i] + c[i];
    }

    return a[0] + a[6];
}
