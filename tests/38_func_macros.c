#define ADD(a, b) ((a) + (b))
#define SQUARE(x) ((x) * (x))
#define MULTI(x, y, z) ((x) * (y) + (z))

int main() {
    if (ADD(10, 20) != 30) return 1;
    if (SQUARE(5) != 25) return 2;
    if (MULTI(2, 3, 4) != 10) return 3;
    
#define TEMP 100
    if (TEMP != 100) return 4;
#undef TEMP
#ifdef TEMP
    return 5;
#endif

    // Nested use of function macros
    if (SQUARE(ADD(1, 2)) != 9) return 6;

    return 42;
}
