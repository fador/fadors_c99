// Test: struct with pointer field, chain access through pointer
// Mimics the core access pattern: type->data.ptr_to->size

struct Inner {
    int kind;
    int size;
};

struct Outer {
    int kind;
    int size;
    int array_len;
    struct Inner *ptr_to;
};

int main() {
    struct Inner base;
    base.kind = 0;
    base.size = 4;

    struct Outer container;
    container.kind = 5;
    container.size = 8;
    container.array_len = 0;
    container.ptr_to = &base;

    // Read through pointer chain: container.ptr_to->size
    int sz = container.ptr_to->size;

    return sz;  // expect 4
}
