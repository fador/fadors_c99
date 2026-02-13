/* Exact reproduction of linker relocation arithmetic */
int printf(const char *fmt, ...);
void *malloc(long size);
void *memcpy(void *dest, const void *src, long n);
void *calloc(long nmemb, long size);
int strcmp(const char *s1, const char *s2);
void free(void *p);

typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef long long ssize_t;

typedef struct {
    char    *name;
    uint64_t value;
    int      section;
    uint8_t  binding;
    uint8_t  type;
    uint64_t size;
} LinkSymbol;

typedef struct {
    uint64_t offset;
    int      section;
    uint32_t sym_index;
    uint32_t type;
    int64_t  addend;
} LinkReloc;

typedef struct {
    uint8_t *data;
    ssize_t  size;
    ssize_t  capacity;
} Buffer;

typedef struct {
    Buffer   text;
    Buffer   data;
    ssize_t  bss_size;
    LinkSymbol *symbols;
    ssize_t     sym_count;
    ssize_t     sym_cap;
    LinkReloc  *relocs;
    ssize_t     reloc_count;
    ssize_t     reloc_cap;
} Linker;

int main() {
    /* Setup linker with one symbol and one reloc */
    Linker l;
    
    l.text.data = calloc(64, 1);
    l.text.size = 53;
    l.text.capacity = 64;
    l.data.data = 0;
    l.data.size = 0;
    l.data.capacity = 0;
    l.bss_size = 0;
    
    l.symbols = malloc(sizeof(LinkSymbol) * 4);
    l.sym_count = 1;
    l.sym_cap = 4;
    l.symbols[0].name = ".Lend_0";
    l.symbols[0].value = 4259043;  /* This is the final virtual address after layout */
    l.symbols[0].section = 1;
    l.symbols[0].binding = 0;
    l.symbols[0].type = 0;
    l.symbols[0].size = 0;
    
    l.relocs = malloc(sizeof(LinkReloc) * 4);
    l.reloc_count = 1;
    l.reloc_cap = 4;
    l.relocs[0].offset = 47;
    l.relocs[0].section = 1;
    l.relocs[0].sym_index = 0;
    l.relocs[0].type = 4;  /* PLT32 */
    l.relocs[0].addend = -4;
    
    uint64_t text_vaddr = 4194480;  /* 0x4000b0 */
    uint64_t data_vaddr = 4198400;
    
    /* Expected: S=0x40fce3, P = text_vaddr + 47 = 0x4000b0 + 0x2f = 0x4000df */
    /* val = (0x40fce3 + 0xfffffffffffffffc) - 0x4000df = 0x40fcdf - 0x4000df = 0xfc00 = 64512 */
    /* Actually let me fix this: set S so that S + A - P = 0 */
    /* P = 0x4000b0 + 47 = 0x4000df. A = -4. So S + A - P = 0 means S = P - A = 0x4000df + 4 = 0x4000e3 */
    l.symbols[0].value = 0x4000e3;
    
    /* Apply relocation - exact copy of linker.c */
    ssize_t i;
    for (i = 0; i < l.reloc_count; i++) {
        LinkReloc *r = &l.relocs[i];
        
        uint64_t S = l.symbols[r->sym_index].value;
        int64_t  A = r->addend;
        uint64_t P;
        unsigned char *patch;
        
        if (r->section == 1) {
            P     = text_vaddr + r->offset;
            patch = l.text.data + r->offset;
        } else {
            P     = data_vaddr + r->offset;
            patch = l.data.data + r->offset;
        }
        
        printf("S=0x%llx A=%lld P=0x%llx\n", S, A, P);
        
        int64_t val = (int64_t)(S + (uint64_t)A) - (int64_t)P;
        printf("val=%lld\n", val);
        
        if (val != 0) {
            printf("FAIL: expected 0, got %lld\n", val);
            free(l.text.data);
            free(l.symbols);
            free(l.relocs);
            return 1;
        }
    }
    
    printf("PASS\n");
    free(l.text.data);
    free(l.symbols);
    free(l.relocs);
    return 42;
}
