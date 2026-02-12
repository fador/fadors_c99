// Test: self-referential struct with chained access

struct Node {
    int value;
    int extra;
    struct Node *next;
};

int main() {
    struct Node a;
    a.value = 10;
    a.extra = 20;
    
    struct Node b;
    b.value = 30;
    b.extra = 40;
    b.next = &a;

    // Access through chain
    if (b.next->value != 10) return 1;
    if (b.next->extra != 20) return 2;
    
    return 0;
}
