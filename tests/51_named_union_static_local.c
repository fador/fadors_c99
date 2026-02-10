struct Outer {
    union {
        int i;
        char c;
    } data;
};

int test_static() {
    static int counter = 10;
    counter++;
    return counter;
}

int main() {
    struct Outer o;
    o.data.i = 42;
    if (o.data.i != 42) return 1;
    
    if (test_static() != 11) return 2;
    if (test_static() != 12) return 3;
    
    return 0;
}
