#include "types.h"
#include <stdlib.h>
#include <string.h>

/* 0 = LP64 (Linux: long=8), 1 = LLP64 (Windows: long=4) */
static int target_is_windows = 0;
static int target_is_32bit = 0;

void types_set_target(int is_windows, int is_32bit) {
    target_is_windows = is_windows;
    target_is_32bit = is_32bit;
}

Type *type_int() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_INT;
    t->size = 4;
    t->array_len = 0;
    return t;
}

Type *type_short() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_SHORT;
    t->size = 2;
    t->array_len = 0;
    return t;
}

Type *type_long() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_LONG;
    t->size = (target_is_windows || target_is_32bit) ? 4 : 8;  /* LLP64/ILP32 vs LP64 */
    t->array_len = 0;
    return t;
}

Type *type_long_long() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_LONG_LONG;
    t->size = 8;
    t->array_len = 0;
    return t;
}

Type *type_char() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_CHAR;
    t->size = 1;
    t->array_len = 0;
    return t;
}

Type *type_float() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_FLOAT;
    t->size = 4;
    t->array_len = 0;
    return t;
}

Type *type_double() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_DOUBLE;
    t->size = 8;
    t->array_len = 0;
    return t;
}

Type *type_ptr(Type *to) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_PTR;
    t->size = target_is_32bit ? 4 : 8;
    t->data.ptr_to = to;
    t->array_len = 0;
    return t;
}

Type *type_array(Type *base, int len) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_ARRAY;
    t->array_len = len;
    t->data.ptr_to = base;
    t->size = base->size * len;
    return t;
}

static Type *structs[100];
static int structs_count = 0;

Type *type_struct(const char *name) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_STRUCT;
    t->size = 0;
    t->array_len = 0;
    t->data.struct_data.name = name ? strdup(name) : NULL;
    t->data.struct_data.members = NULL;
    t->data.struct_data.members_count = 0;
    return t;
}

Type *type_union(const char *name) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_UNION;
    t->size = 0;
    t->array_len = 0;
    t->data.struct_data.name = name ? strdup(name) : NULL;
    t->data.struct_data.members = NULL;
    t->data.struct_data.members_count = 0;
    return t;
}

Type *type_enum(const char *name) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_ENUM;
    t->size = 4; // Enum is effectively int (4 bytes on LLP64)
    t->array_len = 0;
    t->data.struct_data.name = name ? strdup(name) : NULL;
    t->data.struct_data.members = NULL;
    t->data.struct_data.members_count = 0;
    return t;
}

Type *type_void() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_VOID;
    t->size = 0;
    t->array_len = 0;
    return t;
}
