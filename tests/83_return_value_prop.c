/* Test: Return value propagation
   get_magic() is a multi-statement function but always returns the same constant (7).
   At -O3, calls to get_magic() should be replaced with the constant 7.
   Expected return: 7 + 7 * 3 + 7 = 35 */

int get_magic() {
    int x = 3;
    int y = 4;
    return x + y;
}

int main() {
    int a = get_magic();
    int b = get_magic() * 3;
    int c = get_magic();
    return a + b + c;
}
