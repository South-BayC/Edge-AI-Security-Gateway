/**
 * ota_mgr.c — 应用级 OTA 实现（T9）
 *
 * 依赖板端命令：sha256sum / tar / gzip / cp / rm（Buildroot BusyBox）。
 * 注意：BusyBox tar 通常未编入 gzip 支持，.tar.gz 统一走 `gzip -dc | tar -xf -`。
 * 数字签名当前只检查 signature.sig 是否存在，验签接口留到后续（生产必须启用）。
 */
#include "ota_mgr.h"

#include "http_get.h"
#include "log_svc.h"
#include "ota_hmac.h"
#include "thread_util.h"
#include "error_code.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    ota_state_t state;
    char previous[32];
    char target[32];
    int boot_count;
} ota_meta_t;

static char s_root[PATH_MAX];
static char s_state_path[PATH_MAX];
static char s_cur[32];
static ota_meta_t s_meta;

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

static int run_cmd(const char *fmt, ...)
{
    char cmd[768];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    int rc = system(cmd);
    if (rc == -1)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

static void rm_rf(const char *path)
{
    if (path && path[0])
        run_cmd("rm -rf '%s'", path);
}

/* 解压 .tar.gz：BusyBox tar 通常没编入 gzip 支持（`-z`/`-a` 都不可用），
 * 先用 gzip -dc 解压再交给 tar；宿主/桌面环境则回退 GNU tar 的 -z。 */
static int extract_tar_gz(const char *archive, const char *dir)
{
    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "gzip -dc '%s' | tar -xf - -C '%s'", archive, dir);
    if (run_cmd("%s", cmd) == 0)
        return 0;
    return run_cmd("tar -xzf '%s' -C '%s'", archive, dir);
}

static int valid_version(const char *v)
{
    if (!v || !v[0])
        return 0;
    for (const char *p = v; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static void copy_json_field(const char *buf, const char *key, char *out, int n)
{
    out[0] = 0;
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(buf, needle);
    if (!p)
        return;
    p += strlen(needle);
    int i = 0;
    while (*p && *p != '"' && i < n - 1)
        out[i++] = *p++;
    out[i] = 0;
}

static void meta_load(void)
{
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.state = OTA_IDLE;

    FILE *fp = fopen(s_state_path, "r");
    if (!fp)
        return;

    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;

    const char *p = strstr(buf, "\"state\":");
    if (p)
        s_meta.state = (ota_state_t)atoi(p + 8);
    copy_json_field(buf, "previous", s_meta.previous, sizeof(s_meta.previous));
    copy_json_field(buf, "target", s_meta.target, sizeof(s_meta.target));
    p = strstr(buf, "\"boot_count\":");
    if (p)
        s_meta.boot_count = atoi(p + 13);
}

static void meta_save(void)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", s_state_path) >= (int)sizeof(tmp))
        return;

    FILE *fp = fopen(tmp, "w");
    if (!fp)
        return;

    fprintf(fp,
            "{\"state\":%d,\"previous\":\"%s\",\"target\":\"%s\",\"boot_count\":%d}\n",
            (int)s_meta.state, s_meta.previous, s_meta.target, s_meta.boot_count);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp, s_state_path) != 0) {
        unlink(tmp);
        return;
    }
    chmod(s_state_path, 0600);
}

const char *ota_current_version(void)
{
    s_cur[0] = 0;

    char link[PATH_MAX];
    if (snprintf(link, sizeof(link), "%s/current", s_root) >= (int)sizeof(link))
        return s_cur;

    char buf[PATH_MAX];
    ssize_t r = readlink(link, buf, sizeof(buf) - 1);
    if (r <= 0)
        return s_cur;
    buf[r] = 0;

    const char *base = strrchr(buf, '/');
    snprintf(s_cur, sizeof(s_cur), "%s", base ? base + 1 : buf);
    return s_cur;
}

