#ifndef _TIME_H
#define _TIME_H

#ifndef _STDDEF_H
typedef unsigned int size_t;
#endif

typedef long time_t;

time_t time(time_t *tptr);

#endif
