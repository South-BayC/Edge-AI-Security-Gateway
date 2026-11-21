/**
 * cam_feed.c — 摄像头画面喂数实现（V4L2 → canvas）
 *
 * 数据流:
 *   /dev/video0 (OV5640, mx6s-csi)
 *     → DQBUF 取 YUYV 帧 (640×480，30fps 交付)
 *     → 动态节流（T13：无人 5fps / 运动后 10s 内 30fps）
 *     → YUYV→RGB565 转换（查表，无浮点）+ 翻转/缩放 + 帧差粗判
 *     → 写入后台 ping-pong 缓冲并原子发布（不直写 canvas，消撕裂）
 *   UI 33ms 定时器: cam_feed_blit_if_ready(canvas_buf) 拷贝最新完成帧 → 局部 invalidate
 *
 * 线程安全:
 *   - cam 线程只写 s_frame[s_write_idx]，写完在 s_frame_lock 内发布 s_cur_idx 并翻转写目标
 *   - UI 在 s_frame_lock 内拷贝 current 帧，与发布互斥（锁只覆盖索引更新 + 428KB memcpy）
 *   - 本线程绝不调用任何 lv_* API
 *
 * 诊断: cam 线程每 5s 打印 sensor 帧率(DQBUF) / conv 帧率(转换) / 单帧转换耗时，
 *       用于区分传感器侧瓶颈（sensor 低）与 CPU 侧瓶颈（conv_ms 高）。
 */
#include "cam_feed.h"
#include "log_svc.h"
#include "health_mgr.h"
#include "thread_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAM_DEV     "/dev/video0"
#define CAM_I2C_DEV "/dev/i2c-1"
#define CAM_I2C_ADDR 0x3c             /* OV5640 7 位地址（=dts reg 值；i2ctransfer @0x3c 已实测通信成功） */
#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706        /* 地址被内核 ov5640 驱动占用，必须 FORCE */
#endif
/* T13：动态帧率（无人省 CPU，有运动提高流畅度）
 *   idle 5fps：单核下把 CPU 让给 UI/检测；
 *   active 30fps：检测到运动后保持 10s，随后回落。
 * 采集仍按 30fps 交付，只是降低转换/预览频率。 */
#define CAM_FPS_MS_IDLE     200   /* 5fps */
#define CAM_FPS_MS_ACTIVE   33    /* 30fps */
#define CAM_ACTIVE_HOLD_MS  10000 /* 运动后保持 10s */
#define V4L2_BUF_CNT 4
#define CAM_FPS_TARGET 30      /* S_PARM 档位（驱动仅支持 15/30）：30fps 寄存器组时钟不同，
                                * 兼作捕获滚动诊断（若 30fps 下滚动消失/变化=时钟余量问题） */

/* ---- YUV→RGB 查表（初始化一次，转换时纯查表+加法，比逐像素乘除快 2-3 倍） ----
 * 公式: R = 1.164(Y-16) + 1.596(V-128)
 *       G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
 *       B = 1.164(Y-16) + 2.018(U-128)
 * 拆分为 Y 贡献表 + U/V 贡献表（YUYV 两像素共享 U/V，UV 只算一次） */
static int16_t tbl_r_y[256], tbl_g_y[256], tbl_b_y[256];
static int16_t tbl_r_v[256], tbl_g_u[256], tbl_g_v[256], tbl_b_u[256];

static void yuv_table_init(void)
{
    for (int i = 0; i < 256; i++) {
        int c = i - 16;
        tbl_r_y[i] = (int16_t)((1192 * c) >> 10);
        tbl_g_y[i] = (int16_t)((1192 * c) >> 10);
        tbl_b_y[i] = (int16_t)((1192 * c) >> 10);
    }
    for (int i = 0; i < 256; i++) {
        int d = i - 128, e = i - 128;
        tbl_r_v[i] = (int16_t)((1634 * e) >> 10);
        tbl_g_u[i] = (int16_t)((-400 * d) >> 10);   /* G -= 0.391(U-128) */
        tbl_g_v[i] = (int16_t)((-833 * e) >> 10);   /* G -= 0.813(V-128) */
        tbl_b_u[i] = (int16_t)((2066 * d) >> 10);
    }
}

static uint8_t *s_buf = NULL;          /* canvas 缓冲指针（cam_feed_start 传入，blit 目标） */
static int s_frame_ready = 0;          /* 帧就绪标志（s_frame_lock 保护） */
static int s_started = 0;
static volatile int s_fps_val = 0;     /* 转换帧率（每秒结算，UI 显示用） */
static int s_fps_cnt = 0;              /* 当前秒转换帧计数 */

/* ---- 双缓冲 ping-pong（消撕裂）----
 * cam 线程转换写入 s_frame[s_write_idx]，完成后加锁发布 s_cur_idx 并翻转写目标；
 * UI 在锁内拷贝 s_frame[s_cur_idx]。锁只覆盖索引发布与 428KB memcpy
 * （约 5ms，15fps 下可接受），彻底消除"读到半帧"的窗口。 */
static uint8_t s_frame[2][CAM_DISP_W * CAM_DISP_H * 2];
static int s_cur_idx = 0;              /* 最新完成帧索引（UI blit 源） */
static int s_write_idx = 0;            /* cam 线程当前写目标索引 */
static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* T5-B：优雅退出 */
static volatile int s_quit = 0;
static pthread_t s_tid;
static int s_thread_running = 0;

