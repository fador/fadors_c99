#define TEST_MACRO 1

int main() {
#ifdef TEST_MACRO
    int x = 42;
#else
    int x = 0;
#endif

#ifndef UNDEF_MACRO
    int y = 100;
#else
    int y = 0;
#endif

#ifdef UNDEF_MACRO
    return 1;
#elif TEST_MACRO
    if (x == 42 && y == 100) return 42;
#else
    return 2;
#endif
    return 0;
}
