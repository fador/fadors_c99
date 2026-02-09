// Test for-init variable declaration
int main() {
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += i;
    }
    // 0+1+2+3+4 = 10
    if (sum != 10) return 1;
    
    // Nested for with compound assignment and var decl
    int total = 0;
    for (int j = 1; j <= 3; j++) {
        total += j * 10;
    }
    // 10 + 20 + 30 = 60
    if (total != 60) return 2;
    
    return 0;
}
