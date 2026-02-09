// Test system includes and typedef'd types
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int main() {
    // size_t from stddef.h
    size_t s = 42;
    if (s != 42) return 1;
    
    // uint8_t from stdint.h
    uint8_t b = 200;
    if (b != 200) return 2;
    
    // NULL from stddef.h
    int *p = NULL;
    if (p != 0) return 3;
    
    // malloc from stdlib.h
    char *buf = malloc(16);
    if (buf == 0) return 4;
    
    // strlen from string.h
    char *hello = "hello";
    int len = strlen(hello);
    if (len != 5) return 5;
    
    // strcmp from string.h
    if (strcmp(hello, "hello") != 0) return 6;
    
    // isalpha from ctype.h
    if (!isalpha(65)) return 7;
    
    free(buf);
    
    return 0;
}
