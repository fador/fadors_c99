/* Test 97: assert power-of-2 with exact value —
   assert confirms x is a power of 2 AND exactly 4.
   Optimizer substitutes x with 4, then y * 4 → y << 2.
   Expected: 10 * 4 = 40 */
#include <assert.h>

int compute(int x, int y) {
    assert((x & (x - 1)) == 0 && x == 4);
    return y * x;
}

int main() {
    return compute(4, 10);
}
