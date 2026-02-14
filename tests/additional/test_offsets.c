#include <stdio.h>
#include <stddef.h>
#include "lexer.h"
#include "parser.h"

int main() {
    printf("sizeof(Parser) = %llu\n", (unsigned long long)sizeof(Parser));
    printf("sizeof(Token) = %llu\n", (unsigned long long)sizeof(Token));
    printf("sizeof(Lexer) = %llu\n", (unsigned long long)sizeof(Lexer));
    printf("sizeof(TypedefEntry) = %llu\n", (unsigned long long)sizeof(TypedefEntry));
    printf("sizeof(EnumConstant) = %llu\n", (unsigned long long)sizeof(EnumConstant));
    
    Parser p;
    char *base = (char*)&p;
    printf("offset lexer = %lld\n", (long long)((char*)&p.lexer - base));
    printf("offset current_token = %lld\n", (long long)((char*)&p.current_token - base));
    printf("offset typedefs = %lld\n", (long long)((char*)&p.typedefs - base));
    printf("offset typedefs_count = %lld\n", (long long)((char*)&p.typedefs_count - base));
    printf("offset enum_constants = %lld\n", (long long)((char*)&p.enum_constants - base));
    printf("offset enum_constants_count = %lld\n", (long long)((char*)&p.enum_constants_count - base));
    printf("offset structs = %lld\n", (long long)((char*)&p.structs - base));
    printf("offset structs_count = %lld\n", (long long)((char*)&p.structs_count - base));
    printf("offset locals = %lld\n", (long long)((char*)&p.locals - base));
    printf("offset locals_count = %lld\n", (long long)((char*)&p.locals_count - base));
    printf("offset globals = %lld\n", (long long)((char*)&p.globals - base));
    printf("offset globals_count = %lld\n", (long long)((char*)&p.globals_count - base));
    printf("offset packing_stack = %lld\n", (long long)((char*)&p.packing_stack - base));
    printf("offset packing_ptr = %lld\n", (long long)((char*)&p.packing_ptr - base));
    return 0;
}
