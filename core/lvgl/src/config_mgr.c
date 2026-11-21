/**
 * config_mgr.c — 应用配置持久化实现（最小 JSON + 原子写）
 *
 * 不依赖第三方 JSON 库：内置递归下降解析器，只支持配置需要的 JSON 子集
 * （对象、数组、字符串、整数、布尔、null）。配置值均为整数/布尔，
 * 字符串仅用于键名，因此解析后不保留字符串值，避免动态内存。
 *
 * 写盘流程：tmp 文件 → write → fsync → rename → fsync 目录。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE          /* O_DIRECTORY / fsync 目录项 */
#endif

#include "config_mgr.h"
#include "log_svc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_MAX_BYTES   8192
#define JSON_NODE_MAX      256
#define JSON_POOL_MAX      4096
#define JSON_KEY_MAX       32

/* ================================================================
 * 最小 JSON 解析器
 * ================================================================ */
typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} jtype_t;

typedef struct {
    jtype_t type;
    int     bval;
    long    ival;
    int     child;                  /* 第一个子节点索引，-1 表示无 */
    int     next;                   /* 下一个兄弟节点索引，-1 表示无 */
    char    key[JSON_KEY_MAX];      /* 对象成员键；数组元素为空 */
} jnode_t;

typedef struct {
    const char *p;
    jnode_t     nodes[JSON_NODE_MAX];
    int         count;
    char        pool[JSON_POOL_MAX];  /* 字符串暂存（解析期间有效） */
    int         pool_used;
    int         err;
} jparser_t;

static void jp_skip(jparser_t *jp)
{
    while (*jp->p && isspace((unsigned char)*jp->p))
        jp->p++;
}

static int jp_alloc(jparser_t *jp)
{
    if (jp->count >= JSON_NODE_MAX) {
        jp->err = 1;
        return -1;
    }
    int i = jp->count++;
    memset(&jp->nodes[i], 0, sizeof(jp->nodes[i]));
    jp->nodes[i].child = -1;
    jp->nodes[i].next = -1;
    return i;
}

/* 解析 JSON 字符串（支持常见转义；\u 简化为 '?'） */
static int jp_string(jparser_t *jp, char *out, int outn)
{
    if (*jp->p != '"')
        return -1;

    jp->p++;
    int n = 0;
    while (*jp->p && *jp->p != '"') {
        char c = *jp->p++;
        if (c == '\\') {
            char e = *jp->p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u':
                /* 配置键均为 ASCII；\u 值不需要保留，跳过 4 位 */
                for (int i = 0; i < 4 && jp->p[i]; i++)
                    jp->p++;
                c = '?';
                break;
            default:
                return -1;
            }
        }
        if (out && n < outn - 1)
            out[n++] = c;
    }

    if (*jp->p != '"')
        return -1;
    jp->p++;
    if (out && outn > 0)
        out[n] = 0;
    return 0;
}

static int jp_value(jparser_t *jp, int node);

/* 对象：{ "key": value, ... } */
static int jp_object(jparser_t *jp, int node)
{
    jp->p++;   /* '{' */
    jp->nodes[node].type = J_OBJ;
    jp_skip(jp);

    if (*jp->p == '}') {
        jp->p++;
        return 0;
    }

    int last = -1;
    while (1) {
        jp_skip(jp);

        char key[JSON_KEY_MAX] = {0};
        if (jp_string(jp, key, sizeof(key)) != 0)
            return -1;

        jp_skip(jp);
        if (*jp->p != ':')
            return -1;
        jp->p++;

        int child = jp_alloc(jp);
        if (child < 0)
            return -1;
        strncpy(jp->nodes[child].key, key, JSON_KEY_MAX - 1);

        if (jp_value(jp, child) != 0)
            return -1;

        if (last < 0)
            jp->nodes[node].child = child;
        else
            jp->nodes[last].next = child;
        last = child;

        jp_skip(jp);
        if (*jp->p == ',') {
            jp->p++;
            continue;
        }
        if (*jp->p == '}') {
            jp->p++;
            break;
        }
        return -1;
    }
    return 0;
}

