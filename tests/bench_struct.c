// Benchmark: struct field access pattern
// Tests: struct layout, member offset computation, pointer chasing
struct Point {
    int x;
    int y;
    int z;
};

int distance_sq(struct Point *a, struct Point *b) {
    int dx = a->x - b->x;
    int dy = a->y - b->y;
    int dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

int main() {
    struct Point pts[64];
    int i = 0;

    // Initialize
    while (i < 64) {
        pts[i].x = i * 3;
        pts[i].y = i * 7 + 1;
        pts[i].z = i * 11 + 2;
        i = i + 1;
    }

    int total = 0;
    int iter = 0;
    while (iter < 200000) {
        i = 0;
        while (i < 63) {
            total = total + distance_sq(&pts[i], &pts[i + 1]);
            i = i + 1;
        }
        iter = iter + 1;
    }

    return (total >> 16) & 0xFF;
}
