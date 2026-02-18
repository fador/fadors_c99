#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

char *preprocess(const char *source, const char *filename);
void preprocess_define(const char *name, const char *value);
void preprocess_add_include_path(const char *path);
void preprocess_reset(void);

#endif // PREPROCESSOR_H
