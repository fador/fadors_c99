int main() {
    // Test 1: Post-increment in if
    // a starts at 0. a++ returns 0 (false). Body should NOT execute.
    // a becomes 1.
    int a = 0;
    if (a++) {
        return 1; // Construct failure
    }
    if (a != 1) {
        return 2; // Construct failure
    }

    // Test 2: Pre-increment in if
    // b starts at 0. ++b returns 1 (true). Body SHOULD execute.
    // b becomes 1.
    int b = 0;
    if (++b) {
        // OK
    } else {
        return 3; // Construct failure
    }
    if (b != 1) {
        return 4; // Construct failure
    }

    // Test 3: Post-decrement in if
    // c starts at 1. c-- returns 1 (true). Body SHOULD execute.
    // c becomes 0.
    int c = 1;
    if (c--) {
        // OK
    } else {
        return 5; // Construct failure
    }
    if (c != 0) {
        return 6; // Construct failure
    }

    // Test 4: Pre-decrement in if
    // d starts at 1. --d returns 0 (false). Body should NOT execute.
    // d becomes 0.
    int d = 1;
    if (--d) {
        return 7; // Construct failure
    }
    if (d != 0) {
        return 8; // Construct failure
    }

    return 0; // Success
}
