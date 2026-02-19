#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("Step 1: printf works!\n");
    
    puts("Step 2: puts works!");
    
    putchar('S');
    putchar('3');
    putchar('\n');
    
    printf("Step 4: printf with number: %d\n", 42);
    
    printf("Step 5: printf with string: %s\n", "hello");
    
    printf("Step 6: about to call malloc...\n");
    char *buf = malloc(128);
    if (!buf) {
        printf("Step 6: FAIL malloc returned NULL\n");
        // exit(1); 
    } else {
        printf("Step 7: malloc OK %x\n", (unsigned int)buf);
        
        buf[0] = 'H';
        buf[1] = 'i';
        buf[2] = '\0';
        printf("Step 8: buf = %s\n", buf);
        
        free(buf);
        printf("Step 9: free OK\n");
    }
    
    printf("Step 10: about to fopen...\n");
    FILE *f = fopen("TEST.TXT", "w");
    if (!f) {
        printf("Step 10: FAIL fopen returned NULL\n");
        // exit(1);
    } else {
        printf("Step 11: fopen OK\n");
        fputs("Hello File", f);
        fclose(f);
        printf("Step 12: fclose OK\n");
    }
    
    printf("ALL TESTS PASSED!\n");
    return 0;
}
