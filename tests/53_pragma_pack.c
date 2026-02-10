#include <stdio.h>

#pragma pack(push, 1)
struct Packed1 {
    char a;
    int b;
};
#pragma pack(pop)

struct Default {
    char a;
    int b;
};

#pragma pack(push, 2)
struct Packed2 {
    char a;
    int b;
};
#pragma pack(pop)

int main() {
    // Packed1: 1 (char) + 8 (int) = 9
    // pack: 1. alignment = min(8, 1) = 1.
    if (sizeof(struct Packed1) != 9) {
        printf("Error: sizeof(struct Packed1) is %d, expected 9\n", (int)sizeof(struct Packed1));
        return 1;
    }

    // Default: 1 (char) + 7 (padding) + 8 (int) = 16
    if (sizeof(struct Default) != 16) {
        printf("Error: sizeof(struct Default) is %d, expected 16\n", (int)sizeof(struct Default));
        return 2;
    }

    // Packed2: 1 (char) + 1 (padding) + 8 (int) = 10
    // pack: 2. alignment = min(8, 2) = 2.
    if (sizeof(struct Packed2) != 10) {
        printf("Error: sizeof(struct Packed2) is %d, expected 10\n", (int)sizeof(struct Packed2));
        return 3;
    }

    printf("Pragma pack tests passed!\n");
    return 0;
}
