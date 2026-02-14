int printf(const char *fmt, ...);
int isalpha(int c);
int isspace(int c);

int main() {
    const char *p = "int main";
    int bol = 1;
    int out_pos = 0;
    char output[256];
    printf("before\n");
    while (*p) {
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            bol = 1; continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2; continue;
        }
        if (*p == '"') {
            output[out_pos++] = *p++;
            continue;
        }
        if (*p == '\'') {
            output[out_pos++] = *p++;
            continue;
        }
        
        if (bol && *p == '#') {
            bol = 0;
            p++;
            while (isspace(*p)) p++;
        } else if (isalpha(*p) || *p == '_') {
            bol = 0;
            output[out_pos++] = *p++;
        } else {
            if (*p == '\n') bol = 1;
            output[out_pos++] = *p++;
        }
    }
    output[out_pos] = 0;
    printf("result='%s'\n", output);
    return out_pos;
}
