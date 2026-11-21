/**
 * ui_terminal.c — 本地调试终端（无输入框：键盘输入直接回显到终端）
 *
 * 交互：
 *   - 输出区显示命令历史，最后一行是命令行提示（$ + 正在输入的内容）；
 *   - 软键盘直接书写，按键即时回显到提示行，退格/左右键同样生效；
 *   - READY（回车）执行命令；CANCEL 收起键盘；
 *   - 隐藏 textarea 仅作键盘缓冲，不显示在界面上。
 */
#include "ui_terminal.h"
#include "ui_widgets.h"
#include "ui_theme.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TERM_BUF_MAX 4096
#define TERM_CMD_MAX 96

static char g_term_buf[TERM_BUF_MAX];
static int  g_term_len = 0;
static lv_obj_t *g_term_out_box = NULL;
static lv_obj_t *g_term_out_lbl = NULL;   /* 历史输出 */
static lv_obj_t *g_term_cmd_lbl = NULL;   /* 命令行提示行 */
static lv_obj_t *g_term_buf_ta = NULL;    /* 隐藏的键盘缓冲 textarea */
static lv_obj_t *g_term_kb = NULL;
static lv_obj_t *g_term_qrow = NULL;      /* 快捷命令行 */
static lv_obj_t *g_term_btnrow = NULL;    /* 键盘/清屏行 */
static lv_timer_t *g_term_poll = NULL;
static int   g_term_fd = -1;
static pid_t g_term_pid = -1;

/* 输出区滚到底部（flex 列布局：历史在上，提示行在末尾） */
static void _term_scroll_bottom(void)
{
    if (!g_term_out_box) return;
    lv_obj_update_layout(g_term_out_box);
    lv_obj_scroll_to_y(g_term_out_box, LV_COORD_MAX, LV_ANIM_OFF);
}

/* 历史滚动缓冲：追加文本并刷新输出 */
static void _term_append(const char *s)
{
    if (!s || !s[0]) return;
    int n = (int)strlen(s);
    if (g_term_len + n >= TERM_BUF_MAX) {
        int drop = g_term_len + n - TERM_BUF_MAX + 512;
        if (drop > g_term_len) drop = g_term_len;
        memmove(g_term_buf, g_term_buf + drop, (size_t)(g_term_len - drop));
        g_term_len -= drop;
    }
    memcpy(g_term_buf + g_term_len, s, (size_t)n);
    g_term_len += n;
    g_term_buf[g_term_len] = 0;

    if (g_term_out_lbl) {
        lv_label_set_text(g_term_out_lbl, g_term_buf);
        _term_scroll_bottom();
    }
}

/* 命令行提示行 = "$ " + 键盘缓冲内容 */
static void _term_prompt_refresh(void)
{
    const char *txt = g_term_buf_ta ? lv_textarea_get_text(g_term_buf_ta) : "";
    if (g_term_cmd_lbl) {
        char line[TERM_CMD_MAX + 8];
        snprintf(line, sizeof(line), "$ %s", txt ? txt : "");
        lv_label_set_text(g_term_cmd_lbl, line);
        _term_scroll_bottom();
    }
}

void ui_terminal_kill(void)
{
    if (g_term_fd >= 0) {
        close(g_term_fd);
        g_term_fd = -1;
    }
    if (g_term_pid > 0) {
        kill(g_term_pid, SIGKILL);
        waitpid(g_term_pid, NULL, WNOHANG);
        g_term_pid = -1;
    }
    if (g_term_poll) {
        lv_timer_delete(g_term_poll);
        g_term_poll = NULL;
    }
}

void ui_terminal_close(void)
{
    ui_terminal_kill();
    g_term_out_box = NULL;
    g_term_out_lbl = NULL;
    g_term_cmd_lbl = NULL;
    g_term_buf_ta = NULL;
    g_term_kb = NULL;
    g_term_qrow = NULL;
    g_term_btnrow = NULL;
    g_term_len = 0;
    g_term_buf[0] = 0;
}

