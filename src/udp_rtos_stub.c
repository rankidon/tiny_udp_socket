/*
 * udp_rtos_stub.c — RTOS 适配层 pthread 桩实现
 *
 * 提供非内联（可链接）的 pthread 版互斥锁和信号量实现。
 * 用于在 macOS/Linux 上测试协议栈的 RTOS 阻塞/超时/线程安全功能。
 */

#include "udp_rtos_stub.h"

void udp_mutex_init(udp_mutex_t *m)
{
    pthread_mutex_init(&m->mutex, NULL);
}

void udp_mutex_lock(udp_mutex_t *m)
{
    pthread_mutex_lock(&m->mutex);
}

void udp_mutex_unlock(udp_mutex_t *m)
{
    pthread_mutex_unlock(&m->mutex);
}

void udp_sem_init(udp_sem_t *s, uint32_t max_count, uint32_t initial_count)
{
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count     = initial_count;
    s->max_count = max_count;
}

int udp_sem_wait(udp_sem_t *s, uint32_t timeout_ms)
{
    int ret = 0;

    pthread_mutex_lock(&s->mutex);

    if (timeout_ms == 0) {
        if (s->count > 0) {
            s->count--;
        } else {
            ret = -1;
        }
        pthread_mutex_unlock(&s->mutex);
        return ret;
    }

    while (s->count == 0) {
        if (timeout_ms == UINT32_MAX) {
            pthread_cond_wait(&s->cond, &s->mutex);
        } else {
            struct timeval  now;
            struct timespec ts;

            gettimeofday(&now, NULL);
            ts.tv_sec  = now.tv_sec + (time_t)(timeout_ms / 1000);
            ts.tv_nsec = (now.tv_usec * 1000) + (long)((timeout_ms % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }

            ret = pthread_cond_timedwait(&s->cond, &s->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mutex);
                return -1;
            }
        }
    }

    s->count--;
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

void udp_sem_signal(udp_sem_t *s)
{
    pthread_mutex_lock(&s->mutex);
    if (s->count < s->max_count) {
        s->count++;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
}
