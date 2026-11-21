/**
 * test_json_util.c — 极简 JSON 取值/转义测试
 */
#include <stdio.h>
#include <string.h>
#include "domain/json_util.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    int v = -1;
    char s[32];

    /* 1. 紧凑格式 */
    CHECK(json_get_int("{\"cloud_review\":1}", "cloud_review", &v) == 1 && v == 1,
          "compact int");
    /* 2. 冒号/逗号前后有空白 */
    CHECK(json_get_int("{ \"brightness\" : 80 , \"x\":0 }", "brightness", &v) == 1 && v == 80,
          "spaced int");
    /* 3. 缺字段 */
    CHECK(json_get_int("{\"a\":1}", "cloud_review", &v) == 0, "missing key -> 0");
    /* 4. 负值 */
    CHECK(json_get_int("{\"ps_off\":-12}", "ps_off", &v) == 1 && v == -12, "negative int");

    /* 5. 字符串字段 */
    memset(s, 0, sizeof(s));
    CHECK(json_get_str("{\"mqtt_broker\":\"192.168.3.26\"}", "mqtt_broker", s, sizeof(s)) == 1
          && strcmp(s, "192.168.3.26") == 0, "string value");
    /* 6. 截断保护 */
    memset(s, 0, sizeof(s));
    CHECK(json_get_str("{\"k\":\"abcdefgh\"}", "k", s, 4) == 1 && strcmp(s, "abc") == 0,
          "string truncated to n-1");
    /* 7. 非字符串值 */
    CHECK(json_get_str("{\"k\":123}", "k", s, sizeof(s)) == 0, "non-string -> 0");

    /* 8. 转义 */
    char e[32];
    json_escape("a\"b\\c", e, sizeof(e));
    CHECK(strcmp(e, "a\\\"b\\\\c") == 0, "escape quote and backslash");
    /* 9. 控制字符替换为空格 */
    json_escape("a\nb", e, sizeof(e));
    CHECK(strcmp(e, "a b") == 0, "control char -> space");
    /* 10. NULL 输入 */
    json_escape(NULL, e, sizeof(e));
    CHECK(e[0] == 0, "NULL escaped to empty");
    /* 11. 转义截断有余量时不留半个转义 */
    json_escape("abcdef", e, 4);
    CHECK(strlen(e) == 3, "escape respects buffer size");

    /* 12. 已知限制：同名嵌套键取第一次出现（与旧实现一致） */
    CHECK(json_get_int("{\"a\":{\"zone\":0},\"zone\":1}", "zone", &v) == 1 && v == 0,
          "known limitation: first match wins");
    /* 13. 前缀不误匹配（"zones" 不是 "zone"） */
    CHECK(json_get_int("{\"zones\":[1]}", "zone", &v) == 0, "prefix key not matched");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
