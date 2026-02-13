// Minimal switch test
int main() {
    int x = 2;
    int res = 0;
    switch (x) {
        case 1: res = 10; break;
        case 2: res = 20; break;
        default: res = 30; break;
    }
    return res; // expect 20
}
