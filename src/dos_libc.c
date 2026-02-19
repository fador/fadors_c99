#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// -----------------------------------------------------------------------------
// Assembly Syscalls (from dos_lib.s)
// -----------------------------------------------------------------------------
extern int _dos_open(const char *path, int mode);
extern int _dos_creat(const char *path, int attr);
extern int _dos_read(int handle, void *buf, int count);
extern int _dos_write(int handle, const void *buf, int count);
extern int _dos_close(int handle);
extern long _dos_lseek(int handle, long offset, int whence);
extern int _dos_delete(const char *path);
extern int _dos_rename(const char *old, const char *new);

// -----------------------------------------------------------------------------
// Memory Management (Static Heap)
// -----------------------------------------------------------------------------
#define HEAP_SIZE (8 * 1024) // 8KB heap
static char heap_memory[HEAP_SIZE];
static size_t heap_ptr = 0;

typedef struct BlockHeader {
    size_t size;
    int is_free;
    struct BlockHeader *next;
} BlockHeader;

static BlockHeader *free_list = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    
    // Align to 8 bytes
    size = (size + 7) & ~7;
    
    if (free_list == NULL) {
        puts("malloc: init heap");
        // Init heap
        free_list = (BlockHeader *)heap_memory;
        free_list->size = HEAP_SIZE - sizeof(BlockHeader);
        free_list->is_free = 1;
        free_list->next = NULL;
    }
    
    BlockHeader *curr = free_list;
    // printf("DEBUG: malloc %d, free_list=%x heap=%x\n", size, (int)free_list, (int)heap_memory);
    BlockHeader *prev = NULL;
    
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            // Found a block
            if (curr->size >= size + sizeof(BlockHeader) + 16) {
                // Split
                BlockHeader *new_block = (BlockHeader *)((char *)curr + sizeof(BlockHeader) + size);
                new_block->size = curr->size - size - sizeof(BlockHeader);
                new_block->is_free = 1;
                new_block->next = curr->next;
                
                curr->size = size;
                curr->next = new_block;
            }
            curr->is_free = 0;
            puts("malloc: returning block");
            return (void *)((char *)curr + sizeof(BlockHeader));
        }
        prev = curr;
        curr = curr->next;
    }
    
    return NULL; // OOM
}

void free(void *ptr) {
    if (!ptr) return;
    BlockHeader *block = (BlockHeader *)((char *)ptr - sizeof(BlockHeader));
    block->is_free = 1;
    
    // Coalesce? Not strictly needed for a simple compiler run, but good practice.
    // For now, keep it simple to avoid bugs.
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    
    BlockHeader *block = (BlockHeader *)((char *)ptr - sizeof(BlockHeader));
    if (block->size >= size) return ptr; // Shrink or same? Just return.
    
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

// -----------------------------------------------------------------------------
// File I/O
// -----------------------------------------------------------------------------

// Pre-allocated FILE objects
static FILE file_pool[20];
FILE *stdin = &file_pool[0];
FILE *stdout = &file_pool[1];
FILE *stderr = &file_pool[2];

void _init_stdio() {
    stdin->handle = 0; stdin->mode = O_RDONLY;
    stdout->handle = 1; stdout->mode = O_WRONLY;
    stderr->handle = 2; stderr->mode = O_WRONLY;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    int rw = 0;
    if (strchr(mode, 'r')) { flags = 0; rw = O_RDONLY; }
    if (strchr(mode, 'w')) { flags = 1; rw = O_WRONLY; }
    // if (strchr(mode, '+')) ... simplistic for now
    
    int handle = -1;
    if (flags == 0) { // Read
        handle = _dos_open(path, 0); // Open RO
    } else { // Write
        handle = _dos_creat(path, 0); // Create Normal
    }
    
    // printf("DEBUG: fopen handle=%d\n", handle);
    if (handle < 0) return NULL;
    
    // Find free FILE
    for (int i = 3; i < 20; i++) {
        if (file_pool[i].handle == 0) { // Assumes 0 is stdin, used check
            FILE *f = &file_pool[i];
            f->handle = handle;
            f->mode = rw;
            f->error = 0;
            f->eof = 0;
            return f;
        }
    }
    _dos_close(handle);
    return NULL;
}

int fclose(FILE *stream) {
    if (!stream || stream->handle < 3) return 0; // Don't close stdio?
    _dos_close(stream->handle);
    stream->handle = 0;
    return 0;
}

int fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) return 0;
    size_t total = size * nmemb;
    int r = _dos_read(stream->handle, ptr, total);
    if (r < 0) { stream->error = 1; return 0; }
    if (r == 0) { stream->eof = 1; return 0; }
    return r / size; // Short read?
}

int fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) return 0;
    size_t total = size * nmemb;
    int r = _dos_write(stream->handle, ptr, total);
    if (r < 0) { stream->error = 1; return 0; }
    return r / size;
}

int fgetc(FILE *stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) == 1) return c;
    return EOF;
}

