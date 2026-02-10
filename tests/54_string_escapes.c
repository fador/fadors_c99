#include <stdio.h>
#include <string.h>

int main() {
    // Declaring an even number of locals at the top ensures consistent 16-byte stack alignment for Win64 calls
    char *p1 = "align"; 
    char *s1 = "Line 1\nLine 2";
    char *s2 = "Tab\tSeparated";
    char *s3 = "Quote: \"\"";
    char *s4 = "Backslash: \\";
    char *s5 = "End"; // Wait, that's 6 locals. Even. Good.
    
    printf("Testing string escapes:\n");
    printf("S1: %s\n", s1);
    
    if (strlen(s1) != 13) {
        printf("Error: strlen(s1) is %d, expected 13\n", (int)strlen(s1));
        return 1;
    }
    
    if (s1[6] != '\n') return 2;
    if (s2[3] != '\t') return 3;
    if (s3[7] != '"') return 4;
    if (s4[11] != '\\') return 5;

    printf("All string escape tests passed!\n");
    return 0;
}
