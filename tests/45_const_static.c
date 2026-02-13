// Test const, static, and unsigned support

static int helper() {
    return 42;
}

int main() {
    // const qualifier - should parse and ignore
    const int x = 10;
    if (x != 10) return 1;
    
    // const char pointer
    const char *msg = "hello";
    if (*msg != 104) return 2; // 'h' = 104
    
    // unsigned int
    unsigned int u = 100;
    if (u != 100) return 3;
    
    // unsigned without int (implicit unsigned int)
    unsigned v = 200;
    if (v != 200) return 4;
    
    // static function call
    int r = helper();
    if (r != 42) return 5;
    
    // const after pointer: int *const
    int y = 5;
    int *const p = &y;
    if (*p != 5) return 6;
    
    // sizeof with const type
    if (sizeof(const int) != 4) return 7;
    
    // unsigned char
    unsigned char c = 255;
    if (c != 255) return 8;
    
    return 0;
}
