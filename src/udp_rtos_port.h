/*
 * udp_rtos_port.h — RTOS 适配层接口
 *
 * 协议栈需要 RTOS 提供以下原语以支持阻塞操作和线程安全：
 *   - 互斥锁（mutex）：保护协议栈内部数据结构的并发访问
 *   - 计数信号量（semaphore）：支持带超时的阻塞等待
 *
 * 使用方式：
 *   裸机模式：  #include "udp_stack.h"  （自动得到 no-op 实现）
 *   RTOS 模式： #define UDP_RTOS
 *              #include "udp_rtos_stub.h"  —— 测试用 pthread 桩
 *              或在你的 .c 文件中实现下方声明的 7 个函数
 *
 * 移植指南：文件末尾包含 FreeRTOS / RT-Thread 的参考实现。
 */

#ifndef UDP_RTOS_PORT_H
#define UDP_RTOS_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 类型定义（裸机和 RTOS 共用）
 * ================================================================ */

#ifndef UDP_MUTEX_T_DEFINED
#define UDP_MUTEX_T_DEFINED
typedef struct {
    void   *handle;
    uint8_t _data[32];
} udp_mutex_t;
#endif

#ifndef UDP_SEM_T_DEFINED
#define UDP_SEM_T_DEFINED
typedef struct {
    void   *handle;
    uint8_t _data[40];
} udp_sem_t;
#endif

/* ================================================================
 * RTOS 模式：声明接口（由用户实现或 udp_rtos_stub.h 提供）
 * 裸机模式：直接提供 static inline no-op 回退
 * ================================================================ */

#ifdef UDP_RTOS

void udp_mutex_init(udp_mutex_t *m);
void udp_mutex_lock(udp_mutex_t *m);
void udp_mutex_unlock(udp_mutex_t *m);

void udp_sem_init (udp_sem_t *s, uint32_t max_count, uint32_t initial_count);
int  udp_sem_wait (udp_sem_t *s, uint32_t timeout_ms);
void udp_sem_signal(udp_sem_t *s);

#else /* !UDP_RTOS — bare-metal no-op */

static inline void udp_mutex_init(udp_mutex_t *m)  { (void)m; }
static inline void udp_mutex_lock(udp_mutex_t *m)  { (void)m; }
static inline void udp_mutex_unlock(udp_mutex_t *m){ (void)m; }

static inline void udp_sem_init (udp_sem_t *s, uint32_t max, uint32_t init)
{ (void)s; (void)max; (void)init; }

static inline int  udp_sem_wait (udp_sem_t *s, uint32_t timeout_ms)
{ (void)s; (void)timeout_ms; return -1; }  /* 裸机永远"超时" */

static inline void udp_sem_signal(udp_sem_t *s) { (void)s; }

#endif /* UDP_RTOS */

#ifdef __cplusplus
}
#endif

#endif /* UDP_RTOS_PORT_H */


/*
 * ================================================================
 * 常见 RTOS 移植参考
 * ================================================================
 *
 * ---- FreeRTOS ----
 *
 *   #include "FreeRTOS.h"
 *   #include "semphr.h"
 *
 *   typedef struct { SemaphoreHandle_t h; StaticSemaphore_t b; } udp_mutex_t;
 *   typedef struct { SemaphoreHandle_t h; StaticSemaphore_t b; } udp_sem_t;
 *
 *   void udp_mutex_init(udp_mutex_t *m) {
 *       m->h = xSemaphoreCreateMutexStatic(&m->b); }
 *   void udp_mutex_lock(udp_mutex_t *m)  { xSemaphoreTake(m->h, portMAX_DELAY); }
 *   void udp_mutex_unlock(udp_mutex_t *m){ xSemaphoreGive(m->h); }
 *
 *   void udp_sem_init(udp_sem_t *s, uint32_t max, uint32_t init) {
 *       s->h = xSemaphoreCreateCountingStatic(max, init, &s->b); }
 *   int  udp_sem_wait(udp_sem_t *s, uint32_t to) {
 *       TickType_t t = (to == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(to);
 *       return (xSemaphoreTake(s->h, t) == pdTRUE) ? 0 : -1; }
 *   void udp_sem_signal(udp_sem_t *s)    { xSemaphoreGive(s->h); }
 *
 * ---- RT-Thread ----
 *
 *   #include <rtthread.h>
 *
 *   typedef struct rt_mutex     udp_mutex_t;
 *   typedef struct rt_semaphore udp_sem_t;
 *
 *   void udp_mutex_init(udp_mutex_t *m)  { rt_mutex_init(m, "udp", RT_IPC_FLAG_FIFO); }
 *   void udp_mutex_lock(udp_mutex_t *m)  { rt_mutex_take(m, RT_WAITING_FOREVER); }
 *   void udp_mutex_unlock(udp_mutex_t *m){ rt_mutex_release(m); }
 *   void udp_sem_init(udp_sem_t *s, uint32_t max, uint32_t init)
 *                                         { rt_sem_init(s, "udp", init, RT_IPC_FLAG_FIFO); }
 *   int  udp_sem_wait(udp_sem_t *s, uint32_t to) {
 *       rt_int32_t t = (to == UINT32_MAX) ? RT_WAITING_FOREVER
 *                    : rt_tick_from_millisecond((rt_int32_t)to);
 *       return (rt_sem_take(s, t) == RT_EOK) ? 0 : -1; }
 *   void udp_sem_signal(udp_sem_t *s)    { rt_sem_release(s); }
 */
