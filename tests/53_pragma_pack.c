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
    // Packed1: 1 (char) + 4 (int) = 5
    // pack: 1. alignment = min(4, 1) = 1.
    if (sizeof(struct Packed1) != 5) {
        printf("Error: sizeof(struct Packed1) is %d, expected 5\n", (int)sizeof(struct Packed1));
        return 1;
    }

    // Default: 1 (char) + 3 (padding) + 4 (int) = 8
    if (sizeof(struct Default) != 8) {
        printf("Error: sizeof(struct Default) is %d, expected 8\n", (int)sizeof(struct Default));
        return 2;
    }

    // Packed2: 1 (char) + 1 (padding) + 4 (int) = 6
    // pack: 2. alignment = min(4, 2) = 2.
    if (sizeof(struct Packed2) != 6) {
        printf("Error: sizeof(struct Packed2) is %d, expected 6\n", (int)sizeof(struct Packed2));
        return 3;
    }

    printf("Pragma pack tests passed!\n");
    return 0;
}
