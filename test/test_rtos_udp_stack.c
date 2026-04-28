/*
 * test_rtos_udp_stack.c — RTOS 模式阻塞与线程安全测试
 *
 * 在 macOS 上使用 pthread 模拟 RTOS 行为，验证：
 *   1. recvfrom 阻塞 → 来包后唤醒
 *   2. recvfrom SO_RCVTIMEO 超时
 *   3. select 超时
 *   4. 线程安全（多线程并发 send/recv）
 *   5. close 唤醒阻塞的 recvfrom
 *
 * 编译：
 *   gcc -Wall -Wextra -std=c99 -DUDP_RTOS -pthread -I../src \
 *       -o test_rtos_udp_stack test_rtos_udp_stack.c ../src/udp_stack.c ../src/udp_rtos_stub.c
 *
 * 注意：必须 -DUDP_RTOS 启用 RTOS 模式，udp_rtos_stub.h 自动提供 pthread 实现。
 */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

static void alarm_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n!!! TEST TIMEOUT (stuck) !!!\n");
    fflush(stderr);
    _exit(99);
}

/*
 * 关键：确保 UDP_RTOS 已定义（通过命令行 -DUDP_RTOS 或在此定义）
 * udp_rtos_port.h 声明接口，udp_rtos_stub.c 提供 pthread 实现。
 */
#ifndef UDP_RTOS
#define UDP_RTOS
#endif
#include "../src/udp_rtos_stub.h"
#include "../src/udp_stack.h"

/* ================================================================
 * 测试工具
 * ================================================================ */

static int test_pass = 0;
static int test_fail = 0;

#define TEST(name)                                                        \
    do { printf("  TEST: %s ... ", name); } while (0)

#define PASS()                                                            \
    do { printf("PASS\n"); test_pass++; } while (0)

#define FAIL(msg)                                                         \
    do { printf("FAIL (%s)\n", msg); test_fail++; } while (0)

#define CHECK(cond, msg)                                                  \
    do { if (!(cond)) { FAIL(msg); goto done; } } while (0)

/* ---- 辅助函数 ---- */

static uint32_t ip(const char *s)
{
    uint32_t v;
    tiny_udp_inet_pton(AF_INET, s, &v);
    return v;
}

static void make_addr(struct sockaddr_in *a, const char *ip_str, uint16_t port_host)
{
    a->sin_len    = sizeof(*a);
    a->sin_family = AF_INET;
    a->sin_port   = (uint16_t)((port_host >> 8) | (port_host << 8));
    a->sin_addr.s_addr = ip(ip_str);
}

/**
 * @brief 将 tx_pool 中的所有待发包回环到 rx_pool
 */
static void loopback_all(void)
{
    while (g_tx_read_idx < g_tx_write_idx) {
        uint32_t tx_slot = g_tx_read_idx % TX_POOL_SIZE;
        uint32_t rx_slot = g_rx_write_idx % RX_POOL_SIZE;
        uint16_t pkt_len = ((uint16_t)tx_pool[tx_slot][2] << 8) | tx_pool[tx_slot][3];
        uint32_t j;
        for (j = 0; j < pkt_len && j < RX_PKT_MAXLEN; j++) {
            rx_pool[rx_slot][j] = tx_pool[tx_slot][j];
        }
        g_tx_read_idx++;
        g_rx_write_idx++;
    }
}

/* ================================================================
 * 测试 1：阻塞 recvfrom → 数据到达后唤醒
 * ================================================================ */

typedef struct {
    int     fd;
    int     ready;        /* 0=waiting, 1=got data, -1=error */
    int     saved_errno;
    uint8_t buf[256];
    int     len;
} recv_thread_ctx_t;

static void *recv_thread_func(void *arg)
{
    recv_thread_ctx_t *ctx = (recv_thread_ctx_t *)arg;
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    ctx->ready = 0;  /* 即将开始阻塞 */
    ctx->len = tiny_udp_recvfrom(ctx->fd, ctx->buf, sizeof(ctx->buf), 0,
                                 (struct sockaddr *)&from, &fromlen);
    ctx->saved_errno = errno;  /* 立即保存，防止被后续调用覆盖 */
    if (ctx->len >= 0) {
        ctx->ready = 1;
    } else {
        ctx->ready = -1;
    }
    return NULL;
}

