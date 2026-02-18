#ifndef _STDIO_H
#define _STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int handle;
    int mode;
    int error;
    int eof;
    // Simple buffer (if needed later)
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// File access modes
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, void *args);
int vsnprintf(char *str, size_t size, const char *format, void *args);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int remove(const char *filename);
int rename(const char *oldname, const char *newname);
void perror(const char *s);
int feof(FILE *stream);
int ferror(FILE *stream);

void exit(int status);
int puts(const char *s);
int putchar(int c);

// Memory (implemented in dos_libc.c)
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

// System
int system(const char *command);
char *getenv(const char *name);
int abs(int j);
long labs(long j);
int atoi(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF -1
#define NULL ((void*)0)

#ifdef __cplusplus
}
#endif

#endif
