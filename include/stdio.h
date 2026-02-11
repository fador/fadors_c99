#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

typedef void FILE;

extern FILE *__acrt_iob_func(int);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, int offset, int whence);
int ftell(FILE *stream);
int fflush(FILE *stream);
int feof(FILE *stream);
int puts(const char *s);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EOF -1

#endif