/* ---- 非缓存源帧中转（帧率关键）----
 * V4L2 mmap 缓冲为 writecombine/非缓存映射（DMA 一致性），逐字节散读每次
 * 都付 DRAM 延迟（板测：缓存源 29ms/帧 vs 非缓存源 114ms/帧）。
 * 先整帧 memcpy（突发读）进缓存区，再查表转换，读全部命中 cache。 */
static uint8_t s_src_copy[CAM_SRC_W * CAM_SRC_H * 2];

/* ---- 翻转（OV5640 安装方向：上下 180° 需要 V 翻；左右方向按实际排线/镜头）
 * 默认只做上下翻转（V=1,H=0）。若画面左右反了，把 GATEWAY_CAM_HFLIP 设为 1；
 * 若上下也反，把 GATEWAY_CAM_VFLIP 设为 0。 */
static volatile int s_vflip = 1;
static volatile int s_hflip = 0;

/* ---- 诊断计数（区分传感器侧 / CPU 侧瓶颈）---- */
static volatile int s_cap_val = 0;     /* DQBUF 帧率（传感器实际交付） */
static int s_cap_cnt = 0;
static volatile int s_conv_ms_x10 = 0; /* 最近一秒平均单帧转换耗时 ×0.1ms */
static volatile int s_stream_ok = 0;   /* STREAMON 成功且未断流（占位符显隐用） */

/* ---- 运动检测粗判引擎（T4：背景建模 + 形态学 + 连通域） ----
 * 算法:
 *   RGB565 帧 → 降采样 80×50 灰度
 *   → 全局光照突变检测（均值突变则重置背景，不产生命中）
 *   → 与滑动平均背景做差 → 二值前景
 *   → 3×3 开运算（腐蚀+膨胀）去孤立噪点
 *   → 4 邻域连通域，过滤面积 < min_area 的小块
 *   → 有效前景占比 > ratio，连续 2 次 → 命中
 * 目的: 抑制灯光开关、自动曝光、窗帘/风扇抖动造成的误报。
 * 内存: 4×4KB 静态缓冲；80×50 上形态学/连通域开销很小。 */
#define MD_W                80
#define MD_H                50
#define MD_DIFF_TH          24     /* 像素灰度差阈值默认值（0-63 G 高 6 位，运行时可调） */
#define MD_RATIO_PCT        5      /* 有效前景占比阈值 % */
#define MD_DEBOUNCE         2      /* 连续 N 次超限 → 命中 */
#define MD_BG_ALPHA_X1000   50     /* 背景更新系数 0.05 */
#define MD_MIN_AREA         20     /* 连通域最小面积（降采样像素） */
#define MD_LIGHT_JUMP_PCT   15     /* 全局均值突变阈值 % */
#define MD_LIGHT_MIN        10     /* 绝对最小灰度差（0~63，暗场景防误判） */
#define MD_LIGHT_FRAME_JUMP 10     /* 相邻帧均值突变阈值（0~63）：开关灯瞬间立即识别 */
#define MD_LIGHT_SKIP_FRAMES 20    /* 光照事件后跳过的帧数：等 AE/白平衡稳定（约 1.3s@15fps） */

static uint8_t md_bg[MD_W * MD_H];    /* 滑动平均背景 */
static uint8_t md_cur[MD_W * MD_H];   /* 当前帧降采样灰度 */
static uint8_t md_fg[MD_W * MD_H];    /* 前景二值图 */
static uint8_t md_tmp[MD_W * MD_H];   /* 形态学临时图 */
static int md_bg_valid = 0;
static int md_cnt = 0;
static int md_prev_mean = -1;         /* 上一帧全局均值（单帧突变检测） */
static int md_light_skip = 0;         /* 光照事件后剩余跳过帧数 */
static volatile int s_bg_alpha_x1000 = MD_BG_ALPHA_X1000;
static volatile int s_min_area = MD_MIN_AREA;
static volatile int s_light_jump = MD_LIGHT_JUMP_PCT;
static volatile int s_motion_en = 1;   /* 运动检测使能（默认开） */
static int s_motion_hits = 0;          /* 命中计数（cam 线程原子自增，消费端原子取走） */
static volatile int s_motion_ratio = MD_RATIO_PCT; /* 变化占比阈值 %（可调） */
static volatile int s_motion_diff = MD_DIFF_TH;    /* 像素灰度差阈值（可调，过滤小偏差） */
static volatile int s_motion_interval = 1;         /* 帧差对比间隔 N（每 N 个转换帧判定一次） */
static int s_motion_tick = 0;                      /* 间隔计数器 */

/* T13：最近一次运动命中时间（CLOCK_MONOTONIC ms，uint32 回绕安全）
 * 初值取“很久以前”，避免开机前 10 秒被误判为 active。 */
static volatile uint32_t s_last_hit_ms =
    (uint32_t)(0u - (uint32_t)CAM_ACTIVE_HOLD_MS - 1000u);

/* C4：ROI（屏蔽区域，百分比 0~100）与阴影抑制（仅轻微变暗的像素忽略） */
static int s_roi_en = 0;
static int s_roi[4] = { 0, 0, MD_W, MD_H };   /* md 坐标 x0,y0,x1,y1 */
static int s_shadow_suppress = 0;

void cam_feed_set_roi(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0; if (x0 > 100) x0 = 100;
    if (y0 < 0) y0 = 0; if (y0 > 100) y0 = 100;
    if (x1 < 0) x1 = 0; if (x1 > 100) x1 = 100;
    if (y1 < 0) y1 = 0; if (y1 > 100) y1 = 100;
    if (x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = 100; y1 = 100; }

    s_roi[0] = x0 * MD_W / 100;
    s_roi[1] = y0 * MD_H / 100;
    s_roi[2] = x1 * MD_W / 100;
    s_roi[3] = y1 * MD_H / 100;
    s_roi_en = !(x0 == 0 && y0 == 0 && x1 == 100 && y1 == 100);
}

