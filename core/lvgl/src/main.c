/**
 * main.c — 智能安防网关(LVGL 应用)板端入口
 *
 * 程序启动流程：
 *   1) log_init()        启动分级异步日志（先于配置加载，便于记录启动过程）；
 *   2) config_init()     加载持久化配置（亮度/检测参数/预警阈值）；
 *   3) lvgl_port_init()  初始化 LVGL：打开 /dev/fb0 作为显示fb，并自动探测
 *                        电容触摸设备(/dev/input 里名字含 gt9147/goodix 的)；
 *   4) 创建主界面：设置深色背景后调用 ui_home_create() 构建整个主页 UI
 *      （顶栏/摄像头预览/分区卡片/时间轴/底栏及各弹窗）；
 *   5) dev_bridge_start() 启动外设桥接线程：采集 KEY0/光感/IMU/摄像头信号，
 *      驱动 UI 状态刷新、检测告警、联动声光；
 *   6) 进入 LVGL 主循环：lv_timer_handler() 处理界面刷新与事件，
 *      usleep(5ms) 控制循环节拍（低功耗，板端常驻运行）。
 *
 * 与 sim_main.c 的区别：本文件运行在真实开发板上（fbdev + evdev 真实硬件），
 * sim_main.c 是 PC 上用 SDL 窗口模拟同一套 UI（用于开发调试）。
 */
#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl_port.h"
#include "ui/ui_home.h"
#include "dev_bridge.h"
#include "cam_feed.h"
#include "detector.h"
#include "config_mgr.h"
#include "log_svc.h"
#include "mqtt_hub.h"
#include "health_mgr.h"
#include "ota_mgr.h"
#include "time_sync.h"
#include "cloud_detect.h"
#include "http_get.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/reboot.h>

static volatile sig_atomic_t g_quit = 0;

static void _on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