static void _term_poll_cb(lv_timer_t *t)
{
    if (g_term_fd < 0) {
        lv_timer_delete(t);
        g_term_poll = NULL;
        return;
    }

    char buf[256];
    for (;;) {
        ssize_t n = read(g_term_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            _term_append(buf);
            continue;
        }
        if (n == 0) {
            close(g_term_fd);
            g_term_fd = -1;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        if (errno == EINTR)
            continue;
        close(g_term_fd);
        g_term_fd = -1;
        break;
    }

    if (g_term_fd < 0) {
        if (g_term_pid > 0) {
            waitpid(g_term_pid, NULL, WNOHANG);
            g_term_pid = -1;
        }
        lv_timer_delete(t);
        g_term_poll = NULL;
    }
}

static void _term_run(const char *cmd)
{
    if (!cmd || !cmd[0])
        return;

    ui_terminal_kill();   /* 上一条未结束则终止，避免并发 shell */

    _term_append("$ ");
    _term_append(cmd);
    _term_append("\n");

    int p[2];
    if (pipe(p) != 0) {
        _term_append("pipe failed\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        _term_append("fork failed\n");
        return;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]);
        close(p[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(p[1]);
    int fl = fcntl(p[0], F_GETFL, 0);
    if (fl >= 0)
        fcntl(p[0], F_SETFL, fl | O_NONBLOCK);

    g_term_fd = p[0];
    g_term_pid = pid;
    g_term_poll = lv_timer_create(_term_poll_cb, 100, NULL);
}

static void _term_kb_hide(void)
{
    if (g_term_kb)
        lv_obj_add_flag(g_term_kb, LV_OBJ_FLAG_HIDDEN);
    /* 键盘收起后恢复按钮行/快捷命令行（弹出时隐藏，避免被遮挡） */
    if (g_term_btnrow)
        lv_obj_clear_flag(g_term_btnrow, LV_OBJ_FLAG_HIDDEN);
    if (g_term_qrow)
        lv_obj_clear_flag(g_term_qrow, LV_OBJ_FLAG_HIDDEN);
}

/* 键盘按键 → 缓冲变化 → 提示行即时回显 */
static void _on_kb_buf_changed(lv_event_t *e)
{
    (void)e;
    _term_prompt_refresh();
}

static void _on_term_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        const char *txt = g_term_buf_ta ? lv_textarea_get_text(g_term_buf_ta) : "";
        if (txt && txt[0])
            _term_run(txt);
        if (g_term_buf_ta)
            lv_textarea_set_text(g_term_buf_ta, "");
        _term_prompt_refresh();
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        _term_kb_hide();
}

static void _on_term_kb_btn(lv_event_t *e)
{
    (void)e;
    if (!g_term_kb) return;

    /* 键盘（200px）会覆盖按钮行/快捷行，先隐藏它们腾出空间 */
    if (g_term_btnrow)
        lv_obj_add_flag(g_term_btnrow, LV_OBJ_FLAG_HIDDEN);
    if (g_term_qrow)
        lv_obj_add_flag(g_term_qrow, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(g_term_kb, LV_OBJ_FLAG_HIDDEN);

    if (g_term_buf_ta) {
        lv_obj_add_state(g_term_buf_ta, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(g_term_buf_ta, LV_TEXTAREA_CURSOR_LAST);
    }
    _term_prompt_refresh();
}

static void _on_term_clear(lv_event_t *e)
{
    (void)e;
    g_term_len = 0;
    g_term_buf[0] = 0;
    if (g_term_out_lbl)
        lv_label_set_text(g_term_out_lbl, "");
    if (g_term_buf_ta)
        lv_textarea_set_text(g_term_buf_ta, "");
    _term_prompt_refresh();
}

static void _on_term_quick(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    _term_run(cmd);
    if (g_term_buf_ta)
        lv_textarea_set_text(g_term_buf_ta, "");
    _term_prompt_refresh();
}

/* 小尺寸按钮助手（本模块私有，避免依赖 ui_home 的静态函数） */
static lv_obj_t *term_mini_btn(lv_obj_t *par, const char *txt, uint32_t clr,
                               lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(par);
    if (!b) return NULL;
    lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(clr), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_left(b, 10, 0);
    lv_obj_set_style_pad_right(b, 10, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = uiw_label_font(b, txt, clr, FONT_UI);
    if (l) lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *_term_cmd_btn(lv_obj_t *par, const char *label, const char *cmd)
{
    lv_obj_t *b = lv_button_create(par);
    if (!b) return NULL;
    lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_pad_left(b, 10, 0);
    lv_obj_set_style_pad_right(b, 10, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = uiw_label_font(b, label, CLR_TEXT_LO, &lv_font_montserrat_14);
    if (l) lv_obj_center(l);
    lv_obj_add_event_cb(b, _on_term_quick, LV_EVENT_CLICKED, (void *)cmd);
    return b;
}

void ui_terminal_build(lv_obj_t *par)
{
    if (!par) return;

    g_term_len = 0;
    g_term_buf[0] = 0;
    g_term_fd = -1;
    g_term_pid = -1;
    g_term_poll = NULL;

    /* 输出区：flex 纵向排列（历史在上、提示行在末尾），否则两个 label 会重叠 */
    lv_obj_t *out = uiw_obj(par);
    if (!out) return;
    lv_obj_set_size(out, 860, 260);
    lv_obj_set_style_bg_color(out, lv_color_hex(0x050A12), 0);
    lv_obj_set_style_bg_opa(out, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(out, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(out, 1, 0);
    lv_obj_set_style_pad_all(out, 8, 0);
    lv_obj_set_flex_flow(out, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(out, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(out, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(out, LV_DIR_VER);
    g_term_out_box = out;

    g_term_out_lbl = lv_label_create(out);
    if (g_term_out_lbl) {
        lv_obj_set_width(g_term_out_lbl, 840);
        lv_label_set_long_mode(g_term_out_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(g_term_out_lbl, FONT_UI, 0);
        lv_obj_set_style_text_color(g_term_out_lbl, lv_color_hex(0x00FF9C), 0);
    }

    /* 命令行提示行：键盘输入直接回显在这里 */
    g_term_cmd_lbl = lv_label_create(out);
    if (g_term_cmd_lbl) {
        lv_obj_set_width(g_term_cmd_lbl, 840);
        lv_label_set_long_mode(g_term_cmd_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(g_term_cmd_lbl, FONT_UI, 0);
        lv_obj_set_style_text_color(g_term_cmd_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_top(g_term_cmd_lbl, 4, 0);
    }

    _term_append("i.MX6ULL debug shell (root)\n");
    _term_prompt_refresh();

    /* 按钮行：键盘 / 清屏（无输入框，回车执行） */
    lv_obj_t *row = uiw_obj(par);
    if (row) {
        g_term_btnrow = row;
        lv_obj_set_size(row, 860, 36);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        term_mini_btn(row, "键盘", CLR_CYAN, _on_term_kb_btn);
        term_mini_btn(row, "清屏", CLR_AMBER, _on_term_clear);
        lv_obj_t *hint = uiw_label_font(row, "回车执行命令", CLR_TEXT_DIM, FONT_UI);
        (void)hint;
    }

    /* 快捷命令 */
    lv_obj_t *qrow = uiw_obj(par);
    if (qrow) {
        g_term_qrow = qrow;
        lv_obj_set_size(qrow, 860, 34);
        lv_obj_set_flex_flow(qrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(qrow, 8, 0);
        lv_obj_clear_flag(qrow, LV_OBJ_FLAG_SCROLLABLE);
        _term_cmd_btn(qrow, "uname -a", "uname -a");
        _term_cmd_btn(qrow, "free -m", "free -m");
        _term_cmd_btn(qrow, "ps", "ps");
        _term_cmd_btn(qrow, "dmesg", "dmesg | tail -15");
        _term_cmd_btn(qrow, "ifconfig", "ifconfig");
    }

    /* 键盘缓冲：隐藏 textarea（键盘直接写入，不显示在界面上） */
    g_term_buf_ta = lv_textarea_create(par);
    if (g_term_buf_ta) {
        lv_obj_set_size(g_term_buf_ta, 8, 8);
        lv_textarea_set_one_line(g_term_buf_ta, true);
        lv_textarea_set_max_length(g_term_buf_ta, TERM_CMD_MAX);
        lv_obj_add_flag(g_term_buf_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_term_buf_ta, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_event_cb(g_term_buf_ta, _on_kb_buf_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* 软键盘（默认隐藏，贴子页底部） */
    g_term_kb = lv_keyboard_create(par);
    if (g_term_kb) {
        lv_obj_set_size(g_term_kb, 860, 200);
        lv_obj_align(g_term_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(g_term_kb, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_flag(g_term_kb, LV_OBJ_FLAG_HIDDEN);
        if (g_term_buf_ta)
            lv_keyboard_set_textarea(g_term_kb, g_term_buf_ta);
        lv_keyboard_set_mode(g_term_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_add_event_cb(g_term_kb, _on_term_kb_event, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(g_term_kb, _on_term_kb_event, LV_EVENT_CANCEL, NULL);
    }
}