void cam_feed_set_shadow_suppress(int en)
{
    s_shadow_suppress = en ? 1 : 0;
}

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L));
}

/* ---- 公开接口（cam_feed.h 声明；非 static 供 UI/桥接层调用） ---- */
static void scale_map_init(void);   /* 前置声明：cam_feed_set_flip 需重建映射表 */

void cam_feed_set_motion_en(int en) { s_motion_en = en ? 1 : 0; }
int  cam_feed_get_motion_en(void)  { return s_motion_en; }
int  cam_feed_get_motion_hits(void)
{
    return __atomic_exchange_n(&s_motion_hits, 0, __ATOMIC_RELAXED);
}
void cam_feed_set_motion_threshold(int pct)
{
    if (pct < 1) pct = 1;
    if (pct > 50) pct = 50;
    s_motion_ratio = pct;
}
int cam_feed_get_motion_threshold(void) { return s_motion_ratio; }

/* 帧差对比间隔：每 N 个转换帧判定一次（1=每帧；N 越大帧间差异越大、开销越小） */
void cam_feed_set_motion_interval(int n)
{
    if (n < 1) n = 1;
    if (n > 30) n = 30;
    s_motion_interval = n;
    s_motion_tick = 0;
}
int cam_feed_get_motion_interval(void) { return s_motion_interval; }

/* 像素灰度差阈值：过滤小幅亮度偏差（5~100，默认 24） */
void cam_feed_set_motion_diff(int d)
{
    if (d < 5) d = 5;
    if (d > 100) d = 100;
    s_motion_diff = d;
}
int cam_feed_get_motion_diff(void) { return s_motion_diff; }

/* T4：背景建模/形态学参数（后续可接设置页；当前用默认值） */
void cam_feed_set_motion_bg_alpha(int x1000)
{
    if (x1000 < 1) x1000 = 1;
    if (x1000 > 500) x1000 = 500;   /* 0.001~0.5 */
    s_bg_alpha_x1000 = x1000;
}
int cam_feed_get_motion_bg_alpha(void) { return s_bg_alpha_x1000; }

void cam_feed_set_motion_min_area(int v)
{
    if (v < 1) v = 1;
    if (v > 500) v = 500;
    s_min_area = v;
}
int cam_feed_get_motion_min_area(void) { return s_min_area; }

void cam_feed_set_motion_light_jump(int pct)
{
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    s_light_jump = pct;
}
int cam_feed_get_motion_light_jump(void) { return s_light_jump; }

/* 画面翻转（V=上下翻，H=左右翻；默认 V+H=180°，匹配排线安装方向）。
 * 需在 cam_feed_start 前调用（映射表在采集线程启动时构建）；启动后调用
 * 会重建映射表，存在一帧混用旧映射的瞬态，无害。 */
void cam_feed_set_flip(int v, int h)
{
    s_vflip = v ? 1 : 0;
    s_hflip = h ? 1 : 0;
    scale_map_init();
}

/* RGB565 → 降采样灰度（G 通道近似亮度） */
static void md_downsample(const uint8_t *frame, uint8_t *out)
{
    for (int y = 0; y < MD_H; y++) {
        const uint16_t *sline = (const uint16_t *)frame + (y * CAM_DISP_H / MD_H) * CAM_DISP_W;
        for (int x = 0; x < MD_W; x++) {
            uint16_t px = sline[x * CAM_DISP_W / MD_W];
            out[y * MD_W + x] = (uint8_t)((px >> 5) & 0x3F);   /* G 高 6 位 */
        }
    }
}

/* 3×3 腐蚀：图内 9 邻域全为 1 才保留（图外邻域忽略，保留边缘） */
static void md_erode3x3(const uint8_t *src, uint8_t *dst)
{
    for (int y = 0; y < MD_H; y++) {
        for (int x = 0; x < MD_W; x++) {
            int keep = 1;
            for (int dy = -1; dy <= 1 && keep; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= MD_H || xx < 0 || xx >= MD_W)
                        continue;
                    if (src[yy * MD_W + xx] == 0) {
                        keep = 0;
                        break;
                    }
                }
            }
            dst[y * MD_W + x] = (uint8_t)keep;
        }
    }
}

/* 3×3 膨胀：图内 9 邻域任一为 1 即保留 */
static void md_dilate3x3(const uint8_t *src, uint8_t *dst)
{
    for (int y = 0; y < MD_H; y++) {
        for (int x = 0; x < MD_W; x++) {
            int keep = 0;
            for (int dy = -1; dy <= 1 && !keep; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= MD_H || xx < 0 || xx >= MD_W)
                        continue;
                    if (src[yy * MD_W + xx]) {
                        keep = 1;
                        break;
                    }
                }
            }
            dst[y * MD_W + x] = (uint8_t)keep;
        }
    }
}

