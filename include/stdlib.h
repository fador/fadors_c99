#ifndef _STDLIB_H
#define _STDLIB_H

#ifndef _STDDEF_H
typedef unsigned int size_t;
#endif

void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
int atoi(const char *str);
void exit(int status);

#endif
