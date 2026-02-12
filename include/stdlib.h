#ifndef _STDLIB_H
#define _STDLIB_H

#ifndef _STDDEF_H
typedef unsigned int size_t;
#endif

void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
int atoi(const char *str);
long strtol(const char *str, char **endptr, int base);
unsigned long strtoul(const char *str, char **endptr, int base);
void exit(int status);

#endif
