#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>

typedef enum {
    TYPE_INT,
    TYPE_PTR,
    TYPE_STRUCT,
    TYPE_VOID
} TypeKind;

struct Type;

typedef struct Member {
    char *name;
    struct Type *type;
    int offset;
} Member;

typedef struct Type {
    TypeKind kind;
    int size;
    union {
        struct Type *ptr_to;
        struct {
            char *name;
            Member *members;
            int members_count;
        } struct_data;
    } data;
} Type;

Type *type_int();
Type *type_ptr(Type *to);
Type *type_struct(const char *name);

#endif // TYPES_H
