int main() {
    int x = 2;
    int res = 0;
    switch (x) {
        case 1: res = 10; break;
        case 2: res = 20; break;
        case 3: res = 30; break;
        default: res = 40; break;
    }
    
    int y = 5;
    int res2 = 0;
    switch (y) {
        case 1: res2 = 1; break;
        case 2: res2 = 2; break;
        default: res2 = 100; break;
    }
    
    return res + res2; // Should be 20 + 100 = 120
}
