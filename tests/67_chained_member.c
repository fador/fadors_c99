// Simplified test: chained member access through pointer in struct

struct Inner {
    int x;
    int y;
};

struct Outer {
    int a;
    struct Inner *ip;
};

int main() {
    struct Inner i;
    i.x = 10;
    i.y = 20;
    
    struct Outer o;
    o.a = 5;
    o.ip = &i;
    
    // Direct access through intermediate
    struct Inner *p;
    p = o.ip;
    if (p->x != 10) return 1;
    if (p->y != 20) return 2;
    
    // Chained: o.ip->x and o.ip->y
    if (o.ip->x != 10) return 3;
    if (o.ip->y != 20) return 4;
    
    return 0;
}
