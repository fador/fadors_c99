/* Test 95: assert exact value hint — optimizer should substitute x with 8,
   turning y * x into y * 8, then strength-reduce to y << 3.
   Expected: 5 * 8 = 40 */
#include <assert.h>

int compute(int x, int y) {
    assert(x == 8);
    return y * x;
}

int main() {
    return compute(8, 5);
}
