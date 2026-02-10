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

int main() {
    if (sizeof(struct Packed1) != 5) return 100 + (int)sizeof(struct Packed1);
    if (sizeof(struct Default) != 8) return 200 + (int)sizeof(struct Default);
    return 0;
}
