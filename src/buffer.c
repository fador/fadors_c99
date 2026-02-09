#include "buffer.h"
#include <stdlib.h>
#include <string.h>

void buffer_init(Buffer *buf) {
    buf->capacity = 1024;
    buf->size = 0;
    buf->data = malloc(buf->capacity);
}

void buffer_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

static void buffer_ensure_capacity(Buffer *buf, size_t extra) {
    if (buf->size + extra > buf->capacity) {
        while (buf->size + extra > buf->capacity) {
            buf->capacity *= 2;
        }
        buf->data = realloc(buf->data, buf->capacity);
    }
}

void buffer_write_byte(Buffer *buf, uint8_t byte) {
    buffer_ensure_capacity(buf, 1);
    buf->data[buf->size++] = byte;
}

void buffer_write_word(Buffer *buf, uint16_t word) {
    buffer_ensure_capacity(buf, 2);
    memcpy(buf->data + buf->size, &word, 2);
    buf->size += 2;
}

void buffer_write_dword(Buffer *buf, uint32_t dword) {
    buffer_ensure_capacity(buf, 4);
    memcpy(buf->data + buf->size, &dword, 4);
    buf->size += 4;
}

void buffer_write_qword(Buffer *buf, uint64_t qword) {
    buffer_ensure_capacity(buf, 8);
    memcpy(buf->data + buf->size, &qword, 8);
    buf->size += 8;
}

void buffer_write_bytes(Buffer *buf, const void *data, size_t size) {
    buffer_ensure_capacity(buf, size);
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}

void buffer_pad(Buffer *buf, size_t alignment) {
    size_t pad = (alignment - (buf->size % alignment)) % alignment;
    for (size_t i = 0; i < pad; i++) {
        buffer_write_byte(buf, 0);
    }
}
