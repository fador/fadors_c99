// Test that variables declared inside a loop don't cause stack overflow
int main() {
    int total = 0;
    int i = 0;
    while (i < 100) {
        int a = i * 2;
        int b = a + 1;
        total = total + b;
        i = i + 1;
    }
    // total = sum of (2i+1) for i=0..99 = 2*(0+1+...+99) + 100 = 2*4950 + 100 = 10000
    return total / 100;  // should be 100
}
