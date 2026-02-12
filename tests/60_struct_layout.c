// Test: verify struct member offsets match expected layout
// This tests the specific layout of a struct similar to the compiler's Type struct

struct Inner {
    char *name;
    char *members;
    int count;
};

struct MyType {
    int kind;
    int size;
    int array_len;
    union {
        char *ptr_to;
        struct Inner s;
    } data;
};

int main() {
    struct MyType t;

    // Get addresses using pointer arithmetic
    char *base = (char *)&t;
    char *p_kind = (char *)&t.kind;
    char *p_size = (char *)&t.size;
    char *p_array_len = (char *)&t.array_len;
    char *p_data = (char *)&t.data;
    char *p_ptr_to = (char *)&t.data.ptr_to;

    int off_kind = p_kind - base;
    int off_size = p_size - base;
    int off_array_len = p_array_len - base;
    int off_data = p_data - base;

    // Expected: kind=0, size=4, array_len=8, data=16
    if (off_kind != 0) return 1;
    if (off_size != 4) return 2;
    if (off_array_len != 8) return 3;
    if (off_data != 16) return 4;

    // Test write and read through members
    t.kind = 42;
    t.size = 99;
    t.array_len = 5;

    if (t.kind != 42) return 5;
    if (t.size != 99) return 6;
    if (t.array_len != 5) return 7;

    return 0;
}