/* 数组：[ value, ... ] */
static int jp_array(jparser_t *jp, int node)
{
    jp->p++;   /* '[' */
    jp->nodes[node].type = J_ARR;
    jp_skip(jp);

    if (*jp->p == ']') {
        jp->p++;
        return 0;
    }

    int last = -1;
    while (1) {
        int child = jp_alloc(jp);
        if (child < 0)
            return -1;

        if (jp_value(jp, child) != 0)
            return -1;

        if (last < 0)
            jp->nodes[node].child = child;
        else
            jp->nodes[last].next = child;
        last = child;

        jp_skip(jp);
        if (*jp->p == ',') {
            jp->p++;
            continue;
        }
        if (*jp->p == ']') {
            jp->p++;
            break;
        }
        return -1;
    }
    return 0;
}

static int jp_value(jparser_t *jp, int node)
{
    jp_skip(jp);
    char c = *jp->p;

    if (c == '{')
        return jp_object(jp, node);

    if (c == '[')
        return jp_array(jp, node);

    if (c == '"') {
        int off = jp->pool_used;
        if (off < 0 || off >= JSON_POOL_MAX)
            return -1;
        if (jp_string(jp, jp->pool + off, JSON_POOL_MAX - off) != 0)
            return -1;
        jp->pool_used = off + (int)strlen(jp->pool + off) + 1;
        jp->nodes[node].type = J_STR;
        jp->nodes[node].ival = off;      /* 字符串值存 pool，ival 记偏移 */
        return 0;
    }

    if (strncmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        jp->nodes[node].type = J_BOOL;
        jp->nodes[node].bval = 1;
        return 0;
    }
    if (strncmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        jp->nodes[node].type = J_BOOL;
        jp->nodes[node].bval = 0;
        return 0;
    }
    if (strncmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        jp->nodes[node].type = J_NULL;
        return 0;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        long v = strtol(jp->p, &end, 10);
        if (end == jp->p)
            return -1;
        jp->p = end;

        /* 跳过小数/指数（配置不使用，但保证 JSON 兼容） */
        if (*jp->p == '.') {
            jp->p++;
            while (isdigit((unsigned char)*jp->p))
                jp->p++;
        }
        if (*jp->p == 'e' || *jp->p == 'E') {
            jp->p++;
            if (*jp->p == '+' || *jp->p == '-')
                jp->p++;
            while (isdigit((unsigned char)*jp->p))
                jp->p++;
        }

        jp->nodes[node].type = J_NUM;
        jp->nodes[node].ival = v;
        return 0;
    }

    return -1;
}

static int jp_find(const jparser_t *jp, int obj, const char *key)
{
    if (obj < 0 || obj >= jp->count)
        return -1;
    for (int i = jp->nodes[obj].child; i >= 0; i = jp->nodes[i].next) {
        if (strcmp(jp->nodes[i].key, key) == 0)
            return i;
    }
    return -1;
}

static int jp_int(const jparser_t *jp, int obj, const char *key, int def)
{
    int i = jp_find(jp, obj, key);
    if (i < 0)
        return def;
    if (jp->nodes[i].type == J_NUM)
        return (int)jp->nodes[i].ival;
    if (jp->nodes[i].type == J_BOOL)
        return jp->nodes[i].bval;
    return def;
}

static void jp_int_array(const jparser_t *jp, int obj, const char *key,
                         int *out, int n)
{
    int arr = jp_find(jp, obj, key);
    if (arr < 0 || jp->nodes[arr].type != J_ARR)
        return;

    int i = 0;
    for (int child = jp->nodes[arr].child; child >= 0 && i < n;
         child = jp->nodes[child].next) {
        if (jp->nodes[child].type == J_NUM)
            out[i] = (int)jp->nodes[child].ival;
        i++;
    }
}

static int jp_obj(const jparser_t *jp, int obj, const char *key)
{
    int i = jp_find(jp, obj, key);
    if (i < 0 || jp->nodes[i].type != J_OBJ)
        return -1;
    return i;
}

/* 取对象里的字符串字段；成功 1，缺失/类型不符 0 */
static int jp_str(const jparser_t *jp, int obj, const char *key,
                  char *out, int n)
{
    int i = jp_find(jp, obj, key);
    if (i < 0 || jp->nodes[i].type != J_STR || !out || n <= 0)
        return 0;
    snprintf(out, (size_t)n, "%s", jp->pool + jp->nodes[i].ival);
    return 1;
}

