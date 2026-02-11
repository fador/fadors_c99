#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>

typedef enum {
    TYPE_INT,
    TYPE_SHORT,
    TYPE_LONG,
    TYPE_LONG_LONG,
    TYPE_CHAR,
    TYPE_PTR,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ENUM,
    TYPE_ARRAY,
    TYPE_VOID,
    TYPE_FLOAT,
    TYPE_DOUBLE
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
    int array_len;
    union {
        struct Type *ptr_to; // base type for pointers and arrays
        struct {
            char *name;
            Member *members;
            int members_count;
        } struct_data;
    } data;
} Type;

Type *type_int();
Type *type_short();
Type *type_long();
Type *type_long_long();
Type *type_char();
Type *type_float();
Type *type_double();
Type *type_ptr(Type *to);
Type *type_array(Type *base, int len);
Type *type_struct(const char *name);
Type *type_union(const char *name);
Type *type_enum(const char *name);
Type *type_void();

#endif // TYPES_H
