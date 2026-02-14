/* Test 96: assert range — optimizer uses range info from assert.
   assert(x >= 0 && x <= 100) narrows the range but doesn't substitute.
   The division by 4 is already strength-reduced by existing constant optimization.
   Expected: 80 / 4 = 20 */
#include <assert.h>

int compute(int x) {
    assert(x >= 0 && x <= 100);
    return x / 4;
}

int main() {
    return compute(80);
}
