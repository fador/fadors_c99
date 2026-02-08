struct Point {
    int x;
    int y;
};

int main() {
    struct Point p;
    int *ptr = &p.x;
    *ptr = 10;
    return p.x;
}
