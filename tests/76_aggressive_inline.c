/* Test: aggressive inlining of multi-statement functions (-O3)
 * A function with multiple statements ending in return should be
 * inlined at the call site, producing the same result.
 * Expected return: 22
 */

int compute(int a, int b) {
    int x = a + b;
    int y = x * 2;
    int z = y - a;
    return z;
}

int main() {
    /* compute(10, 6): x=16, y=32, z=32-10=22 */
    int result = compute(10, 6);
    return result;
}
