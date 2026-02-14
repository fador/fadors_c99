int printf(const char *fmt, ...);

int main() {
    int a = 0x80000000;
    int b = 0x40000000;
    int c = 0x00300000;
    int d = 0x00000040;
    int result = a | b | c | d;
    return result;
}
