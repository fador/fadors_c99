/* Test: full loop unrolling with small count (-O3)
 * Loop with 5 iterations fully unrolled.
 * Expected return: 10  (0+1+2+3+4)
 */

int main() {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
