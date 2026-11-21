/**
 * json_util.c — 极简 JSON 取值/转义工具
 */
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

const char *json_value_of(const char *json, const char *key)
{
    if (!json || !key)
        return NULL;

    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(json, needle);
    if (!p)
        return NULL;
    p = json_skip_ws(p + strlen(needle));
    if (*p != ':')
        return NULL;
    return json_skip_ws(p + 1);
}

int json_get_int(const char *json, const char *key, int *out)
{
    const char *p = json_value_of(json, key);
    if (!p || !out)
        return 0;
    *out = atoi(p);
    return 1;
}

int json_get_str(const char *json, const char *key, char *out, int n)
{
    if (!out || n <= 0)
        return 0;

    out[0] = 0;
    const char *p = json_value_of(json, key);
    if (!p || *p != '"')
        return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < n - 1)
        out[i++] = *p++;
    out[i] = 0;
    return 1;
}

void json_escape(const char *in, char *out, int n)
{
    int j = 0;
    if (!in)
        in = "";
    if (!out || n <= 0)
        return;

    for (int i = 0; in[i] && j < n - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= n)
                break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            out[j++] = ' ';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}
