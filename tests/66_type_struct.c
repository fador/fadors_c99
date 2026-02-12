// Test: Type-like struct with union - check field access offsets  
// This tests if the compiler generates correct offsets for ->kind and ->size

struct Member {
    char *name;
    int *type;
    int offset;
};

struct Type {
    int kind;
    int size;
    int array_len;
    union {
        struct Type *ptr_to;
        struct {
            char *name;
            struct Member *members;
            int members_count;
        } struct_data;
    } data;
};

int main() {
    struct Type t;
    t.kind = 42;
    t.size = 99;
    t.array_len = 7;

    // Direct access should work
    if (t.kind != 42) return 1;
    if (t.size != 99) return 2;
    if (t.array_len != 7) return 3;

    // Now through a pointer
    struct Type *p;
    p = &t;
    if (p->kind != 42) return 4;
    if (p->size != 99) return 5;
    if (p->array_len != 7) return 6;

    // Set up chain: outer->data.ptr_to->size
    struct Type inner;
    inner.kind = 0;
    inner.size = 4;
    inner.array_len = 0;

    struct Type outer;
    outer.kind = 9;
    outer.size = 16;
    outer.array_len = 4;
    outer.data.ptr_to = &inner;

    // Access through chain
    struct Type *pt;
    pt = outer.data.ptr_to;
    if (pt->kind != 0) return 7;
    if (pt->size != 4) return 8;

    // Direct chain access
    if (outer.data.ptr_to->kind != 0) return 9;
    if (outer.data.ptr_to->size != 4) return 10;
    
    return 0;
}