/* 解析根对象；成功返回 0，失败 -1 */
static int json_parse_root(const char *text, jparser_t *jp)
{
    memset(jp, 0, sizeof(*jp));
    jp->p = text;

    int root = jp_alloc(jp);
    if (root < 0)
        return -1;

    if (jp_value(jp, root) != 0)
        return -1;

    jp_skip(jp);
    if (*jp->p != 0)
        return -1;

    return (jp->nodes[root].type == J_OBJ) ? 0 : -1;
}

/* ================================================================
 * 默认值 / 范围裁剪 / 序列化
 * ================================================================ */
void config_defaults(struct app_config *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->version = CONFIG_VERSION;
    out->cloud_review = 1;
    out->motion_interval = 1;
    out->motion_diff = 24;
    out->camera_zone = 0;   /* 单摄默认归属前门 */
    out->ap_ps_off = 0;
    out->ap_ir_off = 0;
    out->ap_als_scale = 1000;
    out->brightness = 80;
    out->schedule_on = 0;
    out->sched_start_h = 22;
    out->sched_end_h = 6;

    out->calib_valid = 0;
    out->imu_off[0] = out->imu_off[1] = out->imu_off[2] = 0;
    out->imu_scale[0] = out->imu_scale[1] = out->imu_scale[2] = 1000;
    out->temp_off = 0;
    /* C3：温漂斜率 0=未拟合（纯自动，采集到 2 个温度点后才有值） */
    out->temp_k[0] = out->temp_k[1] = out->temp_k[2] = 0;
    out->temp_ref = 0;
    out->calib_pt_n = 0;

    snprintf(out->mqtt_broker, sizeof(out->mqtt_broker), "%s", "192.168.3.100");
    out->mqtt_port = 1883;
    out->mqtt_tls = 0;   /* 默认明文，启用 TLS 需显式配置 + 编译 mbedTLS */
    snprintf(out->mqtt_ca_path, sizeof(out->mqtt_ca_path), "%s",
             "/opt/edge-gateway/certs/ca.crt");

    out->warn.debounce = 2;
    out->warn.cooldown_sec = 60;
    out->warn.fusion_on = 0;          /* C1/C5：默认关闭，保持即时告警 */
    out->warn.fusion_threshold = 70;
    out->warn.fusion_window_ms = 3000;

    /* 前门：AP3216C 距离 > 30 告警；> 20 只预警（C2 双阈值） */
    out->warn.zones[0].src = WARN_SRC_AP3216C;
    out->warn.zones[0].channel = AP_DATA_PS;
    out->warn.zones[0].threshold = 30;
    out->warn.zones[0].threshold_warn = 20;
    out->warn.zones[0].cmp = WARN_CMP_GT;
    out->warn.zones[0].enabled = 1;

    /* 后门/窗户：默认不触发 */
    out->warn.zones[1].src = WARN_SRC_NONE;
    out->warn.zones[1].cmp = WARN_CMP_GT;
    out->warn.zones[2].src = WARN_SRC_NONE;
    out->warn.zones[2].cmp = WARN_CMP_GT;

    /* 仓库：ICM 去重力线性加速度 > 30 告警；> 15 只预警
     * （C3 起阈值改为 lin 语义，静止≈0，与安装角度无关） */
    out->warn.zones[3].src = WARN_SRC_ICM;
    out->warn.zones[3].channel = IMU_DATA_LIN;
    out->warn.zones[3].threshold = 30;
    out->warn.zones[3].threshold_warn = 15;
    out->warn.zones[3].cmp = WARN_CMP_GT;
    out->warn.zones[3].enabled = 1;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void config_sanitize(struct app_config *c)
{
    if (!c)
        return;

    if (c->version < 1)
        c->version = CONFIG_VERSION;
    if (c->version > CONFIG_VERSION)
        c->version = CONFIG_VERSION;

    c->cloud_review = c->cloud_review ? 1 : 0;
    c->motion_interval = clamp_int(c->motion_interval, 1, 30);
    c->motion_diff = clamp_int(c->motion_diff, 5, 100);
    c->camera_zone = clamp_int(c->camera_zone, 0, 3);
    c->ap_ps_off = clamp_int(c->ap_ps_off, -2048, 2048);
    c->ap_ir_off = clamp_int(c->ap_ir_off, -2048, 2048);
    c->ap_als_scale = clamp_int(c->ap_als_scale, 500, 2000);
    c->brightness = clamp_int(c->brightness, 0, 100);
    c->schedule_on = c->schedule_on ? 1 : 0;
    c->sched_start_h = clamp_int(c->sched_start_h, 0, 23);
    c->sched_end_h = clamp_int(c->sched_end_h, 0, 23);

    c->mqtt_port = clamp_int(c->mqtt_port, 1, 65535);
    c->mqtt_tls = c->mqtt_tls ? 1 : 0;
    c->mqtt_ca_path[sizeof(c->mqtt_ca_path) - 1] = 0;

    c->calib_valid = c->calib_valid ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        c->imu_off[i] = clamp_int(c->imu_off[i], -32768, 32767);
        c->imu_scale[i] = clamp_int(c->imu_scale[i], 500, 2000);
        c->temp_k[i] = clamp_int(c->temp_k[i], -20000, 20000);
    }
    c->temp_off = clamp_int(c->temp_off, -10000, 10000);
    c->temp_ref = clamp_int(c->temp_ref, -10000, 10000);
    c->calib_pt_n = clamp_int(c->calib_pt_n, 0, 4);
    for (int i = 0; i < 4; i++)
        c->calib_pt_temp[i] = clamp_int(c->calib_pt_temp[i], -10000, 10000);
    for (int a = 0; a < 3; a++)
        for (int i = 0; i < 4; i++)
            c->calib_pt_off[a][i] = clamp_int(c->calib_pt_off[a][i], -32768, 32767);

    c->warn.debounce = clamp_int(c->warn.debounce, 1, 20);
    c->warn.cooldown_sec = clamp_int(c->warn.cooldown_sec, 0, 3600);
    c->warn.fusion_on = c->warn.fusion_on ? 1 : 0;
    c->warn.fusion_threshold = clamp_int(c->warn.fusion_threshold, 1, 100);
    c->warn.fusion_window_ms = clamp_int(c->warn.fusion_window_ms, 500, 10000);

    for (int i = 0; i < 4; i++) {
        struct zone_warn_cfg *z = &c->warn.zones[i];
        z->src = clamp_int(z->src, WARN_SRC_NONE, WARN_SRC_ICM);
        z->channel = clamp_int(z->channel, 0, 4);
        z->threshold = clamp_int(z->threshold, 0, 2000);
        z->threshold_warn = clamp_int(z->threshold_warn, 0, 2000);
        /* 预警阈值必须低于告警阈值，否则预警永不生效 */
        if (z->threshold_warn > z->threshold)
            z->threshold_warn = z->threshold;
        z->cmp = clamp_int(z->cmp, WARN_CMP_GT, WARN_CMP_LT);
        z->enabled = z->enabled ? 1 : 0;
    }
}

