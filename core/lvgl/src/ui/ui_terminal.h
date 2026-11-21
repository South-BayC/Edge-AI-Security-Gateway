/**
 * ui_terminal.h — 本地调试终端（无输入框，键盘输入直接回显）
 *
 * 从 ui_home.c 抽出的独立 UI 模块：
 *   输出区 = 历史 + 末尾命令行提示行；软键盘按键即时回显到提示行；
 *   READY（回车）执行命令，CANCEL 收起键盘。
 *
 * 依赖：lvgl + ui_widgets + ui_theme（仅 ui 层）。
 */
#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 在指定容器内构建终端面板（重复调用会重置输出） */
void ui_terminal_build(lv_obj_t *par);

/** 终止子进程 / 关闭管道 / 删除轮询定时器（可重复调用） */
void ui_terminal_kill(void);

/** 关闭终端并清空内部对象指针（页面切换/销毁时调用） */
void ui_terminal_close(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_TERMINAL_H */
