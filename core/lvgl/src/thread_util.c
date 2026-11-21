/**
 * thread_util.c — 统一线程创建实现
 */
#include "thread_util.h"

#include <limits.h>      /* PTHREAD_STACK_MIN */

int thread_start(pthread_t *tid, size_t stack, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    int rc;

    if (!fn)
        return -1;

    if (pthread_attr_init(&attr) != 0)
        return -1;

    if (stack >= (size_t)PTHREAD_STACK_MIN)
        (void)pthread_attr_setstacksize(&attr, stack);

    rc = pthread_create(tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}
