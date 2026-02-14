int printf(const char *fmt, ...); int g_val = 123; int *g_ptr = &g_val; int main() { printf("Value: %d\n", *g_ptr); return 0; }
