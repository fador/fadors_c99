/* Test: Dead argument elimination
   add_extra() takes 3 params but 'unused' is never referenced in the body.
   At -O3, the optimizer should remove the dead parameter and update call sites.
   Expected return: (10 + 20) + (5 + 15) = 50 */

int add_extra(int a, int unused, int b) {
    return a + b;
}

int main() {
    int x = add_extra(10, 999, 20);
    int y = add_extra(5, 888, 15);
    return x + y;
}
