// PGO test: verifies profile-guided optimization workflow
// Step 1: Compile with -fprofile-generate, run to create profile data
// Step 2: Compile with -fprofile-use=default.profdata -O3
//
// The compute() function has 10 statements (9 var_decl + 1 return).
// Without PGO: too large for O3 aggressive inline (threshold=8) → call remains.
// With PGO (hot): threshold raised to 20 → function is inlined.

int compute(int a, int b) {
    int x = a + b;
    int y = a - b;
    int z = x * y;
    int w = z + a;
    int v = w - b;
    int u = v + x;
    int t = u + z;
    int s = t - w;
    int r = s + v;
    return r;
}

// Cold function: small enough to inline normally (≤8 stmts) but PGO
// prevents inlining because it's cold  (call count = 0).
int cold_func(int a, int b) {
    int p = a * b;
    int q = p + a;
    return q - b;
}

int main() {
    int sum = 0;
    int i = 0;
    // Hot path: calls compute many times
    while (i < 1000000) {
        sum = sum + compute(i, i + 1);
        i = i + 1;
    }
    // Cold path: never taken at runtime (sum != 0)
    if (sum == 0) {
        sum = cold_func(sum, 42);
    }
    return (sum >> 16) & 0xFF;
}
