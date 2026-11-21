/**
 * sim_main.c — 智能安防网关 UI 的 PC 模拟器入口（开发调试用，板端不编译）
 *
 * 与 main.c 用真实 fbdev/evdev 不同，这里用 LVGL 的 SDL 驱动在 PC 上弹一个
 * 1024x600 的窗口来模拟屏幕，鼠标模拟触摸，方便在没有开发板的开发机上
 * 快速预览/调试 UI 布局与交互，再同步到板端。
 */
#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include "log_svc.h"
#include <unistd.h>
#include <stdio.h>

// 复用主 UI 创建（与 main.c 保持一致，简版）
extern void ui_home_create(lv_obj_t *parent);

int main(void) {
    log_init(NULL);
    LOGI("SIM", "simulator start");

    lv_init();
    lv_sdl_window_create(1024, 600);
    lv_sdl_mouse_create();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
    ui_home_create(scr);

    LOGI("SIM", "1024x600 安防网关模拟器已启动，按 Ctrl+C 退出");
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
