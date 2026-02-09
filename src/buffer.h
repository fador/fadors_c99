#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Buffer;

void buffer_init(Buffer *buf);
void buffer_free(Buffer *buf);
void buffer_write_byte(Buffer *buf, uint8_t byte);
void buffer_write_word(Buffer *buf, uint16_t word);
void buffer_write_dword(Buffer *buf, uint32_t dword);
void buffer_write_qword(Buffer *buf, uint64_t qword);
void buffer_write_bytes(Buffer *buf, const void *data, size_t size);
void buffer_pad(Buffer *buf, size_t alignment);

#endif
