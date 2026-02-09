#include "types.h"
#include <stdlib.h>
#include <string.h>

Type *type_int() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_INT;
    t->size = 8; // simplified
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

Type *type_ptr(Type *to) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_PTR;
    t->size = 8;
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
    t->size = 8; // Enum is effectively int
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
