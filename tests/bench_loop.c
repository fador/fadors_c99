// Benchmark: tight loop with arithmetic
// Tests: loop overhead, constant folding, strength reduction
int main() {
    int sum = 0;
    int i = 0;
    while (i < 10000000) {
        sum = sum + i * 3;
        i = i + 1;
    }
    // Return low byte to fit in exit code
    return (sum >> 16) & 0xFF;
}