/* 4 邻域连通域：只统计面积 >= min_area 的前景块，返回总面积 */
static int md_components(const uint8_t *fg, int min_area)
{
    static uint8_t visited[MD_W * MD_H];
    static int stack[MD_W * MD_H];
    const int total = MD_W * MD_H;
    int total_area = 0;

    memset(visited, 0, sizeof(visited));

    for (int i = 0; i < total; i++) {
        if (!fg[i] || visited[i])
            continue;

        int top = 0;
        int area = 0;
        stack[top++] = i;
        visited[i] = 1;

        while (top > 0) {
            int p = stack[--top];
            int x = p % MD_W;
            int y = p / MD_W;
            area++;

            static const int dx[4] = { -1, 1, 0, 0 };
            static const int dy[4] = { 0, 0, -1, 1 };
            for (int k = 0; k < 4; k++) {
                int xx = x + dx[k];
                int yy = y + dy[k];
                if (xx < 0 || xx >= MD_W || yy < 0 || yy >= MD_H)
                    continue;
                int q = yy * MD_W + xx;
                if (fg[q] && !visited[q]) {
                    visited[q] = 1;
                    stack[top++] = q;
                }
            }
        }

        if (area >= min_area)
            total_area += area;
    }
    return total_area;
}

/* 运动判定：背景建模 + 光照突变抑制 + 形态学 + 连通域 */
static void motion_detect(const uint8_t *frame)
{
    const int total = MD_W * MD_H;

    if (!s_motion_en) {
        md_bg_valid = 0;
        md_cnt = 0;
        md_prev_mean = -1;
        md_light_skip = 0;
        return;
    }

    md_downsample(frame, md_cur);

    if (!md_bg_valid) {
        memcpy(md_bg, md_cur, (size_t)total);
        md_bg_valid = 1;
        md_cnt = 0;
        md_prev_mean = -1;
        return;
    }

    /* 全局亮度突变：均值变化超过阈值 → 视为光照事件，重置背景，不产生命中 */
    int cur_sum = 0, bg_sum = 0;
    for (int i = 0; i < total; i++) {
        cur_sum += md_cur[i];
        bg_sum += md_bg[i];
    }
    int cur_mean = cur_sum / total;
    int bg_mean = bg_sum / total;
    int jump = cur_mean - bg_mean;
    if (jump < 0) jump = -jump;

    /* 相对阈值 + 绝对下限：暗场景 bg_mean 很小时，避免把噪声当光照突变 */
    int light_thr = (bg_mean * s_light_jump) / 100;
    if (light_thr < MD_LIGHT_MIN)
        light_thr = MD_LIGHT_MIN;

    /* 单帧均值突变：开灯瞬间画面整体跳变，即使相对背景的累计差还没超阈值，
     * 也能立刻识别为光照事件。 */
    int frame_jump = 0;
    if (md_prev_mean >= 0) {
        frame_jump = cur_mean - md_prev_mean;
        if (frame_jump < 0) frame_jump = -frame_jump;
    }
    md_prev_mean = cur_mean;

    if (jump > light_thr || frame_jump > MD_LIGHT_FRAME_JUMP) {
        if (md_light_skip == 0)   /* 只在光照事件开始时打印，AE 渐变期间不刷日志 */
            LOGD("CAM", "light jump reset: mean %d -> %d (bg_jump=%d frame_jump=%d thr=%d)",
                 bg_mean, cur_mean, jump, frame_jump, light_thr);
        memcpy(md_bg, md_cur, (size_t)total);
        md_cnt = 0;
        md_light_skip = MD_LIGHT_SKIP_FRAMES;
        return;
    }

    /* 光照事件后 AE/白平衡需要若干帧才能稳定：跳过判定，并持续用当前帧
     * 重建背景，避免整段渐变被当成运动。 */
    if (md_light_skip > 0) {
        md_light_skip--;
        memcpy(md_bg, md_cur, (size_t)total);
        md_cnt = 0;
        return;
    }

    /* 前景：与滑动平均背景做差
     * C4：阴影抑制——仅“轻微变暗”（差值小于背景一半）的像素不算运动目标；
     * 真实物体通常伴随亮度或纹理的显著变化，不会被这条规则误伤。 */
    for (int i = 0; i < total; i++) {
        int d = md_cur[i] - md_bg[i];
        int ad = d < 0 ? -d : d;
        int fg = (ad > s_motion_diff) ? 1 : 0;
        if (fg && s_shadow_suppress && d < 0 && ad * 2 < md_bg[i])
            fg = 0;
        md_fg[i] = (uint8_t)fg;
    }

    /* 3×3 开运算：去除孤立噪点/细小抖动 */
    md_erode3x3(md_fg, md_tmp);
    md_dilate3x3(md_tmp, md_fg);

    /* C4：ROI 屏蔽——区域外的前景直接清零（墙面/窗帘/窗外） */
    if (s_roi_en) {
        for (int y = 0; y < MD_H; y++) {
            for (int x = 0; x < MD_W; x++) {
                if (x < s_roi[0] || x >= s_roi[2] ||
                    y < s_roi[1] || y >= s_roi[3])
                    md_fg[y * MD_W + x] = 0;
            }
        }
    }

    /* 连通域面积过滤：忽略过小的反光/噪点 */
    int area = md_components(md_fg, s_min_area);

    if (area * 100 > total * s_motion_ratio)
        md_cnt++;
    else
        md_cnt = 0;

    if (md_cnt >= MD_DEBOUNCE) {
        md_cnt = 0;
        s_last_hit_ms = mono_ms();   /* T13：触发动态帧率提升 */
        __atomic_fetch_add(&s_motion_hits, 1, __ATOMIC_RELAXED);   /* 原子自增，避免与 UI 清零竞争 */
        LOGD("CAM", "motion hit area=%d/%d", area, total);
    }

    /* 背景缓慢更新（默认 0.05）；光照突变时已在上面重置 */
    int a = s_bg_alpha_x1000;
    if (a < 1) a = 1;
    if (a > 500) a = 500;
    for (int i = 0; i < total; i++)
        md_bg[i] = (uint8_t)((md_bg[i] * (1000 - a) + md_cur[i] * a) / 1000);
}

