#ifndef _STDARG_H
#define _STDARG_H

typedef char *va_list;

// Simple macros for stack-based arguments (x86 32-bit push)
// Arguments are pushed right-to-left. Stack grows down.
// Last fixed arg is at lower address than first variable arg.
// So va_start points to address after last fixed arg.

#define _INTSIZEOF(n)   ((sizeof(n) + sizeof(int) - 1) & -sizeof(int))

#define va_start(ap, v)  (ap = (va_list)&v + _INTSIZEOF(v))
#define va_arg(ap, t)    (*(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)))
#define va_end(ap)       (ap = (va_list)0)

#endif