static void test_blocking_recvfrom(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    pthread_t thr;
    recv_thread_ctx_t ctx;

    /* 创建并绑定 */
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr_a, "0.0.0.0", 4000);
    make_addr(&addr_b, "0.0.0.0", 4001);
    tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));

    /* SO_RCVTIMEO = 2 秒，用于阻塞 */
    {
        struct timeval tv = { 2, 0 };
        tiny_udp_setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    TEST("recvfrom blocks, then woken by incoming data");
    ctx.fd = fd_b;
    ctx.ready = -2;

    /* 启动线程，它将阻塞在 recvfrom */
    pthread_create(&thr, NULL, recv_thread_func, &ctx);

    /* 等待线程就绪 */
    usleep(50000);  /* 50ms */

    /* 此时线程应该阻塞着，没有数据 */
    CHECK(ctx.ready == 0, "thread should be waiting");

    /* 发送数据 */
    make_addr(&addr_b, "127.0.0.1", 4001);
    tiny_udp_sendto(fd_a, "BLOCKING", 8, 0, (struct sockaddr *)&addr_b, sizeof(addr_b));

    /* 回环 + tick */
    loopback_all();
    tiny_udp_tick();

    /* 等待线程被唤醒 */
    pthread_join(thr, NULL);

    CHECK(ctx.ready == 1, "recvfrom should succeed after data");
    CHECK(ctx.len == 8, "recvfrom length mismatch");
    {
        const char *expect = "BLOCKING";
        int j;
        for (j = 0; j < 8; j++) {
            if (ctx.buf[j] != (uint8_t)expect[j]) {
                FAIL("payload mismatch");
                goto done;
            }
        }
    }
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 测试 2：recvfrom 超时
 * ================================================================ */

static void test_recvfrom_timeout(void)
{
    int fd;
    struct sockaddr_in addr;
    pthread_t thr;
    recv_thread_ctx_t ctx;


    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr, "0.0.0.0", 5000);
    tiny_udp_bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    /* 设置 200ms 超时 */
    {
        struct timeval tv = { 0, 200000 };  /* 200ms */
        tiny_udp_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    TEST("recvfrom timeout after 200ms");
    ctx.fd = fd;
    ctx.ready = -2;
    pthread_create(&thr, NULL, recv_thread_func, &ctx);

    /* 等待超时 */
    pthread_join(thr, NULL);

    CHECK(ctx.ready == -1, "recvfrom should fail on timeout");
    CHECK(ctx.saved_errno == EAGAIN, "errno should be EAGAIN");
    PASS();

    tiny_udp_close(fd);
    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 测试 3：select 超时
 * ================================================================ */

static void test_select_timeout(void)
{
    int fd;
    struct sockaddr_in addr;
    fd_set readset;
    struct timeval tv;
    int ret;


    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr, "0.0.0.0", 6000);
    tiny_udp_bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    TEST("select with 100ms timeout returns 0");
    FD_ZERO(&readset);
    FD_SET(fd, &readset);
    tv.tv_sec  = 0;
    tv.tv_usec = 100000;  /* 100ms */
    ret = tiny_udp_select(fd + 1, &readset, NULL, NULL, &tv);
    CHECK(ret == 0, "select should timeout (return 0)");
    CHECK(!FD_ISSET(fd, &readset), "fd should not be readable after timeout");
    PASS();

    tiny_udp_close(fd);
    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 测试 4：线程安全 — 多线程并发
 * ================================================================ */

#define CONCURRENT_COUNT  20

typedef struct {
    int     fd_send;
    int     fd_recv;
    int     send_count;
    int     recv_count;
    uint8_t recv_map[CONCURRENT_COUNT]; /* 0=not received, 1=received */
} concurrent_ctx_t;

static void *sender_thread(void *arg)
{
    concurrent_ctx_t *ctx = (concurrent_ctx_t *)arg;
    struct sockaddr_in to;
    make_addr(&to, "127.0.0.1", 7001);
    {
        int i;
        for (i = 0; i < CONCURRENT_COUNT; i++) {
            uint8_t data[4];
            data[0] = (uint8_t)(i >> 24);
            data[1] = (uint8_t)(i >> 16);
            data[2] = (uint8_t)(i >> 8);
            data[3] = (uint8_t)(i);
            tiny_udp_sendto(ctx->fd_send, data, 4, 0,
                            (struct sockaddr *)&to, sizeof(to));
        }
    }
    ctx->send_count = CONCURRENT_COUNT;
    return NULL;
}

static void *receiver_thread(void *arg)
{
    concurrent_ctx_t *ctx = (concurrent_ctx_t *)arg;
    int count = 0;
    while (count < CONCURRENT_COUNT) {
        uint8_t buf[256];
        int ret = tiny_udp_recvfrom(ctx->fd_recv, buf, sizeof(buf), 0,
                                    NULL, NULL);
        if (ret < 0) {
            /* 可能超时，继续等待 */
            usleep(1000);
            continue;
        }
        if (ret == 4) {
            uint32_t seq = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
            if (seq < CONCURRENT_COUNT && ctx->recv_map[seq] == 0) {
                ctx->recv_map[seq] = 1;
                count++;
            }
        }
    }
    ctx->recv_count = count;
    return NULL;
}

static void *tick_thread(void *arg)
{
    (void)arg;
    {
        int i;
        for (i = 0; i < 100; i++) {
            loopback_all();
            tiny_udp_tick();
            usleep(1000);  /* 1ms */
        }
    }
    return NULL;
}

static void test_concurrent(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    pthread_t s_thr, r_thr, t_thr;
    concurrent_ctx_t ctx;


    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr_a, "0.0.0.0", 7000);
    make_addr(&addr_b, "0.0.0.0", 7001);
    tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));

    /* 设置接收方 500ms 超时 */
    {
        struct timeval tv = { 0, 500000 };
        tiny_udp_setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ctx.fd_send = fd_a;
    ctx.fd_recv = fd_b;
    ctx.send_count = 0;
    ctx.recv_count = 0;
    { int zi; for (zi = 0; zi < CONCURRENT_COUNT; zi++) ctx.recv_map[zi] = 0; }

    TEST("concurrent send/recv/tick (20 packets)");
    pthread_create(&t_thr, NULL, tick_thread, NULL);
    pthread_create(&s_thr, NULL, sender_thread, &ctx);
    pthread_create(&r_thr, NULL, receiver_thread, &ctx);

    pthread_join(s_thr, NULL);
    pthread_join(r_thr, NULL);
    /* 额外做几次 tick 确保所有包都被处理 */
    {
        int i;
        for (i = 0; i < 10; i++) {
            loopback_all();
            tiny_udp_tick();
            usleep(2000);
        }
    }
    pthread_join(t_thr, NULL);

    CHECK(ctx.send_count == CONCURRENT_COUNT, "not all packets sent");
    CHECK(ctx.recv_count == CONCURRENT_COUNT, "not all packets received");
    {
        int si;
        for (si = 0; si < CONCURRENT_COUNT; si++) {
            if (ctx.recv_map[si] == 0) {
                FAIL("missing or duplicate sequence number");
                goto done;
            }
        }
    }
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 测试 5：close 唤醒阻塞的 recvfrom
 * ================================================================ */

static void *close_wake_thread(void *arg)
{
    recv_thread_ctx_t *ctx = (recv_thread_ctx_t *)arg;
    ctx->len = tiny_udp_recvfrom(ctx->fd, ctx->buf, sizeof(ctx->buf), 0,
                                 NULL, NULL);
    ctx->saved_errno = errno;
    ctx->ready = (ctx->len >= 0) ? 1 : -1;
    return NULL;
}

static void test_close_wakes_blocked_recv(void)
{
    int fd;
    struct sockaddr_in addr;
    pthread_t thr;
    recv_thread_ctx_t ctx;


    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr, "0.0.0.0", 8000);
    tiny_udp_bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    TEST("close() wakes blocked recvfrom");
    ctx.fd = fd;
    ctx.ready = -2;
    pthread_create(&thr, NULL, close_wake_thread, &ctx);

    /* 等待线程进入阻塞 */
    usleep(50000);

    /* 此时线程应该阻塞着 */
    CHECK(ctx.ready == -2, "thread should be waiting");

    /* close 会 signal semaphore，唤醒阻塞线程 */
    tiny_udp_close(fd);

    pthread_join(thr, NULL);

    /* recvfrom 应该返回错误（socket 被关闭了） */
    CHECK(ctx.ready == -1, "recvfrom should fail after close");
    PASS();

    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 主函数
 * ================================================================ */

int main(void)
{
    setbuf(stdout, NULL);  /* unbuffered for debugging */
    signal(SIGALRM, alarm_handler);

    /* 全局一次性初始化（不可重复调用，否则 pthread mutex/cond 行为未定义） */
    tiny_udp_init(ip("127.0.0.1"));

    printf("=== RTOS Blocking & Thread Safety Test Suite ===\n\n");
    fflush(stdout);

    printf("[1] Blocking recvfrom → woke by data\n");
    fflush(stdout);
    alarm(3);
    test_blocking_recvfrom();
    alarm(0);

    printf("\n[2] recvfrom SO_RCVTIMEO timeout\n");
    fflush(stdout);
    alarm(5);
    test_recvfrom_timeout();
    alarm(0);

    printf("\n[3] select timeout\n");
    fflush(stdout);
    alarm(5);
    test_select_timeout();
    alarm(0);

    printf("\n[4] Thread safety — concurrent send/recv/tick\n");
    fflush(stdout);
    alarm(10);
    test_concurrent();
    alarm(0);

    printf("\n[5] close() wakes blocked recvfrom\n");
    fflush(stdout);
    alarm(5);
    test_close_wakes_blocked_recv();
    alarm(0);

    printf("\n========================================\n");
    printf("RESULTS: %d passed, %d failed\n", test_pass, test_fail);

    return test_fail > 0 ? 1 : 0;
}
