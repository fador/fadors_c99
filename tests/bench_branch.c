// Benchmark: branch-heavy code (conditionals)
// Tests: branch prediction, dead code elimination, boolean simplification
int collatz_steps(int n) {
    int steps = 0;
    while (n != 1) {
        if ((n & 1) == 0) {
            n = n >> 1;
        } else {
            n = n * 3 + 1;
        }
        steps = steps + 1;
    }
    return steps;
}

int main() {
    int total = 0;
    int i = 2;
    while (i < 100000) {
        total = total + collatz_steps(i);
        i = i + 1;
    }
    return (total >> 16) & 0xFF;
}
