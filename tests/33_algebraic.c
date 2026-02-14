/* Test: -O1 algebraic simplification */
int main() {
    int a = 7;
    int b = a + 0;    /* should simplify to a = 7 */
    int c = a * 1;    /* should simplify to a = 7 */
    int d = 0 + a;    /* should simplify to a = 7 */
    int e = a - 0;    /* should simplify to a = 7 */
    int f = a / 1;    /* should simplify to a = 7 */
    int g = a | 0;    /* should simplify to a = 7 */
    int h = a ^ 0;    /* should simplify to a = 7 */
    return b + c + d + e + f + g + h; /* 7*7 = 49 */
}