static int config_serialize(const struct app_config *c, char *buf, size_t n)
{
    int off = snprintf(buf, n,
        "{\n"
        "  \"version\": %d,\n"
        "  \"cloud_review\": %d,\n"
        "  \"mqtt_broker\": \"%s\",\n"
        "  \"mqtt_user\": \"%s\",\n"
        "  \"mqtt_pass\": \"%s\",\n"
        "  \"cloud_api_key\": \"%s\",\n"
        "  \"mqtt_port\": %d, \"mqtt_tls\": %d, \"mqtt_ca_path\": \"%s\",\n"
        "  \"camera_zone\": %d,\n"
        "  \"ap_ps_off\": %d, \"ap_ir_off\": %d, \"ap_als_scale\": %d,\n"
        "  \"motion\": {\"interval\": %d, \"diff\": %d},\n"
        "  \"ui\": {\"brightness\": %d, \"schedule_on\": %d, "
        "\"sched_start_h\": %d, \"sched_end_h\": %d},\n"
        "  \"calib\": {\"valid\": %d, \"imu_off\": [%d, %d, %d], "
        "\"imu_scale\": [%d, %d, %d], \"temp_off\": %d, "
        "\"temp_k\": [%d, %d, %d], \"temp_ref\": %d, \"pts\": %d, "
        "\"pt_temp\": [%d, %d, %d, %d], "
        "\"pt_off_x\": [%d, %d, %d, %d], "
        "\"pt_off_y\": [%d, %d, %d, %d], "
        "\"pt_off_z\": [%d, %d, %d, %d]},\n"
        "  \"warn\": {\n"
        "    \"debounce\": %d,\n"
        "    \"cooldown_sec\": %d,\n"
        "    \"fusion_on\": %d,\n"
        "    \"fusion_threshold\": %d,\n"
        "    \"fusion_window_ms\": %d,\n"
        "    \"zones\": [\n",
        c->version, c->cloud_review,
        c->mqtt_broker, c->mqtt_user, c->mqtt_pass, c->cloud_api_key,
        c->mqtt_port, c->mqtt_tls, c->mqtt_ca_path,
        c->camera_zone, c->ap_ps_off, c->ap_ir_off, c->ap_als_scale,
        c->motion_interval, c->motion_diff,
        c->brightness, c->schedule_on,
        c->sched_start_h, c->sched_end_h,
        c->calib_valid,
        c->imu_off[0], c->imu_off[1], c->imu_off[2],
        c->imu_scale[0], c->imu_scale[1], c->imu_scale[2],
        c->temp_off,
        c->temp_k[0], c->temp_k[1], c->temp_k[2],
        c->temp_ref, c->calib_pt_n,
        c->calib_pt_temp[0], c->calib_pt_temp[1],
        c->calib_pt_temp[2], c->calib_pt_temp[3],
        c->calib_pt_off[0][0], c->calib_pt_off[0][1],
        c->calib_pt_off[0][2], c->calib_pt_off[0][3],
        c->calib_pt_off[1][0], c->calib_pt_off[1][1],
        c->calib_pt_off[1][2], c->calib_pt_off[1][3],
        c->calib_pt_off[2][0], c->calib_pt_off[2][1],
        c->calib_pt_off[2][2], c->calib_pt_off[2][3],
        c->warn.debounce, c->warn.cooldown_sec,
        c->warn.fusion_on, c->warn.fusion_threshold, c->warn.fusion_window_ms);

    if (off < 0 || (size_t)off >= n)
        return -1;

    for (int i = 0; i < 4; i++) {
        const struct zone_warn_cfg *z = &c->warn.zones[i];
        int r = snprintf(buf + off, n - (size_t)off,
            "      {\"src\": %d, \"channel\": %d, \"threshold\": %d, "
            "\"threshold_warn\": %d, \"cmp\": %d, \"enabled\": %d}%s\n",
            z->src, z->channel, z->threshold, z->threshold_warn,
            z->cmp, z->enabled, (i < 3) ? "," : "");
        if (r < 0 || (size_t)(off + r) >= n)
            return -1;
        off += r;
    }

    int r = snprintf(buf + off, n - (size_t)off,
        "    ]\n"
        "  }\n"
        "}\n");
    if (r < 0 || (size_t)(off + r) >= n)
        return -1;

    return off + r;
}

