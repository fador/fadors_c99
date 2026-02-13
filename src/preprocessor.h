#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

char *preprocess(const char *source, const char *filename);
void preprocess_define(const char *name, const char *value);

#endif // PREPROCESSOR_H
