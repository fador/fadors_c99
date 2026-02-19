#include <stdio.h>
#include <stdlib.h>

extern void dos_libc_init();

char *g_buf;

int main() {
    dos_libc_init();
    printf("Step 1: printf works!\r\n");
    
    puts("Step 2: puts works!\r\n");
    
    putchar('S');
    putchar('3');
    putchar('\r');
    putchar('\n');
    
    printf("Step 4: printf with number: %d\r\n", 42);
    
    printf("Step 5: printf with string: %s\r\n", "hello");
    
    printf("Step 6: about to call malloc...\r\n");
    g_buf = malloc(128);
    printf("DEBUG: main sees buf = %x\r\n", (unsigned int)g_buf);
    if (!g_buf) {
        printf("Step 6: FAIL malloc returned NULL\r\n");
    } else {
        printf("Step 7: malloc OK %x\r\n", (unsigned int)g_buf);
        
        g_buf[0] = 'H';
        g_buf[1] = 'i';
        g_buf[2] = '\0';
        printf("Step 8: buf = %s\r\n", g_buf);
        
        free(g_buf);
        printf("Step 9: free OK\r\n");
    }
    
    printf("Step 10: about to fopen...\r\n");
    FILE *f = fopen("TEST.TXT", "w");
    if (!f) {
        printf("Step 10: FAIL fopen returned NULL\r\n");
    } else {
        printf("Step 11: fopen OK\r\n");
        fputs("Hello File", f);
        fclose(f);
        printf("Step 12: fclose OK\r\n");
    }
    
    printf("ALL TESTS PASSED!\r\n");
    return 0;
}
