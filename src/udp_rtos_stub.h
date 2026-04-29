/*
 * udp_rtos_stub.h — RTOS 适配层 pthread 桩（仅用于测试）
 *
 * 提供 pthread 版本的类型定义和函数声明。
 * 实际实现在 udp_rtos_stub.c 中（非 static，可跨编译单元链接）。
 *
 * 使用方式（测试）：
 *   gcc -DUDP_RTOS -pthread test.c udp_stack.c udp_rtos_stub.c
 *
 * 嵌入式项目中使用时，请用 udp_rtos_port.h 中的 TODO 实现替换。
 */

#ifndef UDP_RTOS_STUB_H
#define UDP_RTOS_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <errno.h>

/* ================================================================
 * 裸机 errno 桥接
 *
 * udp_stack.h 的 #ifndef guard 在系统 <errno.h> 已定义宏时自动跳过，
 * 导致 udp_stack.c（不含系统头文件）使用裸机 errno 值（EAGAIN=11），
 * 而测试代码（含 <errno.h>）使用系统值（EAGAIN=35）。
 *
 * 强制覆盖为裸机值，保证整个进程内一致性。
 * ================================================================ */
#ifdef errno
#undef errno
#endif
extern int errno;

/* 覆盖系统 errno 常量到裸机值（与 udp_stack.h 中的定义一致）。
   注意：ETIMEDOUT 保留系统值（60），udp_rtos_stub.c 用其检查
   pthread_cond_timedwait 返回值。 */
#undef EAGAIN
#define EAGAIN          11
#undef EWOULDBLOCK
#define EWOULDBLOCK     11
#undef EINVAL
#define EINVAL          22
#undef ENOMEM
#define ENOMEM          12
#undef ENOTCONN
#define ENOTCONN        107
#undef EBADF
#define EBADF           9

/*
 * 互斥锁类型：直接内嵌 pthread_mutex_t
 */
#ifndef UDP_MUTEX_T_DEFINED
#define UDP_MUTEX_T_DEFINED
typedef struct {
    pthread_mutex_t mutex;
} udp_mutex_t;
#endif

/*
 * 计数信号量类型：pthread_mutex + pthread_cond
 */
#ifndef UDP_SEM_T_DEFINED
#define UDP_SEM_T_DEFINED
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint32_t        count;
    uint32_t        max_count;
} udp_sem_t;
#endif

/*
 * 函数声明（实现在 udp_rtos_stub.c）
 */
void udp_mutex_init(udp_mutex_t *m);
void udp_mutex_lock(udp_mutex_t *m);
void udp_mutex_unlock(udp_mutex_t *m);

void udp_sem_init (udp_sem_t *s, uint32_t max_count, uint32_t initial_count);
int  udp_sem_wait (udp_sem_t *s, uint32_t timeout_ms);
void udp_sem_signal(udp_sem_t *s);

#ifdef __cplusplus
}
#endif

#endif /* UDP_RTOS_STUB_H */
