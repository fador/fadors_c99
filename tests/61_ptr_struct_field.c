// Minimal test: pointer to struct, read second field
struct S {
    int a;
    int b;
};

int main() {
    struct S s;
    s.a = 10;
    s.b = 20;
    
    struct S *p = &s;
    
    // Read through pointer
    if (p->a != 10) return 1;
    if (p->b != 20) return 2;
    
    // Pointer to pointer's target's field
    int result = p->b;
    if (result != 20) return 3;
    
    return 0;
}
