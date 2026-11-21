/**
 * @file lv_linux_fbdev.h
 *
 */

#ifndef LV_LINUX_FBDEV_H
#define LV_LINUX_FBDEV_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../../display/lv_display.h"

#if LV_USE_LINUX_FBDEV

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_display_t * lv_linux_fbdev_create(void);

void lv_linux_fbdev_set_file(lv_display_t * disp, const char * file);

/**
 * Force the display to be refreshed on every change.
 * Expected to be used with LV_DISPLAY_RENDER_MODE_DIRECT or LV_DISPLAY_RENDER_MODE_FULL.
 */
void lv_linux_fbdev_set_force_refresh(lv_display_t * disp, bool enabled);

/** 当前 VSYNC 是否锁定（应用层可据此丢弃摄像头帧，避免撕裂） */
int lv_linux_fbdev_vsync_ok(void);

/**********************
 *      MACROS
 **********************/

#endif /* LV_USE_LINUX_FBDEV */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_LINUX_FBDEV_H */
