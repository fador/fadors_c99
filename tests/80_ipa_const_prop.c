/* Test: IPA constant propagation
   multiply() is always called with b=10.
   At -O3, the optimizer should propagate b=10 into the function body,
   enabling constant folding: a * 10 → compile-time evaluation.
   Expected return: 5 * 10 + 3 * 10 = 80 */

int multiply(int a, int b) {
    int result = a * b;
    return result;
}

int main() {
    int x = multiply(5, 10);
    int y = multiply(3, 10);
    return x + y;
}