static inline int clamp8(int x)
{
    return x < 0 ? 0 : (x > 255 ? 255 : x);
}

static inline uint16_t pack_rgb565(int r, int g, int b)
{
    return (uint16_t)((clamp8(r) >> 3) << 11 | (clamp8(g) >> 2) << 5 | clamp8(b) >> 3);
}

/* ---- 缩放映射表（初始化一次，转换时无除法；含 V/H 翻转） ----
 * 列映射：显示 x → 源 x（最近邻，偶对齐 YUYV）；行映射：显示 y → 源 y */
static int map_col[CAM_DISP_W];
static int map_row[CAM_DISP_H];

static void scale_map_init(void)
{
    for (int x = 0; x < CAM_DISP_W; x++) {
        int sx = x * CAM_SRC_W / CAM_DISP_W;
        sx &= ~1;   /* YUYV 偶对齐（取共享 UV 对） */
        map_col[x] = s_hflip ? (CAM_SRC_W - 2 - sx) : sx;
    }
    for (int y = 0; y < CAM_DISP_H; y++) {
        int sy = y * CAM_SRC_H / CAM_DISP_H;
        map_row[y] = s_vflip ? (CAM_SRC_H - 1 - sy) : sy;
    }
}

/* ---- 单源行 YUYV→RGB565（NEON 8 像素/迭代；x86 模拟器回退标量） ----
 * 公式与标量路径一致：R=(Y-16)*1192>>10 + (V-128)*1634>>10 等（>>10 定点）。
 * NEON 用 vmull_n_s16 宽乘避免 s16 溢出，vqmovun 饱和窄化等价 clamp8。 */
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

static void convert_row(const uint8_t *src, uint16_t *dst)
{
    const int16x8_t c16  = vdupq_n_s16(16);
    const int16x8_t c128 = vdupq_n_s16(128);
    const uint16x8_t mr  = vdupq_n_u16(0xF8);   /* R 高 5 位掩码（<<8 即 <<11） */
    const uint16x8_t mg  = vdupq_n_u16(0xFC);   /* G 高 6 位掩码（<<3 即 <<5） */

    /* vld4_u8 一次取 32 字节 = 16 像素（YUYV 每像素 2 字节）：
     * lane0=偶数像素 Y×8, lane1=U×8, lane2=奇数像素 Y×8, lane3=V×8 */
    for (int i = 0; i < CAM_SRC_W; i += 16, src += 32, dst += 16) {
        uint8x8x4_t q = vld4_u8(src);   /* lane0=Y偶 lane1=U lane2=Y奇 lane3=V */
        int16x8_t y0 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[0])), c16);
        int16x8_t y1 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[2])), c16);
        int16x8_t ud = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[1])), c128);
        int16x8_t vd = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[3])), c128);

        /* 宽乘避免 s16 溢出；(c*k)>>10 与标量 >>10 语义一致 */
        int16x8_t yc0 = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(y0), 1192), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(y0), 1192), 10));
        int16x8_t yc1 = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(y1), 1192), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(y1), 1192), 10));
        int16x8_t rv  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(vd), 1634), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(vd), 1634), 10));
        int16x8_t gu  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(ud), -400), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(ud), -400), 10));
        int16x8_t gv  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(vd), -833), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(vd), -833), 10));
        int16x8_t bu  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(ud), 2066), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(ud), 2066), 10));

        int16x8_t gadd = vaddq_s16(gu, gv);
        int16x8_t r0 = vaddq_s16(yc0, rv);
        int16x8_t g0 = vaddq_s16(yc0, gadd);
        int16x8_t b0 = vaddq_s16(yc0, bu);
        int16x8_t r1 = vaddq_s16(yc1, rv);
        int16x8_t g1 = vaddq_s16(yc1, gadd);
        int16x8_t b1 = vaddq_s16(yc1, bu);

        /* 饱和窄化 u8（等价 clamp8）→ RGB565 组包 → 偶/奇像素交错存储 */
        uint16x8_t r016 = vmovl_u8(vqmovun_s16(r0));
        uint16x8_t g016 = vmovl_u8(vqmovun_s16(g0));
        uint16x8_t b016 = vmovl_u8(vqmovun_s16(b0));
        uint16x8_t r116 = vmovl_u8(vqmovun_s16(r1));
        uint16x8_t g116 = vmovl_u8(vqmovun_s16(g1));
        uint16x8_t b116 = vmovl_u8(vqmovun_s16(b1));

        uint16x8_t px0 = vorrq_u16(vshlq_n_u16(vandq_u16(r016, mr), 8),
                                   vorrq_u16(vshlq_n_u16(vandq_u16(g016, mg), 3),
                                             vshrq_n_u16(b016, 3)));
        uint16x8_t px1 = vorrq_u16(vshlq_n_u16(vandq_u16(r116, mr), 8),
                                   vorrq_u16(vshlq_n_u16(vandq_u16(g116, mg), 3),
                                             vshrq_n_u16(b116, 3)));
        uint16x8x2_t out = {{ px0, px1 }};
        vst2q_u16(dst, out);
    }
}

#else  /* 模拟器（x86 host）标量回退 */