int main(void) {
    /* 时区：系统时钟保持 UTC（SNTP 设置的就是 UTC），显示/日志由 localtime 按 TZ 转换。
     * 板端默认没有 TZ，会显示成 UTC（比北京时间慢 8 小时）；这里默认 CST-8，
     * 已有 TZ 环境变量时不覆盖。必须在 log_init 之前设置，首行日志才正确。 */
    if (!getenv("TZ")) {
        setenv("TZ", "CST-8", 1);
        tzset();
    }

    /* 先启动日志：配置加载/显示初始化失败也能落盘追溯 */
    log_init(NULL);
    LOGI("MAIN", "application start");

    /* 时间同步（后台线程）：板端无 RTC 电池时日志会停在 1970。
     * 服务器优先 GATEWAY_NTP_SERVER，默认 192.168.3.26（开发机）。 */
    time_sync_start(NULL);

    /* T8：单实例锁 + 看门狗。已有实例时直接退出，避免两个进程抢设备/日志。 */
    if (health_init() == -2) {
        LOGW("MAIN", "another instance is running, exit");
        log_close();
        return 1;
    }

    /* 板端 RTC/NTP 未同步时日志时间会是 1970，显式提示一次 */
    time_t now = time(NULL);
    if (now < 1700000000L)
        LOGW("MAIN", "system time not set (%ld), log timestamp unreliable", (long)now);

    config_init(NULL);

    /* T11-B2：HTTP 接口鉴权 Key（精判 / OTA / 白名单），来自 gateway.json */
    {
        const struct app_config *c = config_get();
        if (c) {
            cloud_detect_set_api_key(c->cloud_api_key);
            http_get_set_api_key(c->cloud_api_key);
        }
    }

    /* T9：初始化 OTA 目录/状态；安装中断或未确认启动超限时自动回滚 */
    if (ota_init() > 0)
        LOGW("MAIN", "OTA rolled back, restart recommended");

    /* T9：本地安装入口（运维/测试用）。安装完成会原子切换 current 并退出，
     * 下一次由 rcS 从 current/lvgl_gateway 启动新版本，启动成功后自动确认。 */
    const char *ota_pkg = getenv("GATEWAY_OTA_INSTALL");
    if (ota_pkg && ota_pkg[0]) {
        int rc = ota_install(ota_pkg);
        if (rc == 0)
            LOGI("OTA", "install %s ok, current=%s (restart to apply)",
                 ota_pkg, ota_current_version());
        else
            LOGE("OTA", "install %s failed", ota_pkg);
        log_close();
        return rc == 0 ? 0 : 1;
    }

    /* T9：网络 OTA（拉取式）。设置 GATEWAY_OTA_URL 后，启动时检查一次：
     * 拉 manifest → 版本比对 → 下载 → sha256 校验 → 安装 → 重启生效。
     * GATEWAY_OTA_NO_REBOOT=1 时只安装不重启（便于调试）。 */
    const char *ota_url = getenv("GATEWAY_OTA_URL");
    if (ota_url && ota_url[0]) {
        int rc = ota_check(ota_url);
        if (rc == 1) {
            if (getenv("GATEWAY_OTA_NO_REBOOT")) {
                LOGI("MAIN", "OTA installed, reboot manually to apply");
            } else {
                LOGI("MAIN", "OTA installed, rebooting to apply");
                health_shutdown();
                log_close();
                sync();
                reboot(RB_AUTOBOOT);
            }
        }
    }

    /* T9+：启动 OTA 后台线程（定时拉取 + 供 MQTT 下发触发）。
     * GATEWAY_OTA_URL 作为默认 manifest；GATEWAY_OTA_INTERVAL_MIN>0 时周期性检查。 */
    const char *ota_itv = getenv("GATEWAY_OTA_INTERVAL_MIN");
    int ota_min = (ota_itv && ota_itv[0]) ? atoi(ota_itv) : 0;
    ota_periodic_start(ota_url ? ota_url : "", ota_min);

    /* T6：启动 MQTT 常驻线程并加载本地待补传事件（在 UI 事件产生之前） */
    mqtt_hub_start();

    if (lvgl_port_init() != 0) {
        LOGW("MAIN", "lvgl port init failed, continue with fallback");
    }

    // 创建主屏：深色背景 + 智慧安防网关主页（与模拟器一致）
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
    ui_home_create(scr);

    // 启动外设桥接（KEY0/AP3216C/ICM20608 采集 → UI；LED/蜂鸣器 ← 状态）
    dev_bridge_start();

    /* T9：UI/外设/检测初始化完成，视为健康检查通过，确认新版本 */
    ota_confirm();

    LOGI("MAIN", "UI ready, entering main loop");

    /* 基础退出路径：SIGINT/SIGTERM 时退出主循环并 flush 日志。
     * 完整的线程回收/资源释放由后续 T5 优雅退出统一处理。 */
    signal(SIGINT, _on_signal);
    signal(SIGTERM, _on_signal);

    unsigned tick = 0;
    int reboot_pending = 0;
    int restart_pending = 0;
    while (!g_quit) {
        lv_timer_handler();
        usleep(5000);

        /* 每 ~500ms 检查一次线程心跳并喂狗；主线程卡死时自然停止喂狗 */
        if (++tick >= 100) {
            tick = 0;
            health_tick();
        }

        /* UI“重启应用”：优雅清理后 exec 自身（摄像头卡死时的恢复手段） */
        if (health_take_restart()) {
            LOGI("MAIN", "restart requested by UI");
            restart_pending = 1;
            g_quit = 1;
        }

        /* T9+：后台/MQTT 触发的 OTA 安装完成 → 走优雅退出后重启生效 */
        if (ota_take_installed()) {
            LOGI("MAIN", "OTA installed by background check, restarting");
            reboot_pending = 1;
            g_quit = 1;
        }
    }

    LOGI("MAIN", "application exit");
    ota_periodic_stop();
    time_sync_stop();
    dev_bridge_stop();     /* 先停生产者，避免退出过程中继续产生事件 */
    cam_feed_stop();
    detector_stop();
    mqtt_hub_stop();       /* 落盘剩余事件 + 停 writer 线程 */

    if (reboot_pending) {
        LOGI("MAIN", "rebooting to apply OTA");
        health_shutdown();
        log_close();
        sync();
        reboot(RB_AUTOBOOT);
        return 0;
    }

    health_shutdown();
    log_close();

    if (restart_pending) {
        sync();
        execl("/proc/self/exe", "lvgl_gateway", (char *)NULL);
        /* exec 失败才走到这里（日志已关闭，写 stderr） */
        fprintf(stderr, "[MAIN] restart exec failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
