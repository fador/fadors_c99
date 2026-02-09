union Data {
    int i;
    char c;
};
int main() {
    union Data d;
    d.i = 65;
    if (d.c == 65) return 42;
    return 0;
}
