#ifndef _STDIO_H
#define _STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char *format, ...);
int puts(const char *s);
int putchar(int c);
void exit(int status);

#ifdef __cplusplus
}
#endif

#endif
