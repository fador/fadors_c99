/* Test 64-bit arithmetic with structs like linker code */
int printf(const char *fmt, ...);
void *malloc(long size);

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;

typedef struct {
    uint64_t offset;
    int section;
    uint32_t sym_index;
    uint32_t type;
    int64_t addend;
} LinkReloc;

typedef struct {
    uint64_t value;
} LinkSym;

void *memcpy(void *dest, const void *src, long n);

int main() {
    LinkReloc reloc;
    reloc.offset = 47;
    reloc.section = 1;
    reloc.sym_index = 0;
    reloc.type = 4;
    reloc.addend = -4;

    LinkSym sym;
    sym.value = 4259043;  /* 0x4000e3 */

    LinkReloc *r = &reloc;
    uint64_t text_vaddr = 4194480;  /* 0x4000b0 */

    uint64_t S = sym.value;
    int64_t A = r->addend;
    uint64_t P = text_vaddr + r->offset;

    uint64_t SA = S + (uint64_t)A;
    int64_t val = (int64_t)SA - (int64_t)P;

    printf("S=%ld A=%ld SA=%ld P=%ld val=%ld\n", S, A, SA, P, val);

    int32_t v32 = (int32_t)val;
    printf("v32=%d\n", v32);

    unsigned char patch[4];
    memcpy(patch, &v32, 4);
    printf("bytes: %02x %02x %02x %02x\n", patch[0], patch[1], patch[2], patch[3]);

    /* Also test byte extraction */
    unsigned char b0 = (unsigned char)(val & 255);
    unsigned char b1 = (unsigned char)((val >> 8) & 255);
    unsigned char b2 = (unsigned char)((val >> 16) & 255);
    unsigned char b3 = (unsigned char)((val >> 24) & 255);
    printf("shift bytes: %02x %02x %02x %02x\n", b0, b1, b2, b3);

    if (val != 0) {
        printf("FAIL: val expected 0, got %ld\n", val);
        return 1;
    }
    printf("PASS\n");
    return 42;
}
