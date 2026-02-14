/* Test: Dead function elimination
   helper() is called by compute(), but compute() is called by main().
   After inlining at -O3, private_unused() has zero callers and should be
   eliminated from the output.
   Expected return: 42 */

int private_unused() {
    return 99;
}

int compute(int a) {
    return a + 2;
}

int main() {
    return compute(40);
}
