#ifndef _MY_STRING_H_
#define _MY_STRING_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct string {
    char *buf;
    size_t len;
} string_t;

#define STRING_INIT {NULL, 0}

void str_append(string_t *str, const char *s, size_t len) {
    char *newstr = (char*)realloc(str->buf, str->len + len);
    if (newstr == NULL) return;
    memcpy(&newstr[str->len], s, len);
    str->buf = newstr;
    str->len += len;
    return;
}

void str_free(string_t *str) {
    free(str->buf);
    return;
}

#endif