/* ================================================================
 * 文件操作：mkdir -p / 原子写
 * ================================================================ */
static int mkdir_p(const char *path, mode_t mode)
{
    if (!path || !path[0])
        return -1;

    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    size_t len = strlen(tmp);
    if (len == 0)
        return 0;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;

    if (write_all(fd, data, len) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    /* 目录项持久化（掉电后 rename 不丢） */
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir)
            slash[1] = 0;
        else
            *slash = 0;
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return 0;
}

/* ================================================================
 * 模块状态
 * ================================================================ */
static char s_path[PATH_MAX];
static struct app_config g_cfg;
static int g_dirty = 0;
static int g_ready = 0;
static volatile uint32_t g_gen = 0;   /* T11-B3：配置代数（每次 update 自增） */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int try_config_path(const char *path)
{
    if (!path || !path[0])
        return -1;

    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;

    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir)
            slash[1] = 0;
        else
            *slash = 0;
        if (mkdir_p(dir, 0700) != 0)
            return -1;
        if (access(dir, W_OK) != 0)
            return -1;
    } else {
        if (access(".", W_OK) != 0)
            return -1;
    }

    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;
    return 0;
}

static int config_parse_text(const char *text, struct app_config *out)
{
    jparser_t *jp = (jparser_t *)malloc(sizeof(*jp));
    if (!jp)
        return -1;

    if (json_parse_root(text, jp) != 0) {
        LOGW("CONFIG", "JSON parse failed, use defaults");
        free(jp);
        return -1;
    }

    config_defaults(out);

    int root = 0;
    out->version = jp_int(jp, root, "version", out->version);
    out->cloud_review = jp_int(jp, root, "cloud_review", out->cloud_review);
    out->camera_zone = jp_int(jp, root, "camera_zone", out->camera_zone);
    out->ap_ps_off = jp_int(jp, root, "ap_ps_off", out->ap_ps_off);
    out->ap_ir_off = jp_int(jp, root, "ap_ir_off", out->ap_ir_off);
    out->ap_als_scale = jp_int(jp, root, "ap_als_scale", out->ap_als_scale);
    jp_str(jp, root, "mqtt_broker", out->mqtt_broker, sizeof(out->mqtt_broker));
    jp_str(jp, root, "mqtt_user", out->mqtt_user, sizeof(out->mqtt_user));
    jp_str(jp, root, "mqtt_pass", out->mqtt_pass, sizeof(out->mqtt_pass));
    jp_str(jp, root, "cloud_api_key", out->cloud_api_key, sizeof(out->cloud_api_key));
    out->mqtt_port = jp_int(jp, root, "mqtt_port", out->mqtt_port);
    out->mqtt_tls = jp_int(jp, root, "mqtt_tls", out->mqtt_tls);
    jp_str(jp, root, "mqtt_ca_path", out->mqtt_ca_path, sizeof(out->mqtt_ca_path));

    int motion = jp_obj(jp, root, "motion");
    if (motion >= 0) {
        out->motion_interval = jp_int(jp, motion, "interval", out->motion_interval);
        out->motion_diff = jp_int(jp, motion, "diff", out->motion_diff);
    }

    int ui = jp_obj(jp, root, "ui");
    if (ui >= 0) {
        out->brightness = jp_int(jp, ui, "brightness", out->brightness);
        out->schedule_on = jp_int(jp, ui, "schedule_on", out->schedule_on);
        out->sched_start_h = jp_int(jp, ui, "sched_start_h", out->sched_start_h);
        out->sched_end_h = jp_int(jp, ui, "sched_end_h", out->sched_end_h);
    }

    int calib = jp_obj(jp, root, "calib");
    if (calib >= 0) {
        out->calib_valid = jp_int(jp, calib, "valid", out->calib_valid);
        jp_int_array(jp, calib, "imu_off", out->imu_off, 3);
        jp_int_array(jp, calib, "imu_scale", out->imu_scale, 3);
        out->temp_off = jp_int(jp, calib, "temp_off", out->temp_off);
        jp_int_array(jp, calib, "temp_k", out->temp_k, 3);
        out->temp_ref = jp_int(jp, calib, "temp_ref", out->temp_ref);
        out->calib_pt_n = jp_int(jp, calib, "pts", out->calib_pt_n);
        jp_int_array(jp, calib, "pt_temp", out->calib_pt_temp, 4);
        jp_int_array(jp, calib, "pt_off_x", out->calib_pt_off[0], 4);
        jp_int_array(jp, calib, "pt_off_y", out->calib_pt_off[1], 4);
        jp_int_array(jp, calib, "pt_off_z", out->calib_pt_off[2], 4);
    }

    int warn = jp_obj(jp, root, "warn");
    if (warn >= 0) {
        out->warn.debounce = jp_int(jp, warn, "debounce", out->warn.debounce);
        out->warn.cooldown_sec = jp_int(jp, warn, "cooldown_sec", out->warn.cooldown_sec);
        out->warn.fusion_on = jp_int(jp, warn, "fusion_on", out->warn.fusion_on);
        out->warn.fusion_threshold =
            jp_int(jp, warn, "fusion_threshold", out->warn.fusion_threshold);
        out->warn.fusion_window_ms =
            jp_int(jp, warn, "fusion_window_ms", out->warn.fusion_window_ms);

        int zones = jp_find(jp, warn, "zones");
        if (zones >= 0 && jp->nodes[zones].type == J_ARR) {
            int zi = jp->nodes[zones].child;
            for (int i = 0; i < 4 && zi >= 0; i++, zi = jp->nodes[zi].next) {
                struct zone_warn_cfg *z = &out->warn.zones[i];
                z->src = jp_int(jp, zi, "src", z->src);
                z->channel = jp_int(jp, zi, "channel", z->channel);
                z->threshold = jp_int(jp, zi, "threshold", z->threshold);
                z->threshold_warn = jp_int(jp, zi, "threshold_warn", z->threshold_warn);
                z->cmp = jp_int(jp, zi, "cmp", z->cmp);
                z->enabled = jp_int(jp, zi, "enabled", z->enabled);
            }
        }
    }

    /* v1→v2 迁移：ICM 阈值由“合加速度”改为“去重力线性加速度”（C3）。
     * 旧默认 150（合加速度）在新语义下几乎不可能触发，按 30/15 重标。 */
    if (out->version < 2) {
        for (int i = 0; i < 4; i++) {
            struct zone_warn_cfg *z = &out->warn.zones[i];
            if (z->src == WARN_SRC_ICM && z->channel == IMU_DATA_MAG &&
                z->threshold > 100) {
                z->threshold = 30;
                /* v1 没有 threshold_warn 字段，此处出现的值来自新版默认值，
                 * 统一按 LIN 语义重标为 15（不要保留 AP 的 20）。 */
                z->threshold_warn = 15;
                LOGI("CONFIG", "zone=%d IMU threshold migrated to lin semantics (30/15)", i);
            }
        }
    }

    config_sanitize(out);
    free(jp);
    return 0;
}

