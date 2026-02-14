/* Test 94: basic assert — assert with true condition should not trap.
   Expected: return 42 */
#include <assert.h>

int main() {
    int x = 10;
    assert(x == 10);
    return 42;
}
