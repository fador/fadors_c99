/* Test: -O1 strength reduction (multiply/divide/mod by power-of-2) */
int main() {
    int a = 5;
    int b = a * 4;   /* should become a << 2 = 20 */
    int c = 32 / 8;  /* constant fold to 4 */
    int d = a * 8;   /* should become a << 3 = 40 */
    int e = 100 % 16; /* should become 100 & 15 = 4 */
    return b + c + d + e; /* 20 + 4 + 40 + 4 = 68 */
}