char *fgets(char *s, int n, FILE *stream) {
    if (n <= 0) return NULL;
    int i = 0;
    int c;
    while (i < n - 1) {
        c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE *stream) {
    unsigned char uc = c;
    if (fwrite(&uc, 1, 1, stream) == 1) return uc;
    return EOF;
}

int fputs(const char *s, FILE *stream) {
    return fwrite(s, 1, strlen(s), stream);
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    long r = _dos_lseek(stream->handle, offset, whence);
    if (r < 0) return -1;
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) return -1;
    // Seek 0 from current to get pos
    return _dos_lseek(stream->handle, 0, SEEK_CUR);
}

int fflush(FILE *stream) { return 0; } // DOS writes immediately mostly
int feof(FILE *stream) { return stream ? stream->eof : 1; }
int ferror(FILE *stream) { return stream ? stream->error : 1; }
int remove(const char *filename) { return _dos_delete(filename); }
int rename(const char *oldname, const char *newname) { return _dos_rename(oldname, newname); }
void perror(const char *s) { printf("%s: error\n", s); }

// -----------------------------------------------------------------------------
// Strings & Formatting
// -----------------------------------------------------------------------------
// Minimal vsnprintf implementation
static void num_to_str(char **out, size_t *rem, unsigned long val, int base) {
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    else {
        while (val) {
            int d = val % base;
            buf[i++] = (d < 10) ? '0'+d : 'a'+(d-10);
            val /= base;
        }
    }
    while (i > 0) {
        if (*rem > 1) { *((*out)++) = buf[--i]; (*rem)--; } else { --i; }
    }
}

int vsnprintf(char *str, size_t size, const char *format, void *args) {
    // Requires stdarg handling which is tricky if built with a compiler that doesn't support builtins.
    // Our compiler uses standard stack passing, so void* args usually points to first stack arg.
    // But standard `vsnprintf` takes `va_list`.
    // Since we are self-hosting, we know our backend passes args on stack.
    // Let's assume `args` IS `va_list` (char*).

    if (!str || size == 0) return 0;
    char *out = str;
    size_t rem = size;
    char *arg_ptr = (char *)args;
    
    #define NEXT_INT() (*(int*)((arg_ptr += 4) - 4))
    #define NEXT_PTR() (*(char**)((arg_ptr += 4) - 4))

    const char *f = format;
    while (*f && rem > 1) {
        if (*f != '%') {
            *out++ = *f++; rem--;
            continue;
        }
        f++;
        if (*f == 'd') {
            int v = NEXT_INT();
            if (v < 0) { 
                if (rem > 1) { *out++ = '-'; rem--; }
                num_to_str(&out, &rem, -v, 10);
            } else {
                num_to_str(&out, &rem, v, 10);
            }
        } else if (*f == 'x') {
            num_to_str(&out, &rem, NEXT_INT(), 16);
        } else if (*f == 's') {
            char *s = NEXT_PTR();
            while (*s && rem > 1) { *out++ = *s++; rem--; }
        } else if (*f == 'c') {
            *out++ = (char)NEXT_INT(); rem--;
        } else if (*f == '%') {
            *out++ = '%'; rem--;
        }
        f++;
    }
    *out = 0;
    return out - str;
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = vsnprintf(str, 0xFFFF, format, args);
    va_end(args);
    return r;
}

int fprintf(FILE *stream, const char *format, ...) {
    if (!stream) return 0;
    char buf[1024];
    va_list args;
    va_start(args, format);
    int r = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return fwrite(buf, 1, r, stream);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = vsnprintf(str, size, format, args);
    va_end(args);
    return r;
}

// -----------------------------------------------------------------------------
// Stubbed System Functions
// -----------------------------------------------------------------------------
int system(const char *command) { return -1; }
char *getenv(const char *name) { return NULL; }
int abs(int j) { return j < 0 ? -j : j; }
long labs(long j) { return j < 0 ? -j : j; }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

long strtol(const char *nptr, char **endptr, int base) {
    long res = 0;
    int sign = 1;
    while (isspace(*nptr)) nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    
    // Auto-base
    if (base == 0) {
        if (*nptr == '0') {
            if (tolower(nptr[1]) == 'x') { base = 16; nptr += 2; }
            else { base = 8; }
        } else {
            base = 10;
        }
    }
    
    while (*nptr) {
        int v;
        if (isdigit(*nptr)) v = *nptr - '0';
        else if (isalpha(*nptr)) v = tolower(*nptr) - 'a' + 10;
        else break;
        
        if (v >= base) break;
        res = res * base + v;
        nptr++;
    }
    
    if (endptr) *endptr = (char *)nptr;
    return res * sign;
}
// -----------------------------------------------------------------------------
// String & Memory Functions (Standard Library)
// -----------------------------------------------------------------------------

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n) {
    char *p = (char *)s;
    while (n--) *p++ = (char)c;
    return s;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *new = malloc(len);
    if (new) memcpy(new, s, len);
    return new;
}
