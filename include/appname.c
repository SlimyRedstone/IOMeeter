#include "appname.h"

#include <string.h>

const char *appname_base(const char *path)
{
    if (path == NULL) {
        return "";
    }

    const char *base = path;

    for (const char *p = path; *p != 0; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

size_t appname_length(const char *name)
{
    if (name == NULL) {
        return 0;
    }

    /*
     * Only these. Cutting at the last dot instead would turn "python3.11"
     * into "python3", which is a different program that may well also be
     * installed.
     */
    static const char *const suffixes[] = {
        ".exe", ".com", ".bat", ".cmd", ".app", ".bin",
    };

    size_t length = strlen(name);

    for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); s++) {
        size_t suffix = strlen(suffixes[s]);

        if (length <= suffix) {
            continue;
        }

        const char *tail = name + length - suffix;
        bool match = true;

        for (size_t i = 0; i < suffix && match; i++) {
            char c = tail[i];

            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            match = (c == suffixes[s][i]);
        }

        if (match) {
            return length - suffix;
        }
    }

    return length;
}

bool appname_same(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    const char *x = appname_base(a);
    const char *y = appname_base(b);

    size_t nx = appname_length(x);
    size_t ny = appname_length(y);

    if (nx != ny || nx == 0) {
        return false;
    }

    for (size_t i = 0; i < nx; i++) {
        char cx = x[i];
        char cy = y[i];

        if (cx >= 'A' && cx <= 'Z') cx = (char)(cx - 'A' + 'a');
        if (cy >= 'A' && cy <= 'Z') cy = (char)(cy - 'A' + 'a');

        if (cx != cy) {
            return false;
        }
    }
    return true;
}
