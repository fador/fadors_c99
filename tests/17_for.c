int main() {
    int sum = 0;
    for (int i = 0; i < 10; i = i + 1) {
        sum = sum + i;
    }
    return sum; // Should be 45 (0+1+2+3+4+5+6+7+8+9)
}
