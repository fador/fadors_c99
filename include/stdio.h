#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

typedef struct _FILE {
    int handle;
    int mode;
    int error;
    int eof;
} FILE;

#ifdef _WIN32
extern FILE *__acrt_iob_func(int);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#else
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#endif

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
int fputs(const char *s, FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int snprintf(char *str, size_t size, const char *format, ...);
int sscanf(const char *str, const char *format, ...);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EOF -1

#endif
