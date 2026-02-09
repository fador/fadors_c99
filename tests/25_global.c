int g_var = 42;
int g_bss;

int main() {
    g_bss = 10;
    return g_var + g_bss; // Should be 52
}
