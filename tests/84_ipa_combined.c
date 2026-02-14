/* Test: Combined IPA optimizations
   - scale() always called with factor=5 → IPA constant propagation
   - get_offset() always returns 10 → return value propagation
   - dead_helper() is never called → dead function elimination
   - scale() has an unused 'tag' parameter → dead argument elimination
   Expected return: scale(3,5,"") + get_offset() + scale(4,5,"")
                  = (3*5) + 10 + (4*5) = 15 + 10 + 20 = 45 */

int dead_helper(int x) {
    return x * x * x;
}

int get_offset() {
    int base = 7;
    int extra = 3;
    return base + extra;
}

int scale(int value, int factor, int tag) {
    return value * factor;
}

int main() {
    int a = scale(3, 5, 0);
    int b = get_offset();
    int c = scale(4, 5, 0);
    return a + b + c;
}
