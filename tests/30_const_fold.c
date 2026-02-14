/* Test: -O1 constant folding */
int main() {
    int x = 3 + 4;       /* should fold to 7 */
    int y = 10 * 5 - 2;  /* should fold to 48 */
    int z = 100 / 4;     /* should fold to 25 */
    int w = 17 % 5;      /* should fold to 2 */
    return x + y + z + w; /* 7 + 48 + 25 + 2 = 82 */
}
