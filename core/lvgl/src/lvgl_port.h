/**
 * lvgl_port.h — LVGL 板级移植层（头文件）
 *
 * 负责把 LVGL 图形库接到真实的 Linux 硬件上：
 *   - 显示：使用 /dev/fb0（framebuffer，1024x600 RGB 屏），对应 lv_linux_fbdev；
 *   - 触摸：自动探测 /dev/input/eventX 中名字含 gt9147/goodix 的电容触摸设备，
 *           对应 lv_evdev。
 * 业务代码只调用 lvgl_port_init()/lvgl_port_deinit()，不用关心底层设备细节。
 */
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 移植层错误码：0=成功；负数=具体失败原因 */
#define LVGL_PORT_ERR_OK           0   /* 成功 */
#define LVGL_PORT_ERR_FBDEV_OPEN  -4   /* 打开 framebuffer 设备 /dev/fb0 失败 */
#define LVGL_PORT_ERR_EVDEV_OPEN  -5   /* 打开触摸事件设备失败 */

/*
 * 初始化 LVGL 及显示/触摸移植
 * @return 0=成功（含触摸未找到但显示正常的降级成功）；负数为致命错误
 */
int lvgl_port_init(void);

/* 反初始化（LVGL v9 内部管理资源，当前保持空实现） */
void lvgl_port_deinit(void);

/**
 * 显示是否处于可安全刷新状态（VSYNC 锁相正常）。
 * 供 UI 层做撕裂保护，避免直接包含 LVGL 驱动头。
 * @return 1=可刷新 0=暂不可刷新（息屏/唤醒/VSYNC 异常）
 */
int lvgl_port_vsync_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
