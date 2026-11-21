/**
 * sensor_io.h — 传感器设备访问（infra 层，仅 OS/sysfs/input）
 *
 * IIO sysfs 探测/属性读写、ICM20608 buffer 通路、KEY0 input 设备探测。
 * 不含任何业务判定，供 sensor_pipeline 使用。
 */
#ifndef SENSOR_IO_H
#define SENSOR_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按 IIO name 探测设备目录（如 "ap3216c"）；0=成功 */
int sio_find_iio(const char *dev_name, char *out, size_t len);

/** 打开 IIO 通道属性文件；返回 fd 或 -1 */
int sio_open_attr(const char *dev_path, const char *attr);

/** 读 IIO 属性整数值；0=成功 */
int sio_read_int(int fd, int *val);

/** 写 sysfs 文本；0=成功 */
int sio_write_sysfs(const char *path, const char *val);

/** 找 IIO buffer 设备（sysfs 目录 + /dev/iio:deviceX）；0=成功 */
int sio_find_iio_buffer(const char *dev_name, char *sysfs, size_t slen,
                        char *dev, size_t dlen);

/** 打开 IIO buffer（配置通道/长度/使能）；返回 fd 或 -1 */
int sio_imu_buffer_open(char *devpath, size_t n, char *sysfs_out, size_t slen);

/** 按 input 设备名探测（如 "imx6ull-gpio-keys"）；返回 fd 或 -1 */
int sio_find_key(const char *dev_name);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_IO_H */
