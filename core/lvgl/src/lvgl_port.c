/**
 * lvgl_port.c — LVGL 板级移植层（实现）
 *
 * 完成两件事：
 *   1) 显示：lv_linux_fbdev_create() + lv_linux_fbdev_set_file("/dev/fb0")
 *      把 LVGL 渲染输出到真实屏幕的 framebuffer；
 *   2) 触摸：find_touch_dev() 在 /dev/input 下按"设备名字含 gt9147/goodix/gt9"
 *      过滤，动态找到触摸屏对应的 eventX（不能写死 event0/1，因为 event 编号
 *      随驱动加载顺序变化），再交给 lv_evdev_create() 处理触摸事件。
 *
 * 系统时钟：用 clock_gettime(CLOCK_MONOTONIC) 提供毫秒 tick，供 LVGL 定时器用。
 */
#include "lvgl_port.h"
#include "log_svc.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>

static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;

// 系统 tick 回调（毫秒）
static uint32_t tick_get_cb(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ================================================================
 * 触摸设备探测：遍历 /dev/input/eventX，按驱动名匹配 gt9147/goodix。
 * KEY0(gpio-keys) 与触摸都是 input 设备且 event 号随加载顺序变化，
 * 固定 eventN 会绑错设备，必须按名字过滤。
 * 找到则把路径写入 out_path 并返回 0，否则 -1。
 * ================================================================ */
static int find_touch_dev(char *out_path, size_t out_len)
{
    static const char *KEYWORDS[] = { "gt9147", "goodix", "gt9", NULL };

    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    char name[256];
    int ret = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(out_path, out_len, "/dev/input/%s", ent->d_name);
        int fd = open(out_path, O_RDONLY);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
            for (int i = 0; KEYWORDS[i]; i++) {
                if (strstr(name, KEYWORDS[i]) != NULL) {
                    close(fd);
                    ret = 0;
                    goto done;
                }
            }
        }
        close(fd);
    }
done:
    closedir(dir);
    return ret;
}

int lvgl_port_init(void) {
    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_disp = lv_linux_fbdev_create();
    if (!s_disp) {
        LOGE("LVGL", "fbdev create failed");
        return LVGL_PORT_ERR_FBDEV_OPEN;
    }
    lv_linux_fbdev_set_file(s_disp, "/dev/fb0");
    LOGI("LVGL", "fbdev /dev/fb0 ready (1024x600)");

    /* 触摸：自动探测（名字含 gt9147/goodix/gt9），不依赖固定 event 号 */
    char touch_path[64] = {0};
    if (find_touch_dev(touch_path, sizeof(touch_path)) != 0) {
        LOGW("LVGL", "touch device not found (gt9147 loaded?), touch disabled");
        return LVGL_PORT_ERR_OK;
    }

    s_indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    if (!s_indev) {
        LOGE("LVGL", "evdev create failed (%s)", touch_path);
        return LVGL_PORT_ERR_OK;
    }
    lv_indev_set_display(s_indev, s_disp);
    LOGI("LVGL", "touch ready: %s", touch_path);
    return LVGL_PORT_ERR_OK;
}

void lvgl_port_deinit(void) {
    // LVGL v9 由内部管理，暂无需显式释放
}

int lvgl_port_vsync_ok(void)
{
    if (!s_disp)
        return 1;   /* 模拟器/未初始化：不做限制 */
    return lv_linux_fbdev_vsync_ok();
}
