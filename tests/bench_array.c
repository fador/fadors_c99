// Benchmark: array traversal
// Tests: pointer arithmetic, loop strength reduction, memory access patterns
int main() {
    int arr[256];
    int i = 0;

    // Initialize array
    while (i < 256) {
        arr[i] = i * 7 + 3;
        i = i + 1;
    }

    // Repeated traversal
    int sum = 0;
    int iter = 0;
    while (iter < 100000) {
        i = 0;
        while (i < 256) {
            sum = sum + arr[i];
            i = i + 1;
        }
        iter = iter + 1;
    }

    return (sum >> 16) & 0xFF;
}
