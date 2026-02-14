// Benchmark: nested function calls
// Tests: call overhead, potential inlining, register allocation
int add(int a, int b) {
    return a + b;
}

int mul(int a, int b) {
    return a * b;
}

int compute(int x) {
    return add(mul(x, x), mul(x, 3));
}

int main() {
    int sum = 0;
    int i = 0;
    while (i < 5000000) {
        sum = sum + compute(i & 0xFF);
        i = i + 1;
    }
    return (sum >> 16) & 0xFF;
}
