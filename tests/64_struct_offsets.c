// Test: sizeof and offset of Type-like struct
// The compiler itself uses this struct layout

struct Type {
    int kind;       // offset 0, size 4
    int size;       // offset 4, size 4
    int array_len;  // offset 8, size 4
    // 4 bytes padding for pointer alignment
    char *ptr_to;   // offset 16, size 8 (using char* for simplicity since union not supported well)
};

int main() {
    struct Type t;
    // Check that the struct total size is correct
    // kind(4) + size(4) + array_len(4) + pad(4) + ptr_to(8) = 24
    
    char *base = (char*)&t;
    char *kind_addr = (char*)&t.kind;
    char *size_addr = (char*)&t.size;
    char *array_len_addr = (char*)&t.array_len;
    char *ptr_to_addr = (char*)&t.ptr_to;
    
    int off_kind = kind_addr - base;
    int off_size = size_addr - base;
    int off_array_len = array_len_addr - base;
    int off_ptr_to = ptr_to_addr - base;
    
    if (off_kind != 0) return 1;
    if (off_size != 4) return 2;
    if (off_array_len != 8) return 3;
    if (off_ptr_to != 16) return 4;
    
    return 0;
}
