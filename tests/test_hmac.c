/**
 * test_hmac.c — SHA-256 / HMAC-SHA256 标准向量测试（D1）
 */
#include <stdio.h>
#include <string.h>

#include "ota_hmac.h"

static int fails;

static void check(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL: %s\n  got : %s\n  want: %s\n", name, got, want);
        fails++;
    } else {
        printf("ok  : %s\n", name);
    }
}

int main(void)
{
    char h[65];

    sha256_hex("abc", 3, h, sizeof(h));
    check("sha256(abc)", h,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const char *msg = "The quick brown fox jumps over the lazy dog";
    FILE *fp = fopen("/tmp/gw-hmac-msg.txt", "w");
    if (!fp) { printf("FAIL: open temp\n"); return 1; }
    fputs(msg, fp);
    fclose(fp);

    if (hmac_sha256_file_hex("/tmp/gw-hmac-msg.txt", "key", h, sizeof(h)) != 0) {
        printf("FAIL: hmac file\n");
        fails++;
    } else {
        check("hmac(key, fox)", h,
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    }

    remove("/tmp/gw-hmac-msg.txt");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