static int switch_current(const char *version)
{
    if (!valid_version(version))
        return -1;

    char tmp[PATH_MAX], link[PATH_MAX], target[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/current.tmp", s_root);
    snprintf(link, sizeof(link), "%s/current", s_root);
    snprintf(target, sizeof(target), "releases/%s", version);

    unlink(tmp);
    if (symlink(target, tmp) != 0)
        return -1;
    if (rename(tmp, link) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int manifest_read(const char *pkg_dir, char *version, int vn,
                         char *hardware, int hn)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", pkg_dir);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;

    copy_json_field(buf, "version", version, vn);
    copy_json_field(buf, "hardware", hardware, hn);
    return valid_version(version) ? 0 : -1;
}

int ota_rollback(void)
{
    if (!s_meta.previous[0] || strcmp(s_meta.previous, "none") == 0) {
        LOGE("OTA", "rollback failed: no previous version");
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    if (switch_current(s_meta.previous) != 0) {
        LOGE("OTA", "rollback to %s failed", s_meta.previous);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    s_meta.state = OTA_ROLLED_BACK;
    s_meta.boot_count = 0;
    meta_save();
    LOGW("OTA", "rolled back to %s", s_meta.previous);
    return 0;
}

int ota_init(void)
{
    const char *env = getenv("GATEWAY_OTA_DIR");
    snprintf(s_root, sizeof(s_root), "%s", (env && env[0]) ? env : "/opt/edge-gateway");

    mkdir_p(s_root, 0755);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/releases", s_root); mkdir_p(path, 0755);
    snprintf(path, sizeof(path), "%s/ota", s_root); mkdir_p(path, 0755);
    snprintf(path, sizeof(path), "%s/ota/packages", s_root); mkdir_p(path, 0755);
    snprintf(path, sizeof(path), "%s/ota/staging", s_root); mkdir_p(path, 0755);

    snprintf(s_state_path, sizeof(s_state_path), "%s/ota/state.json", s_root);
    meta_load();

    if (s_meta.state == OTA_INSTALLING) {
        LOGW("OTA", "installation interrupted, rolling back");
        ota_rollback();
        return 1;
    }

    if (s_meta.state == OTA_PENDING_CONFIRM) {
        s_meta.boot_count++;
        if (s_meta.boot_count > 1) {
            LOGW("OTA", "new version not confirmed after boot, rolling back");
            ota_rollback();
            return 1;
        }
        meta_save();
    }

    LOGI("OTA", "init root=%s state=%s current=%s",
         s_root, ota_state_name(s_meta.state), ota_current_version());
    return 0;
}

int ota_install(const char *pkg_dir)
{
    if (!pkg_dir || !pkg_dir[0])
        return -1;

    char manifest[PATH_MAX], payload[PATH_MAX], checks[PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", pkg_dir);
    snprintf(payload, sizeof(payload), "%s/payload.tar.gz", pkg_dir);
    snprintf(checks, sizeof(checks), "%s/checksums.sha256", pkg_dir);

    if (access(manifest, R_OK) != 0 || access(payload, R_OK) != 0 ||
        access(checks, R_OK) != 0) {
        LOGE("OTA", "package incomplete: %s", pkg_dir);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    s_meta.state = OTA_INSTALLING;
    meta_save();

    char version[32], hardware[32];
    if (manifest_read(pkg_dir, version, sizeof(version),
                      hardware, sizeof(hardware)) != 0) {
        LOGE("OTA", "invalid manifest in %s", pkg_dir);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    if (hardware[0] && strcmp(hardware, "imx6ull") != 0) {
        LOGE("OTA", "hardware mismatch: %s", hardware);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    /* 1. SHA-256 完整性校验（checksums.sha256 在包目录下执行） */
    if (run_cmd("cd '%s' && sha256sum -c checksums.sha256 >/dev/null 2>&1",
                pkg_dir) != 0) {
        LOGE("OTA", "sha256 verify failed");
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    /* 2. 解压到 staging/<version> */
    char stage[PATH_MAX], release[PATH_MAX];
    snprintf(stage, sizeof(stage), "%s/ota/staging/%s", s_root, version);
    rm_rf(stage);
    if (mkdir_p(stage, 0755) != 0 ||
        extract_tar_gz(payload, stage) != 0) {
        LOGE("OTA", "extract failed");
        rm_rf(stage);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    char bin[PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/lvgl_gateway", stage);
    if (access(bin, X_OK) != 0) {
        LOGE("OTA", "payload missing lvgl_gateway at root");
        rm_rf(stage);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    /* 3. 安装到 releases/<version> */
    snprintf(release, sizeof(release), "%s/releases/%s", s_root, version);
    rm_rf(release);

    if (rename(stage, release) != 0) {
        if (run_cmd("cp -a '%s' '%s'", stage, release) != 0) {
            LOGE("OTA", "install to %s failed", release);
            rm_rf(stage);
            s_meta.state = OTA_FAILED;
            meta_save();
            return -1;
        }
        rm_rf(stage);
    }

    /* 4. 记录 previous 并原子切换 current */
    const char *cur_ver = ota_current_version();
    snprintf(s_meta.previous, sizeof(s_meta.previous), "%s",
             cur_ver[0] ? cur_ver : "none");
    snprintf(s_meta.target, sizeof(s_meta.target), "%s", version);

    if (switch_current(version) != 0) {
        LOGE("OTA", "switch current -> %s failed", version);
        if (s_meta.previous[0] && strcmp(s_meta.previous, "none") != 0)
            switch_current(s_meta.previous);
        s_meta.state = OTA_FAILED;
        meta_save();
        return -1;
    }

    s_meta.state = OTA_PENDING_CONFIRM;
    s_meta.boot_count = 0;
    meta_save();

    LOGI("OTA", "installed %s (previous=%s), pending confirm",
         version, s_meta.previous);
    return 0;
}

int ota_confirm(void)
{
    if (s_meta.state != OTA_PENDING_CONFIRM)
        return 0;

    s_meta.state = OTA_CONFIRMED;
    s_meta.boot_count = 0;
    meta_save();
    LOGI("OTA", "version %s confirmed", s_meta.target);
    return 0;
}

/* ================================================================
 * T9 网络 OTA（拉取式）
 *   manifest.json（compact JSON）：
 *     {"product":"edge-ai-gateway","hardware":"imx6ull","version":"1.1.0",
 *      "package":"pkg-1.1.0.tar.gz","sha256":"..."}
 *   其中 package 可写相对文件名（相对 manifest 所在目录）或完整 http URL；
 *   包内需包含 manifest.json / payload.tar.gz / checksums.sha256（同本地安装包）。
 * ================================================================ */

/* 版本号按数字段比较：1.10.0 > 1.9.0 */
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        long va = 0, vb = 0;
        while (isdigit((unsigned char)*a))
            va = va * 10 + (*a++ - '0');
        while (isdigit((unsigned char)*b))
            vb = vb * 10 + (*b++ - '0');
        if (va != vb)
            return va < vb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static int sha256_of_file(const char *path, char *out, int n)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    char buf[160] = {0};
    int ok = (fgets(buf, sizeof(buf), fp) != NULL);
    pclose(fp);
    if (!ok)
        return -1;

    char *sp = strchr(buf, ' ');
    if (sp)
        *sp = 0;
    snprintf(out, (size_t)n, "%s", buf);
    return 0;
}

/* 从 pkg_dir/manifest.json 取单个字段（要求紧凑 JSON："key":"value"） */
static void manifest_field(const char *dir, const char *key, char *out, int n)
{
    char path[PATH_MAX], buf[4096];

    out[0] = 0;
    snprintf(path, sizeof(path), "%s/manifest.json", dir);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[r] = 0;

    copy_json_field(buf, key, out, n);
}

/* 把相对包名解析到 manifest 所在目录 */
static void url_resolve(const char *base, const char *ref, char *out, int n)
{
    if (!ref || !ref[0]) {
        out[0] = 0;
        return;
    }
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, (size_t)n, "%s", ref);
        return;
    }

    const char *slash = strrchr(base, '/');
    if (!slash) {
        snprintf(out, (size_t)n, "%s", ref);
        return;
    }
    int dirlen = (int)(slash - base) + 1;
    snprintf(out, (size_t)n, "%.*s%s", dirlen, base, ref);
}

/* T11：包完整性认证（HMAC-SHA256，板端无 openssl 也可用）
 * 密钥优先级：GATEWAY_OTA_HMAC_KEY > <root>/ota/ota.key
 * 返回 0=通过；-1=失败（缺密钥/缺值/不匹配） */
static int verify_hmac(const char *pkg_file, const char *want_hex)
{
    char key[160];
    const char *env = getenv("GATEWAY_OTA_HMAC_KEY");

    if (env && env[0]) {
        snprintf(key, sizeof(key), "%s", env);
    } else {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/ota/ota.key", s_root);

        FILE *fp = fopen(path, "r");
        if (!fp) {
            LOGE("OTA", "hmac key not found: %s（见 docs 3.15 部署密钥）", path);
            return -1;
        }
        if (!fgets(key, sizeof(key), fp)) {
            fclose(fp);
            LOGE("OTA", "hmac key file empty: %s", path);
            return -1;
        }
        fclose(fp);

        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == '\n' || key[klen - 1] == '\r'))
            key[--klen] = 0;
    }

    if (!key[0]) {
        LOGE("OTA", "hmac key is empty");
        return -1;
    }

    char got[65];
    if (hmac_sha256_file_hex(pkg_file, key, got, sizeof(got)) != 0) {
        LOGE("OTA", "hmac compute failed: %s", pkg_file);
        return -1;
    }

    /* 大小写无关比较（并做常数时间累加，避免逐字节早退） */
    int diff = 0;
    for (int i = 0; i < 64; i++) {
        char a = got[i], b = want_hex[i];
        if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        diff |= (a ^ b);
    }
    if (diff != 0) {
        LOGE("OTA", "hmac mismatch [%s]: got=%.16s... want=%.16s...",
             err_str(ERR_OTA_HMAC), got, want_hex);
        return -1;
    }

    LOGI("OTA", "hmac ok");
    return 0;
}

int ota_check(const char *manifest_url)
{
    if (!manifest_url || !manifest_url[0])
        return -1;

    /* 安装中/待确认期间不再检查，避免把正在验证的版本覆盖掉 */
    if (s_meta.state == OTA_INSTALLING || s_meta.state == OTA_PENDING_CONFIRM) {
        LOGI("OTA", "check skipped: state=%s", ota_state_name(s_meta.state));
        return 0;
    }

    char dir[PATH_MAX], local[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/ota/staging/download", s_root);
    rm_rf(dir);
    if (mkdir_p(dir, 0755) != 0)
        return -1;

    snprintf(local, sizeof(local), "%s/manifest.json", dir);
    LOGI("OTA", "check %s", manifest_url);
    if (http_get_file(manifest_url, local) != 0) {
        LOGE("OTA", "fetch manifest failed [%s]", err_str(ERR_OTA_MANIFEST));
        return -1;
    }

    char version[32], hardware[32], pkg_ref[256], want_sha[96];
    if (manifest_read(dir, version, sizeof(version), hardware, sizeof(hardware)) != 0) {
        LOGE("OTA", "invalid manifest");
        return -1;
    }
    manifest_field(dir, "package", pkg_ref, sizeof(pkg_ref));
    manifest_field(dir, "sha256", want_sha, sizeof(want_sha));

    if (hardware[0] && strcmp(hardware, "imx6ull") != 0) {
        LOGE("OTA", "hardware mismatch: %s", hardware);
        return -1;
    }

    const char *cur = ota_current_version();
    if (cur[0] && version_cmp(version, cur) <= 0) {
        LOGI("OTA", "up to date (current=%s, remote=%s)", cur, version);
        return 0;
    }

    if (!pkg_ref[0]) {
        LOGE("OTA", "manifest missing package url");
        return -1;
    }

    char pkg_url[512], pkg_file[PATH_MAX];
    url_resolve(manifest_url, pkg_ref, pkg_url, sizeof(pkg_url));
    snprintf(pkg_file, sizeof(pkg_file), "%s/pkg.tar.gz", dir);

    LOGI("OTA", "download %s (current=%s remote=%s)",
         pkg_url, cur[0] ? cur : "none", version);
    if (http_get_file(pkg_url, pkg_file) != 0) {
        LOGE("OTA", "download failed [%s]", err_str(ERR_OTA_FETCH));
        return -1;
    }

    if (want_sha[0]) {
        char got[96] = {0};
        if (sha256_of_file(pkg_file, got, sizeof(got)) != 0 ||
            strcmp(got, want_sha) != 0) {
            LOGE("OTA", "sha256 mismatch [%s]: got=%s want=%s",
                 err_str(ERR_OTA_SHA), got, want_sha);
            unlink(pkg_file);
            return -1;
        }
    }

    /* T11：HMAC 完整性认证（manifest 的 "hmac" 字段）
     * 默认强制（require_auth=1）；调试可用 GATEWAY_OTA_REQUIRE_SIG=0 放行无认证包 */
    char want_hmac[96] = {0};
    manifest_field(dir, "hmac", want_hmac, sizeof(want_hmac));

    const char *rs = getenv("GATEWAY_OTA_REQUIRE_SIG");
    int require_auth = (rs && rs[0] == '0') ? 0 : 1;

    if (want_hmac[0]) {
        if (verify_hmac(pkg_file, want_hmac) != 0) {
            LOGE("OTA", "hmac check failed, reject %s [%s]",
                 version, err_str(ERR_OTA_HMAC));
            unlink(pkg_file);
            return -1;
        }
    } else if (require_auth) {
        LOGE("OTA", "manifest missing \"hmac\" (require=1), reject");
        unlink(pkg_file);
        return -1;
    } else {
        LOGW("OTA", "unauthenticated package accepted (GATEWAY_OTA_REQUIRE_SIG=0)");
    }

    /* 解出安装包目录（manifest.json / payload.tar.gz / checksums.sha256） */
    char ext[PATH_MAX];
    snprintf(ext, sizeof(ext), "%s/ota/staging/pkg-%s", s_root, version);
    rm_rf(ext);
    if (mkdir_p(ext, 0755) != 0 ||
        extract_tar_gz(pkg_file, ext) != 0) {
        LOGE("OTA", "extract package failed [%s]", err_str(ERR_OTA_EXTRACT));
        rm_rf(ext);
        return -1;
    }

    if (ota_install(ext) != 0) {
        LOGE("OTA", "install %s failed [%s]", version, err_str(ERR_OTA_INSTALL));
        rm_rf(ext);
        return -1;
    }
    rm_rf(ext);

    LOGI("OTA", "update %s ready (previous=%s)",
         version, s_meta.previous[0] ? s_meta.previous : "none");
    return 1;   /* 1 = 已安装，需要重启生效 */
}

ota_state_t ota_get_state(void)
{
    return s_meta.state;
}

const char *ota_state_name(ota_state_t st)
{
    switch (st) {
    case OTA_IDLE:            return "IDLE";
    case OTA_INSTALLING:      return "INSTALLING";
    case OTA_PENDING_CONFIRM: return "PENDING_CONFIRM";
    case OTA_CONFIRMED:       return "CONFIRMED";
    case OTA_ROLLED_BACK:     return "ROLLED_BACK";
    case OTA_FAILED:          return "FAILED";
    default:                  return "?";
    }
}

int ota_pending_confirm(void)
{
    return s_meta.state == OTA_PENDING_CONFIRM;
}

const char *ota_root(void)
{
    return s_root;
}

/* ================================================================
 * T9+：OTA 后台线程（定时拉取 + MQTT 下发触发）
 *   - 定时：每 interval_min 分钟检查一次（0=关闭）
 *   - 触发：ota_request_check(url) 由 MQTT cmd 调用，非阻塞
 *   - 安装成功：置 installed 标志，主循环轮询后统一走优雅退出 + reboot
 * ================================================================ */
static pthread_t s_ota_tid;
static pthread_mutex_t s_ota_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ota_cond = PTHREAD_COND_INITIALIZER;
static int s_ota_running = 0;
static int s_ota_stop = 0;
static int s_ota_request = 0;
static int s_ota_installed = 0;
static char s_ota_url[256];
static int s_ota_interval_min = 0;

static void *ota_thread(void *arg)
{
    (void)arg;

    while (1) {
        char url[sizeof(s_ota_url)];

        pthread_mutex_lock(&s_ota_lock);
        if (s_ota_stop) {
            pthread_mutex_unlock(&s_ota_lock);
            break;
        }

        if (!s_ota_request) {
            if (s_ota_interval_min <= 0) {
                /* 仅 MQTT 触发模式：无限期等待请求 */
                pthread_cond_wait(&s_ota_cond, &s_ota_lock);
                pthread_mutex_unlock(&s_ota_lock);
                continue;
            }

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)s_ota_interval_min * 60;

            while (!s_ota_stop && !s_ota_request) {
                if (pthread_cond_timedwait(&s_ota_cond, &s_ota_lock, &ts)
                        == ETIMEDOUT)
                    break;      /* 定时到点 */
            }
            if (s_ota_stop) {
                pthread_mutex_unlock(&s_ota_lock);
                break;
            }
        }

        int triggered = s_ota_request;
        s_ota_request = 0;
        snprintf(url, sizeof(url), "%s", s_ota_url);
        pthread_mutex_unlock(&s_ota_lock);

        if (!url[0])
            continue;

        LOGI("OTA", "%s check", triggered ? "cmd" : "periodic");
        int rc = ota_check(url);
        if (rc == 1) {
            pthread_mutex_lock(&s_ota_lock);
            s_ota_installed = 1;
            pthread_mutex_unlock(&s_ota_lock);
        }
    }
    return NULL;
}

int ota_periodic_start(const char *url, int interval_min)
{
    if (s_ota_running)
        return 0;

    pthread_mutex_lock(&s_ota_lock);
    snprintf(s_ota_url, sizeof(s_ota_url), "%s", url ? url : "");
    s_ota_interval_min = (interval_min > 0) ? interval_min : 0;
    s_ota_stop = 0;
    s_ota_request = 0;
    s_ota_installed = 0;
    pthread_mutex_unlock(&s_ota_lock);

    if (thread_start(&s_ota_tid, 512 * 1024, ota_thread, NULL) != 0) {
        LOGE("OTA", "ota thread create failed");
        return -1;
    }
    s_ota_running = 1;

    LOGI("OTA", "periodic check %s, interval=%d min",
         (url && url[0]) ? url : "(cmd only)", s_ota_interval_min);
    return 0;
}

void ota_periodic_stop(void)
{
    if (!s_ota_running)
        return;

    pthread_mutex_lock(&s_ota_lock);
    s_ota_stop = 1;
    pthread_cond_broadcast(&s_ota_cond);
    pthread_mutex_unlock(&s_ota_lock);

    pthread_join(s_ota_tid, NULL);
    s_ota_running = 0;
    LOGI("OTA", "periodic check stopped");
}

/* MQTT 下发触发（非阻塞）：换 URL 并唤醒线程 */
int ota_request_check(const char *url)
{
    if (!url || !url[0])
        return -1;

    pthread_mutex_lock(&s_ota_lock);
    snprintf(s_ota_url, sizeof(s_ota_url), "%s", url);
    s_ota_request = 1;
    pthread_cond_broadcast(&s_ota_cond);
    int running = s_ota_running;
    pthread_mutex_unlock(&s_ota_lock);

    if (!running) {
        /* 线程未启动（例如 rcS 未配置周期检查）：直接同步执行一次，
         * 失败也不阻塞过久（HTTP 连接 3s + 传输超时）。 */
        LOGI("OTA", "cmd check (no thread): %s", url);
        if (ota_check(url) == 1) {
            pthread_mutex_lock(&s_ota_lock);
            s_ota_installed = 1;
            pthread_mutex_unlock(&s_ota_lock);
        }
    }
    return 0;
}

/* 主循环轮询：1=新版本已安装，需要重启生效（读后清零） */
int ota_take_installed(void)
{
    int v;
    pthread_mutex_lock(&s_ota_lock);
    v = s_ota_installed;
    s_ota_installed = 0;
    pthread_mutex_unlock(&s_ota_lock);
    return v;
}
