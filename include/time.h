#ifndef _TIME_H
#define _TIME_H

#ifndef _STDDEF_H
typedef unsigned int size_t;
#endif

typedef long time_t;

#ifdef _WIN32
time_t _time64(time_t *tptr);
#define time _time64
#else
time_t time(time_t *tptr);
#endif

#endif
