/**
 * thread_util.h — 统一线程创建（显式栈大小）
 *
 * 目的：嵌入式设备上为每个线程设定与职责匹配的栈，避免默认 8MB 虚拟栈
 *       掩盖栈溢出风险，也便于审查“哪个线程用了多少栈”。
 *
 * 约定（见 docs/locking.md）：
 *   - 采集/网络/日志/写盘：128~512KB（只做 I/O 与小块缓冲）
 *   - 摄像头转换：1MB（V4L2 结构体 + 临时缓冲）
 *   - 精判推理：2MB（ncnn 内部临时对象）
 */
#ifndef THREAD_UTIL_H
#define THREAD_UTIL_H

#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 以指定栈大小创建线程
 * @param tid   输出线程句柄（可为 NULL，表示不关心）
 * @param stack 栈大小（字节）；< PTHREAD_STACK_MIN 时用系统默认
 * @param fn    线程函数
 * @param arg   参数
 * @return 0 成功；非 0 = pthread_create 错误码
 */
int thread_start(pthread_t *tid, size_t stack, void *(*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif
