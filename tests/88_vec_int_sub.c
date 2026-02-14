/* Test 88: Vectorization — integer array subtraction
 * Tests packed integer subtraction (psubd).
 * Expected: a[0] + a[7] = 9 + 2 = 11
 */
int main() {
    int a[8];
    int b[8];
    int c[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = 10;
        c[i] = i + 1;
    }

    for (i = 0; i < 8; i++) {
        a[i] = b[i] - c[i];
    }

    return a[0] + a[7];
}
