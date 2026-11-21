/**
 * test_fusion.c — 多源融合/评分测试（D1）
 */
#include <stdio.h>

#include "fusion.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    int score = -1;

    fusion_reset();

    /* 1. 单源：55 分，低于阈值；窗口未过期不定案 */
    fusion_add(0, FUSE_SRC_CAM, 1000, 55);
    CHECK(fusion_score(0, 1200, 3000) == 55, "single source score=55");
    CHECK(fusion_poll(1500, 3000, 70, &score) == -1, "single source not decided yet");

    /* 2. 窗口过期 → 定案（55 分，单源） */
    int z = fusion_poll(5000, 3000, 70, &score);
    CHECK(z == 0, "expired -> decide zone 0");
    CHECK(score == 55, "expired score=55");

    /* 3. 双源共现 → 80 分，达到阈值立即定案 */
    fusion_add(1, FUSE_SRC_CAM, 1000, 55);
    fusion_add(1, FUSE_SRC_SENSOR, 1500, 55);
    CHECK(fusion_score(1, 1600, 3000) == 80, "dual source score=80");
    z = fusion_poll(1600, 3000, 70, &score);
    CHECK(z == 1 && score == 80, "dual source decided immediately");

    /* 4. 定案后分区清空 */
    CHECK(fusion_score(1, 1600, 3000) == 0, "zone cleared after poll");

    /* 5. 强度封顶：单源 100 分也封到 60 */
    fusion_add(2, FUSE_SRC_SENSOR, 2000, 100);
    CHECK(fusion_score(2, 2100, 3000) == 60, "single source capped at 60");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