static void convert_row(const uint8_t *src, uint16_t *dst)
{
    for (int sx = 0; sx < CAM_SRC_W; sx += 2) {
        int y0 = src[sx], u = src[sx + 1], y1 = src[sx + 2], v = src[sx + 3];
        dst[sx]     = pack_rgb565(tbl_r_y[y0] + tbl_r_v[v],
                                  tbl_g_y[y0] + tbl_g_u[u] + tbl_g_v[v],
                                  tbl_b_y[y0] + tbl_b_u[u]);
        dst[sx + 1] = pack_rgb565(tbl_r_y[y1] + tbl_r_v[v],
                                  tbl_g_y[y1] + tbl_g_u[u] + tbl_g_v[v],
                                  tbl_b_y[y1] + tbl_b_u[u]);
    }
}

#endif

/* 源帧（YUYV）→ 翻转缩放写入 dst：两阶段——
 * ① convert_row 把需要的源行转成 RGB565 行缓存（NEON）
 * ② 按 map_row/map_col 取样到画布（行去重 + 列去重/翻转） */
static uint16_t s_row_tmp[CAM_SRC_W];

static void yuyv_scale_to_canvas(const uint8_t *src, uint8_t *dst_buf)
{
    uint16_t *dst = (uint16_t *)dst_buf;
    int prev_row = -1;
    for (int dy = 0; dy < CAM_DISP_H; dy++) {
        int srow = map_row[dy];
        if (srow != prev_row) {
            convert_row(src + srow * CAM_SRC_W * 2, s_row_tmp);
            prev_row = srow;
        }
        uint16_t *dline = dst + dy * CAM_DISP_W;
        for (int dx = 0; dx < CAM_DISP_W; dx++)
            dline[dx] = s_row_tmp[map_col[dx]];
    }
}

/* ---- 务实补丁：STREAMON 后重写 PLL 系统分频 ----
 * 根因链：驱动 STREAMON 的 ov5640_init_mode 用硬编码基线数组（0x3035=0x21 慢时钟）
 * 覆盖 S_PARM 设置的 30fps（30fps blob 本身 + 时序均正确，已验证）。
 * 板测证实：流媒体运行中热写 0x3035=0x11 后寄存器稳定保持、帧率 30fps（30s 无回写）。
 * 故在 STREAMON 后经 /dev/i2c-1 重写一次。init 流程根治后可移除本补丁。 */
static void ov5640_force_30fps_clk(void)
{
    unsigned char buf[3] = { 0x30, 0x35, 0x11 };   /* 寄存器 0x3035 = 0x11（sysdiv 2→1） */
    int f = open(CAM_I2C_DEV, O_RDWR);
    if (f < 0) {
        LOGE("CAM", "workaround open %s failed: %s", CAM_I2C_DEV, strerror(errno));
        return;
    }
    if (ioctl(f, I2C_SLAVE_FORCE, CAM_I2C_ADDR) < 0) {
        LOGE("CAM", "workaround ioctl failed: %s", strerror(errno));
        close(f);
        return;
    }
    int w = write(f, buf, sizeof(buf));
    if (w == 3)
        LOGI("CAM", "PLL 0x3035=0x11 applied (30fps workaround)");
    else
        LOGE("CAM", "workaround write failed (%d): %s", w, strerror(errno));
    close(f);
}

/* ---- V4L2 ioctl 包装 ---- */
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ================================================================
 * V4L2 采集线程
 * ================================================================ */
