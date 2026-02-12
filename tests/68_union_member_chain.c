// Test: chained member access through union member pointer

struct Inner {
    int x;
    int y;
};

struct Outer {
    int a;
    int b;
    union {
        struct Inner *ip;
        int *raw;
    } data;
};

int main() {
    struct Inner i;
    i.x = 10;
    i.y = 20;
    
    struct Outer o;
    o.a = 5;
    o.b = 6;
    o.data.ip = &i;
    
    // Direct access through intermediate
    struct Inner *p;
    p = o.data.ip;
    if (p->x != 10) return 1;
    if (p->y != 20) return 2;
    
    // Chained: o.data.ip->x and o.data.ip->y
    if (o.data.ip->x != 10) return 3;
    if (o.data.ip->y != 20) return 4;
    
    return 0;
}
