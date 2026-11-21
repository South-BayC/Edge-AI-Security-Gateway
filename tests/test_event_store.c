/**
 * test_event_store.c — 事件缓存测试（D1）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "event_store.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    char line[256];

    system("rm -rf /tmp/gw-es");

    CHECK(event_store_init("/tmp/gw-es") == 0, "init");
    CHECK(event_store_append("{\"id\":\"A\",\"v\":1}") == 0, "append A");
    CHECK(event_store_append("{\"id\":\"B\",\"v\":2}") == 0, "append B");
    CHECK(event_store_append("{\"id\":\"A\",\"v\":3}") == 0, "append dup A");
    CHECK(event_store_count() == 2, "dedup count=2");

    CHECK(event_store_get(0, line, sizeof(line)) == 0, "get 0");
    CHECK(strstr(line, "\"A\"") != NULL, "oldest is A");

    CHECK(event_store_remove("A") == 0, "remove A");
    CHECK(event_store_count() == 1, "count=1 after remove");

    event_store_close();

    /* 重载 */
    CHECK(event_store_init("/tmp/gw-es") == 0, "reload init");
    CHECK(event_store_count() == 1, "reload count=1");
    CHECK(event_store_get(0, line, sizeof(line)) == 0 && strstr(line, "\"B\"") != NULL,
          "reload keeps B");

    const char *p = event_store_path();
    struct stat st;
    CHECK(stat(p, &st) == 0, "file exists");
    CHECK((st.st_mode & 0777) == 0600, "file mode 600");

    event_store_close();

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