static void *cam_thread(void *arg)
{
    (void)arg;
    /* T5-B：转换线程降优先级（nice+5），把 CPU 让给 UI 渲染与采集线程 */
    setpriority(PRIO_PROCESS, 0, 5);

    yuv_table_init();   /* 色彩查表初始化（一次） */
    scale_map_init();   /* 缩放映射表初始化（一次） */

    /* 注：曾试过 SCHED_FIFO 提权，引发 RT throttling 且 UI 渲染被抢占更卡，已撤（08-29） */

    /* 启动基准：纯转换耗时（无 V4L2、无渲染干扰），区分低频/抢占与算法本身 */
    {
        uint8_t *bench_src = malloc(CAM_SRC_W * CAM_SRC_H * 2);
        if (bench_src) {
            struct timespec b0, b1;
            memset(bench_src, 0x80, CAM_SRC_W * CAM_SRC_H * 2);
            clock_gettime(CLOCK_MONOTONIC, &b0);
            for (int i = 0; i < 5; i++)
                yuyv_scale_to_canvas(bench_src, s_frame[0]);
            clock_gettime(CLOCK_MONOTONIC, &b1);
            LOGD("CAM", "conv_bench=%ld ms/frame (pure compute)",
                   ((b1.tv_sec - b0.tv_sec) * 1000L +
                    (b1.tv_nsec - b0.tv_nsec) / 1000000L) / 5);
            free(bench_src);
        }
    }

    int fd = open(CAM_DEV, O_RDWR);
    if (fd < 0) {
        LOGE("CAM", "%s open failed: %s (camera preview disabled)",
               CAM_DEV, strerror(errno));
        return NULL;
    }

    /* 1. 格式：640×480 YUYV（OV5640 标准档，缩放到显示尺寸） */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_SRC_W;
    fmt.fmt.pix.height = CAM_SRC_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOGE("CAM", "S_FMT failed: %s (requested %dx%d YUYV, driver returned %dx%d fmt=0x%08x)",
             strerror(errno), CAM_SRC_W, CAM_SRC_H,
             fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);
        close(fd);
        return NULL;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        LOGE("CAM", "YUYV not supported by driver");
        close(fd);
        return NULL;
    }

    /* 1.5 帧率/档位：显式 VGA 档 + 目标帧率，不依赖 G_PARM（其失败会跳过整段）。
     * 枚举实证：ov5640_mode_VGA_640_480 = 0（ov5640.c:51）。
     * 设置后回读——驱动是否真正接受档位一目了然 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CAM_FPS_TARGET;
    parm.parm.capture.capturemode = 0;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
        LOGE("CAM", "S_PARM(%dfps,VGA) failed", CAM_FPS_TARGET);

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0)
        LOGI("CAM", "applied: %d/%d fps, capturemode=%d",
               parm.parm.capture.timeperframe.denominator,
               parm.parm.capture.timeperframe.numerator,
               parm.parm.capture.capturemode);

    /* 2. 申请缓冲（mmap） */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUF_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        LOGE("CAM", "REQBUFS failed");
        close(fd);
        return NULL;
    }

    /* 驱动可能按可用内存裁剪实际缓冲数（REQBUFS 会回写 req.count，
     * 且不报错）。必须以实际数量为准，否则按 4 个 QUERYBUF 会越界失败，
     * 表现为“摄像头完全不工作”，日志只有一行 QUERYBUF failed。 */
    if (req.count < 2) {
        LOGE("CAM", "REQBUFS returned %u buffers (need >= 2)", (unsigned)req.count);
        close(fd);
        return NULL;
    }
    if (req.count > V4L2_BUF_CNT)
        req.count = V4L2_BUF_CNT;   /* 防御：正常不会超过请求值 */

    LOGI("CAM", "V4L2 buffers: requested %d, allocated %u",
         V4L2_BUF_CNT, (unsigned)req.count);

    struct {
        void *start;
        size_t length;
    } bufs[V4L2_BUF_CNT];
    memset(bufs, 0, sizeof(bufs));

    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            LOGE("CAM", "QUERYBUF failed (index %u)", i);
            close(fd);
            return NULL;
        }
        bufs[i].length = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            LOGE("CAM", "mmap failed (index %u)", i);
            close(fd);
            return NULL;
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            LOGE("CAM", "QBUF failed (index %u)", i);
            close(fd);
            return NULL;
        }
    }

    /* 3. 开始采集 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        LOGE("CAM", "STREAMON failed");
        close(fd);
        return NULL;
    }
    LOGI("CAM", "streaming %dx%d YUYV from %s (bytesperline=%u sizeimage=%u)",
           fmt.fmt.pix.width, fmt.fmt.pix.height, CAM_DEV,
           (unsigned)fmt.fmt.pix.bytesperline, (unsigned)fmt.fmt.pix.sizeimage);
    s_stream_ok = 1;
    ov5640_force_30fps_clk();   /* STREAMON 后重写 PLL 分频（驱动 init 会覆盖，见函数注释） */

    /* 4. 采集循环：DQBUF → 转换（33ms 节流）→ 发布 → QBUF（30fps） */
    long long last_ms = 0, fps_last_ms = 0, first_frame_ms = 0;
    long conv_us_acc = 0;
    int  conv_n = 0, diag_printed = 0;
    int  last_active = -1;      /* T13：上次帧率模式（用于切换日志） */
    while (!s_quit) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EINTR) continue;
            LOGE("CAM", "DQBUF failed");
            s_stream_ok = 0;
            break;
        }
        s_cap_cnt++;   /* 传感器实际交付帧计数 */

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (first_frame_ms == 0) first_frame_ms = now_ms;

        /* T13：无人 5fps / 运动后 10s 内 30fps（切换时打一条日志） */
        int active = ((uint32_t)((uint32_t)now_ms - s_last_hit_ms)
                          < CAM_ACTIVE_HOLD_MS) ? 1 : 0;
        int fps_gap = active ? CAM_FPS_MS_ACTIVE : CAM_FPS_MS_IDLE;

        if (now_ms - last_ms >= fps_gap) {
            struct timespec t0, t1;
            long conv_us;
            last_ms = now_ms;

            if (active != last_active) {
                LOGI("CAM", "fps mode -> %s", active ? "active(30fps)" : "idle(5fps)");
                last_active = active;
            }

            clock_gettime(CLOCK_MONOTONIC, &t0);
            memcpy(s_src_copy, bufs[b.index].start, CAM_SRC_W * CAM_SRC_H * 2);
            yuyv_scale_to_canvas(s_src_copy, s_frame[s_write_idx]);
            /* 帧差粗判：按可配置间隔判定（间隔 N=隔 N 帧对比，帧间差异随 N 增大） */
            if (++s_motion_tick >= s_motion_interval) {
                s_motion_tick = 0;
                motion_detect(s_frame[s_write_idx]);   /* 命中计数供桥接层消费 */
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            conv_us = (t1.tv_sec - t0.tv_sec) * 1000000L
                    + (t1.tv_nsec - t0.tv_nsec) / 1000;
            conv_us_acc += conv_us;
            conv_n++;

            pthread_mutex_lock(&s_frame_lock);
            s_cur_idx = s_write_idx;   /* 发布完成帧（写目标随后切到另一块） */
            s_frame_ready = 1;
            pthread_mutex_unlock(&s_frame_lock);
            s_write_idx ^= 1;
            s_fps_cnt++;
        }

        /* 每秒结算帧率/耗时（UI 读数用）；诊断行仅开播 5s 后打印一次，不再刷屏 */
        if (now_ms - fps_last_ms >= 1000) {
            /* T8：摄像头线程每秒心跳；断流/线程卡死会被看门狗检测 */
            health_beat(HEALTH_CAM);
            s_fps_val = s_fps_cnt;
            s_cap_val = s_cap_cnt;
            s_conv_ms_x10 = conv_n ? (int)(conv_us_acc / conv_n / 100) : 0;
            s_fps_cnt = 0; s_cap_cnt = 0; conv_us_acc = 0; conv_n = 0;
            fps_last_ms = now_ms;
            if (!diag_printed && first_frame_ms && now_ms - first_frame_ms >= 5000) {
                diag_printed = 1;
                LOGI("CAM", "sensor=%d fps, conv=%d fps, conv_ms=%d.%d",
                       s_cap_val, s_fps_val, s_conv_ms_x10 / 10, s_conv_ms_x10 % 10);
            }
        }

        /* 归还缓冲：DQBUF 返回的 b（含 index）直接传回 QBUF（v4l2 标准用法） */
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            LOGE("CAM", "re-QBUF failed");
            break;
        }
    }

    close(fd);
    return NULL;
}

