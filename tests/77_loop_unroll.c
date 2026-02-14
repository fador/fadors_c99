/* Test: loop unrolling with small constant count (-O3)
 * A for-loop with known iteration count (≤ 8) should be fully unrolled.
 * Expected return: 45  (0+1+2+3+4+5+6+7+8+9)
 */

int main() {
    int sum = 0;
    for (int i = 0; i < 10; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
