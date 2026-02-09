int main() {
    int a[5];
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    a[3] = 40;
    a[4] = a[0] + a[1] + a[2] + a[3];
    return a[4]; // Should be 100
}
