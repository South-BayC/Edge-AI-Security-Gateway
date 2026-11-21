/**
 * imu_math.c — IMU 姿态分离与滑窗统计
 */
#include "imu_math.h"

#include <math.h>
#include <string.h>

void imu_math_reset(imu_math_t *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
}

int imu_math_linear(imu_math_t *m, int ax, int ay, int az)
{
    if (!m)
        return 0;

    float a[3] = { (float)ax, (float)ay, (float)az };

    if (!m->grav_init) {
        m->grav[0] = a[0];
        m->grav[1] = a[1];
        m->grav[2] = a[2];
        m->grav_init = 1;
        return 0;
    }

    /* 低通估计重力：α=0.02，100Hz 下时间常数≈0.5s */
    for (int i = 0; i < 3; i++)
        m->grav[i] = m->grav[i] * 0.98f + a[i] * 0.02f;

    float dx = a[0] - m->grav[0];
    float dy = a[1] - m->grav[1];
    float dz = a[2] - m->grav[2];
    float mag = sqrtf(dx * dx + dy * dy + dz * dz);

    /* ±16G 量程：32768 LSB = 16g */
    return (int)(mag / 32768.0f * 16.0f * 100.0f);
}

void imu_math_stats(imu_math_t *m, int lin_x100, int *rms, int *peak)
{
    if (!m)
        return;

    if (m->n == IMU_MATH_WIN)
        m->sq -= (long long)m->win[m->idx] * m->win[m->idx];
    else
        m->n++;

    m->win[m->idx] = lin_x100;
    m->sq += (long long)lin_x100 * lin_x100;
    m->idx = (m->idx + 1) % IMU_MATH_WIN;

    int pk = 0;
    for (int i = 0; i < m->n; i++) {
        if (m->win[i] > pk)
            pk = m->win[i];
    }

    if (rms)
        *rms = (m->n > 0) ? (int)(sqrt((double)m->sq / (double)m->n) + 0.5) : 0;
    if (peak)
        *peak = pk;
}