/* ================================================================
 * 对外接口
 * ================================================================ */
int config_init(const char *path)
{
    const char *env = getenv("GATEWAY_CONFIG");
    const char *cands[8];
    int n = 0;

    if (path && path[0])
        cands[n++] = path;
    if (env && env[0])
        cands[n++] = env;
    cands[n++] = "/opt/edge-gateway/config/gateway.json";
    cands[n++] = "/etc/edge-gateway/gateway.json";
    cands[n++] = "./gateway.json";

    s_path[0] = 0;
    for (int i = 0; i < n; i++) {
        if (try_config_path(cands[i]) == 0) {
            LOGI("CONFIG", "use path %s", s_path);
            break;
        }
    }

    if (!s_path[0]) {
        strncpy(s_path, cands[0], sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = 0;
        LOGW("CONFIG", "no writable path, fallback %s", s_path);
    }

    config_defaults(&g_cfg);

    FILE *fp = fopen(s_path, "rb");
    if (fp) {
        char buf[CONFIG_MAX_BYTES];
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[got] = 0;

        if (got > 0) {
            struct app_config parsed;
            config_defaults(&parsed);
            if (config_parse_text(buf, &parsed) != 0)
                g_dirty = 1;   /* 损坏配置：用默认值并回写修复 */
            g_cfg = parsed;
            if (g_cfg.version < CONFIG_VERSION) {
                g_cfg.version = CONFIG_VERSION;
                g_dirty = 1;   /* 连同迁移后的字段一起回写 */
            }
            LOGI("CONFIG", "loaded %s (version %d)", s_path, g_cfg.version);
        } else {
            g_dirty = 1;
        }
    } else {
        LOGI("CONFIG", "%s not found, use defaults", s_path);
        g_dirty = 1;
    }

    pthread_mutex_lock(&g_lock);
    g_ready = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

const char *config_path(void)
{
    return s_path;
}

const struct app_config *config_get(void)
{
    return &g_cfg;
}

int config_update(const struct app_config *cfg)
{
    if (!cfg)
        return -1;

    pthread_mutex_lock(&g_lock);
    g_cfg = *cfg;
    config_sanitize(&g_cfg);
    g_dirty = 1;
    g_gen++;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

uint32_t config_generation(void)
{
    return __atomic_load_n(&g_gen, __ATOMIC_RELAXED);
}

int config_flush(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_ready || !g_dirty) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    char buf[CONFIG_MAX_BYTES];
    int len = config_serialize(&g_cfg, buf, sizeof(buf));
    if (len <= 0) {
        pthread_mutex_unlock(&g_lock);
        LOGE("CONFIG", "serialize failed");
        return -1;
    }

    int rc = write_atomic(s_path, buf, (size_t)len);
    if (rc == 0)
        g_dirty = 0;
    pthread_mutex_unlock(&g_lock);

    if (rc != 0)
        LOGE("CONFIG", "flush %s failed: %s", s_path, strerror(errno));
    return rc;
}

int config_is_dirty(void)
{
    pthread_mutex_lock(&g_lock);
    int dirty = g_dirty;
    pthread_mutex_unlock(&g_lock);
    return dirty;
}