/* ================================================================
 * 公共接口
 * ================================================================ */
void cam_feed_start(uint8_t *buf)
{
    if (s_started) return;
    s_started = 1;
    s_buf = buf;
    s_quit = 0;

    /* C4：ROI / 阴影抑制（环境变量，默认关闭）
     * GATEWAY_CAM_ROI="x0,y0,x1,y1"（百分比 0~100）
     * GATEWAY_CAM_SHADOW=1 开启阴影抑制 */
    const char *roi = getenv("GATEWAY_CAM_ROI");
    if (roi && roi[0]) {
        int a = 0, b = 0, c = 100, d = 100;
        if (sscanf(roi, "%d,%d,%d,%d", &a, &b, &c, &d) == 4)
            cam_feed_set_roi(a, b, c, d);
    }
    /* 镜像/翻转（排线安装方向不同）：默认 V=1 H=0
     *   GATEWAY_CAM_VFLIP=0/1 上下翻转
     *   GATEWAY_CAM_HFLIP=0/1 左右翻转
     * 画面左右反了 → HFLIP=1；上下也反 → VFLIP=0。 */
    {
        const char *fv = getenv("GATEWAY_CAM_VFLIP");
        const char *fh = getenv("GATEWAY_CAM_HFLIP");
        int v = fv ? atoi(fv) : s_vflip;
        int h = fh ? atoi(fh) : s_hflip;
        cam_feed_set_flip(v, h);
    }

    const char *sh = getenv("GATEWAY_CAM_SHADOW");
    cam_feed_set_shadow_suppress(sh && atoi(sh) != 0);

    if (thread_start(&s_tid, 1024 * 1024, cam_thread, NULL) != 0) {
        LOGE("CAM", "thread create failed");
        s_started = 0;
        return;
    }
    s_thread_running = 1;
    LOGI("CAM", "camera feed started r6-neon (canvas %dx%d, flip v=%d h=%d)",
           CAM_DISP_W, CAM_DISP_H, s_vflip, s_hflip);
    LOGI("CAM", "motion engine: bg+open+cc (diff=%d ratio=%d min_area=%d)",
         s_motion_diff, s_motion_ratio, s_min_area);
    LOGI("CAM", "dynamic fps: idle=%dms(5fps) active=%dms(30fps) hold=%dms",
         CAM_FPS_MS_IDLE, CAM_FPS_MS_ACTIVE, CAM_ACTIVE_HOLD_MS);
    LOGI("CAM", "motion roi=%s [%d,%d,%d,%d] shadow_suppress=%d",
         s_roi_en ? "on" : "off", s_roi[0], s_roi[1], s_roi[2], s_roi[3],
         s_shadow_suppress);
}

/* T5-B：停止采集线程并 join（退出流程调用；DQBUF 周期 33ms，一般 <100ms 返回） */
void cam_feed_stop(void)
{
    if (!s_thread_running) return;

    s_quit = 1;
    pthread_join(s_tid, NULL);
    s_thread_running = 0;
    s_started = 0;
    s_stream_ok = 0;
    s_buf = NULL;

    pthread_mutex_lock(&s_frame_lock);
    s_frame_ready = 0;
    pthread_mutex_unlock(&s_frame_lock);

    LOGI("CAM", "camera feed stopped");
}

/* UI 定时器调用：有新完成帧则拷入 canvas 缓冲并返回 1（随后调用方 invalidate） */
int cam_feed_blit_if_ready(uint8_t *dst)
{
    if (!dst) return 0;

    pthread_mutex_lock(&s_frame_lock);
    if (!s_frame_ready) {
        pthread_mutex_unlock(&s_frame_lock);
        return 0;
    }
    s_frame_ready = 0;
    memcpy(dst, s_frame[s_cur_idx], CAM_DISP_W * CAM_DISP_H * 2);
    pthread_mutex_unlock(&s_frame_lock);
    return 1;
}

int cam_feed_get_fps(void)
{
    return s_fps_val;
}

/* 采集流是否在工作（STREAMON 成功且未断流）：占位符/网格显隐用 */
int cam_feed_stream_ok(void)
{
    return s_stream_ok;
}

int cam_feed_copy_frame(uint8_t *dst)
{
    if (!dst || !s_buf) return -1;   /* 摄像头未启动，无有效帧 */

    pthread_mutex_lock(&s_frame_lock);
    memcpy(dst, s_frame[s_cur_idx], CAM_DISP_W * CAM_DISP_H * 2);
    pthread_mutex_unlock(&s_frame_lock);
    return 0;
}
