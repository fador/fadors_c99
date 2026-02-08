#include "types.h"
#include <stdlib.h>
#include <string.h>

Type *type_int() {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_INT;
    t->size = 8; // simplified
    return t;
}

Type *type_ptr(Type *to) {
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_PTR;
    t->size = 8;
    t->data.ptr_to = to;
    return t;
}

static Type *structs[100];
static int structs_count = 0;

Type *type_struct(const char *name) {
    for (int i = 0; i < structs_count; i++) {
        if (strcmp(structs[i]->data.struct_data.name, name) == 0) {
            return structs[i];
        }
    }
    Type *t = malloc(sizeof(Type));
    t->kind = TYPE_STRUCT;
    t->size = 0;
    t->data.struct_data.name = strdup(name);
    t->data.struct_data.members = malloc(sizeof(Member) * 10);
    t->data.struct_data.members_count = 0;
    structs[structs_count++] = t;
    return t;
}
