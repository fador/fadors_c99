/* Test: combined aggressive inline + loop unrolling (-O3)
 * Multi-statement function called in a loop that gets unrolled.
 * Expected return: 30  (add_and_double(0,1) + add_and_double(1,1) + ... + add_and_double(4,1))
 *   = 2 + 4 + 6 + 8 + 10 = 30
 */

int add_and_double(int a, int b) {
    int sum = a + b;
    int result = sum * 2;
    return result;
}

int main() {
    int total = 0;
    for (int i = 0; i < 5; i = i + 1) {
        total = total + add_and_double(i, 1);
    }
    return total;
}
