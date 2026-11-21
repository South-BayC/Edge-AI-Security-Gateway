/**
 * sensor_io.c — 传感器设备访问实现（infra 层）
 */
#include "sensor_io.h"
#include "../log_svc.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define IIO_SYSFS   "/sys/bus/iio/devices"

/* 探测 IIO 设备：遍历 iio:device*，读 name 属性匹配（如 "ap3216c"）。
 * 找到则输出设备目录路径（如 /sys/bus/iio/devices/iio:device0） */
int sio_find_iio(const char *dev_name, char *out, size_t len)
{
    DIR *dir = opendir(IIO_SYSFS);
    struct dirent *ent;
    char npath[128];
    char name[64];
    int ret = -1;

    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0) continue;
        snprintf(npath, sizeof(npath), IIO_SYSFS "/%s/name", ent->d_name);
        int fd = open(npath, O_RDONLY);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (read(fd, name, sizeof(name) - 1) > 0) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            if (strcmp(name, dev_name) == 0) {
                snprintf(out, len, IIO_SYSFS "/%s", ent->d_name);
                ret = 0;
            }
        }
        close(fd);
        if (ret == 0) break;
    }
    closedir(dir);
    return ret;
}

/* 打开 IIO 通道属性文件（保持 fd，读取时 lseek 回开头） */
int sio_open_attr(const char *dev_path, const char *attr)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
    return open(path, O_RDONLY);
}

/* 读 IIO 属性整数值（sysfs ASCII → int） */
int sio_read_int(int fd, int *val)
{
    char buf[32] = {0};
    int n;

    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    *val = atoi(buf);
    return 0;
}

/* ---- T3-B：IIO buffer 读取 ---- */
int sio_write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

int sio_find_iio_buffer(const char *dev_name, char *sysfs, size_t slen,
                           char *dev, size_t dlen)
{
    DIR *dir = opendir(IIO_SYSFS);
    struct dirent *ent;
    char npath[128];
    char name[64];
    int ret = -1;

    if (!dir)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0)
            continue;

        snprintf(npath, sizeof(npath), IIO_SYSFS "/%s/name", ent->d_name);
        int fd = open(npath, O_RDONLY);
        if (fd < 0)
            continue;

        memset(name, 0, sizeof(name));
        if (read(fd, name, sizeof(name) - 1) > 0) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            if (strcmp(name, dev_name) == 0) {
                snprintf(sysfs, slen, IIO_SYSFS "/%s", ent->d_name);
                snprintf(dev, dlen, "/dev/%s", ent->d_name);
                ret = 0;
            }
        }
        close(fd);
        if (ret == 0)
            break;
    }
    closedir(dir);
    return ret;
}

int sio_imu_buffer_open(char *devpath, size_t n,
                           char *sysfs_out, size_t slen)
{
    char sysfs[128], dev[64];
    if (sio_find_iio_buffer("icm20608", sysfs, sizeof(sysfs),
                        dev, sizeof(dev)) != 0)
        return -1;

    char path[192];

    /* 先关闭可能残留的 buffer，否则 scan_elements 写入会 EBUSY */
    snprintf(path, sizeof(path), "%s/buffer/enable", sysfs);
    sio_write_sysfs(path, "0");

    static const char *chans[] = {
        "in_accel_x_en", "in_accel_y_en", "in_accel_z_en",
        "in_temp_en",
        "in_anglvel_x_en", "in_anglvel_y_en", "in_anglvel_z_en",
        NULL
    };

    for (int i = 0; chans[i]; i++) {
        snprintf(path, sizeof(path), "%s/scan_elements/%s", sysfs, chans[i]);
        if (sio_write_sysfs(path, "1") != 0) {
            LOGW("IMU", "enable %s failed, fallback sysfs", chans[i]);
            return -1;
        }
    }

    snprintf(path, sizeof(path), "%s/buffer/length", sysfs);
    sio_write_sysfs(path, "64");
    snprintf(path, sizeof(path), "%s/buffer/enable", sysfs);
    if (sio_write_sysfs(path, "1") != 0) {
        LOGW("IMU", "buffer enable failed, fallback sysfs");
        return -1;
    }

    snprintf(devpath, n, "%s", dev);
    if (sysfs_out && slen > 0) {
        strncpy(sysfs_out, sysfs, slen - 1);
        sysfs_out[slen - 1] = 0;
    }

    int fd = open(devpath, O_RDONLY);
    if (fd < 0) {
        sio_write_sysfs(path, "0");
        return -1;
    }
    return fd;
}

int sio_find_key(const char *dev_name)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    int found = -1;
    char path[64];
    char name[256];

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strcmp(name, dev_name) == 0) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}