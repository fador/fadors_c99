enum Color { RED, GREEN = 5, BLUE };
int main() {
    enum Color c = GREEN;
    if (c == 5) return BLUE; // Should return 6
    return 0;
}
