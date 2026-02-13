/* Test 64-bit integer arithmetic */
int printf(const char *fmt, ...);

int main() {
    long S = 4194531;   /* 0x4000e3 */
    long A = -4;
    long P = 4194527;   /* 0x4000df */

    long SA = S + A;
    long val = SA - P;

    printf("S=%ld A=%ld SA=%ld P=%ld val=%ld\n", S, A, SA, P, val);

    if (SA != 4194527) {
        printf("FAIL: SA expected 4194527, got %ld\n", SA);
        return 1;
    }
    if (val != 0) {
        printf("FAIL: val expected 0, got %ld\n", val);
        return 1;
    }
    printf("PASS\n");
    return 42;
}
