/* Test: -O1 dead code elimination */
int main() {
    int x = 10;
    int y = 20;
    return x + y;   /* = 30 */
    int z = 99;     /* dead code: after return */
    return z;       /* dead code: after return */
}
