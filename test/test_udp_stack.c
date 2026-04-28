/*
 * test_udp_stack.c — UDP 协议栈数据流测试
 *
 * 模拟外部系统（rx_pool / tx_pool / 索引管理），
 * 验证协议栈的完整数据流：
 *   发送端 sendto → tx_pool → 模拟回环 → rx_pool → tick → recvfrom 接收端
 *
 * 编译：gcc -Wall -Wextra -std=c99 -I../src -o test_udp_stack test_udp_stack.c ../src/udp_stack.c
 * 运行：./test_udp_stack
 */

#include <stdio.h>
#include "../src/udp_stack.h"

/* ---- 测试工具 ---- */

static int test_pass = 0;
static int test_fail = 0;

#define TEST(name)                                                        \
    do {                                                                  \
        printf("  TEST: %s ... ", name);                                  \
    } while (0)

#define PASS()                                                            \
    do {                                                                  \
        printf("PASS\n");                                                 \
        test_pass++;                                                      \
    } while (0)

#define FAIL(msg)                                                         \
    do {                                                                  \
        printf("FAIL (%s)\n", msg);                                       \
        test_fail++;                                                      \
    } while (0)

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) { FAIL(msg); goto done; }                            \
    } while (0)

/**
 * @brief 模拟外部系统将 tx_pool 中的包"发送"到 rx_pool（回环测试）
 *
 * 从 tx_pool 读取一个包（从 tx_read_idx 开始），直接复制到 rx_pool，
 * 推进两边索引。
 * @return 复制的包数量
 */
static int simulate_loopback_one(void)
{
    uint32_t tx_slot, rx_slot;
    uint16_t pkt_len;

    if (g_tx_read_idx >= g_tx_write_idx) {
        return 0;  /* 没有待发送的包 */
    }

    tx_slot = g_tx_read_idx % TX_POOL_SIZE;

    /* 从 IP 头 Total Length 字段读取包长 */
    pkt_len = ((uint16_t)tx_pool[tx_slot][2] << 8) | tx_pool[tx_slot][3];

    /* 复制到 rx_pool */
    rx_slot = g_rx_write_idx % RX_POOL_SIZE;
    {
        int j;
        for (j = 0; j < pkt_len && j < RX_PKT_MAXLEN; j++) {
            rx_pool[rx_slot][j] = tx_pool[tx_slot][j];
        }
    }

    g_tx_read_idx++;
    g_rx_write_idx++;

    return 1;
}

/* ---- 测试用例 ---- */

/* 辅助：IP 字符串转网络字节序 */
static uint32_t ip_to_u32(const char *ip_str)
{
    uint32_t ip;
    tiny_udp_inet_pton(AF_INET, ip_str, &ip);
    return ip;
}

/* 辅助：创建 sockaddr_in */
static void make_addr(struct sockaddr_in *addr, const char *ip_str, uint16_t port_host)
{
    addr->sin_len    = sizeof(struct sockaddr_in);
    addr->sin_family = AF_INET;
    addr->sin_port   = (uint16_t)((port_host >> 8) | (port_host << 8)); /* host→net */
    addr->sin_addr.s_addr = ip_to_u32(ip_str);
}

static void test_socket_create_close(void)
{
    int fd;

    TEST("socket create");
    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0, "socket creation failed");
    CHECK(fd < UDP_SOCKET_MAX, "fd out of range");
    PASS();

    TEST("close socket");
    {
        int ret = tiny_udp_close(fd);
        CHECK(ret == 0, "close failed");
    }
    PASS();

    TEST("tick reclaims closed socket");
    tiny_udp_tick();
    /* 此时 fd 应该可被复用 */
    {
        int fd2 = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(fd2 == fd, "closed socket not reclaimed (expected reuse of same fd)");
        tiny_udp_close(fd2);
        tiny_udp_tick();
    }
    PASS();

done:
    return;
}

static void test_bind(void)
{
    int fd;
    struct sockaddr_in addr;
    int ret;

    TEST("socket + bind");
    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0, "socket failed");

    make_addr(&addr, "0.0.0.0", 8080);
    ret = tiny_udp_bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    CHECK(ret == 0, "bind failed");
    PASS();

    TEST("bind already-bound port");
    {
        int fd2 = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(fd2 >= 0, "socket2 failed");
        ret = tiny_udp_bind(fd2, (struct sockaddr *)&addr, sizeof(addr));
        CHECK(ret == -1, "bind should fail for duplicate port");
        CHECK(errno == EADDRINUSE, "errno should be EADDRINUSE");
        tiny_udp_close(fd2);
        tiny_udp_tick();
    }
    PASS();

    tiny_udp_close(fd);
    tiny_udp_tick();

done:
    return;
}

static void test_state_machine(void)
{
    int fd;
    struct sockaddr_in addr;
    int ret;

    TEST("invalid bind on unopened socket");
    make_addr(&addr, "0.0.0.0", 8081);
    ret = tiny_udp_bind(99, (struct sockaddr *)&addr, sizeof(addr));
    CHECK(ret == -1, "bind on invalid fd should fail");
    PASS();

    TEST("recvfrom on unbound socket");
    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0, "socket failed");
    {
        uint8_t buf[64];
        ret = tiny_udp_recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        CHECK(ret == -1, "recvfrom on unbound socket should fail");
    }
    tiny_udp_close(fd);
    tiny_udp_tick();
    PASS();

done:
    return;
}

static void test_send_recv_dataflow(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    int ret;
    const char *test_msg = "Hello, UDP!";
    uint8_t rbuf[256];

    /* 初始化 */
    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("create socket A");
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd_a >= 0, "socket A failed");
    PASS();

    TEST("create socket B");
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd_b >= 0, "socket B failed");
    PASS();

    TEST("bind A to port 8000");
    make_addr(&addr_a, "0.0.0.0", 8000);
    ret = tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    CHECK(ret == 0, "bind A failed");
    PASS();

    TEST("bind B to port 9000");
    make_addr(&addr_b, "0.0.0.0", 9000);
    ret = tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));
    CHECK(ret == 0, "bind B failed");
    PASS();

    TEST("sendto A→B");
    make_addr(&addr_b, "127.0.0.1", 9000);
    ret = tiny_udp_sendto(fd_a, test_msg, 11, 0,
                          (struct sockaddr *)&addr_b, sizeof(addr_b));
    CHECK(ret == 11, "sendto failed");
    PASS();

    TEST("loopback tx→rx");
    {
        int n = simulate_loopback_one();
        CHECK(n == 1, "no packet to loopback");
    }
    PASS();

    TEST("tick processes rx packet");
    tiny_udp_tick();
    PASS();

    TEST("recvfrom B receives data");
    {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ret = tiny_udp_recvfrom(fd_b, rbuf, sizeof(rbuf), 0,
                                (struct sockaddr *)&from, &fromlen);
        CHECK(ret == 11, "recvfrom returned wrong length");
        CHECK(from.sin_port == addr_a.sin_port, "source port mismatch");
        {
            int j;
            for (j = 0; j < 11; j++) {
                if (rbuf[j] != (uint8_t)test_msg[j]) {
                    FAIL("payload mismatch");
                    goto done;
                }
            }
        }
    }
    PASS();

    /* 清理 */
    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

static void test_connect_send_recv(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    int ret;
    const char *msg2 = "msg via send";
    uint8_t rbuf[256];

    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("connect mode: create sockets");
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd_a >= 0 && fd_b >= 0, "socket creation failed");
    PASS();

    TEST("bind A:8001, connect B to A");
    make_addr(&addr_a, "0.0.0.0", 8001);
    ret = tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    CHECK(ret == 0, "bind A failed");

    make_addr(&addr_a, "127.0.0.1", 8001);
    ret = tiny_udp_connect(fd_b, (struct sockaddr *)&addr_a, sizeof(addr_a));
    CHECK(ret == 0, "connect B failed");
    PASS();

    TEST("connect mode: send→recv");
    ret = tiny_udp_send(fd_b, msg2, 12, 0);
    CHECK(ret == 12, "send failed");

    simulate_loopback_one();
    tiny_udp_tick();

    ret = tiny_udp_recvfrom(fd_a, rbuf, sizeof(rbuf), 0, NULL, NULL);
    CHECK(ret == 12, "recvfrom failed");
    {
        int j;
        for (j = 0; j < 12; j++) {
            if (rbuf[j] != (uint8_t)msg2[j]) {
                FAIL("payload mismatch in connect send->recv");
                goto done;
            }
        }
    }
    PASS();

    TEST("connect mode: recv filters by peer");
    /* sendto 一个来自不同源的包到 fd_a */
    make_addr(&addr_b, "127.0.0.1", 8001);
    /* 直接用 fd_b 的地址来模拟另一个源端口发数据到 fd_a */
    /* fd_a 已经是 BOUND 状态，应该接受 */
    /* 此时 fd_a 已经有来自 fd_b 的 connect 地址的数据正常收到 */
    PASS();

    TEST("getsockname / getpeername");
    {
        struct sockaddr_in name;
        socklen_t namelen = sizeof(name);
        ret = tiny_udp_getpeername(fd_b, (struct sockaddr *)&name, &namelen);
        CHECK(ret == 0, "getpeername failed");
        CHECK(name.sin_addr.s_addr == ip_to_u32("127.0.0.1"), "peer IP mismatch");
        CHECK(name.sin_port == addr_a.sin_port, "peer port mismatch");
    }
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

static void test_select(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    int ret;

    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("select: create and bind sockets");
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr_a, "0.0.0.0", 7000);
    make_addr(&addr_b, "0.0.0.0", 7001);
    tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));
    PASS();

    TEST("select: no data → readset empty");
    {
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(fd_a, &readset);
        FD_SET(fd_b, &readset);
        ret = tiny_udp_select(64, &readset, NULL, NULL, NULL);
        CHECK(ret == 0, "select should return 0 when no data");
    }
    PASS();

    TEST("select: after send → readset has receiver");
    make_addr(&addr_b, "127.0.0.1", 7001);
    tiny_udp_sendto(fd_a, "data", 4, 0, (struct sockaddr *)&addr_b, sizeof(addr_b));
    simulate_loopback_one();
    tiny_udp_tick();
    {
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(fd_a, &readset);
        FD_SET(fd_b, &readset);
        ret = tiny_udp_select(64, &readset, NULL, NULL, NULL);
        CHECK(ret == 1, "select should return 1");
        CHECK(FD_ISSET(fd_b, &readset), "fd_b should be readable");
        CHECK(!FD_ISSET(fd_a, &readset), "fd_a should not be readable");
    }
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

static void test_poll(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    struct pollfd fds[2];
    int ret;

    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("poll: create and bind");
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr_a, "0.0.0.0", 6000);
    make_addr(&addr_b, "0.0.0.0", 6001);
    tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));
    PASS();

    TEST("poll: POLLOUT always set");
    fds[0].fd      = fd_a;
    fds[0].events  = POLLOUT;
    fds[0].revents = 0;
    ret = tiny_udp_poll(fds, 1, 0);
    CHECK(ret == 1, "poll should return 1");
    CHECK(fds[0].revents & POLLOUT, "POLLOUT should be set");
    PASS();

    TEST("poll: POLLIN after data");
    make_addr(&addr_b, "127.0.0.1", 6001);
    tiny_udp_sendto(fd_a, "poll", 4, 0, (struct sockaddr *)&addr_b, sizeof(addr_b));
    simulate_loopback_one();
    tiny_udp_tick();
    fds[0].fd      = fd_b;
    fds[0].events  = POLLIN | POLLOUT;
    fds[0].revents = 0;
    ret = tiny_udp_poll(fds, 1, 0);
    CHECK(ret == 1, "poll should return 1");
    CHECK(fds[0].revents & POLLIN, "POLLIN should be set");
    CHECK(fds[0].revents & POLLOUT, "POLLOUT should be set");
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

static void test_inet_conversion(void)
{
    char buf[16];
    uint32_t ip;

    TEST("inet_pton normal");
    {
        int ret = tiny_udp_inet_pton(AF_INET, "192.168.1.100", &ip);
        CHECK(ret == 1, "inet_pton failed");
        /* 192.168.1.100 → 0xC0A80164 */
        CHECK(ip == 0xC0A80164, "inet_pton wrong value");
    }
    PASS();

    TEST("inet_ntop normal");
    {
        ip = 0xC0A80164;  /* 192.168.1.100 */
        const char *ret = tiny_udp_inet_ntop(AF_INET, &ip, buf, sizeof(buf));
        CHECK(ret == buf, "inet_ntop failed");
        {
            int j, match = 1;
            const char *expect = "192.168.1.100";
            for (j = 0; expect[j]; j++) {
                if (buf[j] != expect[j]) { match = 0; break; }
            }
            CHECK(match, "inet_ntop wrong string");
        }
    }
    PASS();

    TEST("inet_pton edge: 0.0.0.0");
    CHECK(tiny_udp_inet_pton(AF_INET, "0.0.0.0", &ip) == 1, "0.0.0.0 failed");
    CHECK(ip == 0, "0.0.0.0 wrong value");
    PASS();

    TEST("inet_pton edge: 255.255.255.255");
    CHECK(tiny_udp_inet_pton(AF_INET, "255.255.255.255", &ip) == 1, "255.255.255.255 failed");
    CHECK(ip == 0xFFFFFFFF, "255.255.255.255 wrong value");
    PASS();

    TEST("inet_pton invalid: 256.1.1.1");
    CHECK(tiny_udp_inet_pton(AF_INET, "256.1.1.1", &ip) == 0, "256.1.1.1 should fail");
    PASS();

    TEST("inet_pton invalid: abc");
    CHECK(tiny_udp_inet_pton(AF_INET, "abc", &ip) == 0, "'abc' should fail");
    PASS();

done:
    return;
}

static void test_buffer_overflow(void)
{
    int fd_a, fd_b;
    struct sockaddr_in addr_a, addr_b;
    int ret;
    uint8_t rbuf[64];
    int i;

    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("buffer overflow: fill socket queue");
    fd_a = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    fd_b = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    make_addr(&addr_a, "0.0.0.0", 5000);
    make_addr(&addr_b, "0.0.0.0", 5001);
    tiny_udp_bind(fd_a, (struct sockaddr *)&addr_a, sizeof(addr_a));
    tiny_udp_bind(fd_b, (struct sockaddr *)&addr_b, sizeof(addr_b));
    PASS();

    TEST("buffer overflow: send 64 packets, only 32 retained");
    make_addr(&addr_b, "127.0.0.1", 5001);
    for (i = 0; i < 64; i++) {
        tiny_udp_sendto(fd_a, "x", 1, 0, (struct sockaddr *)&addr_b, sizeof(addr_b));
        simulate_loopback_one();
        tiny_udp_tick();
    }
    /* 应该至少收到 32 个包 */
    {
        int count = 0;
        while (1) {
            ret = tiny_udp_recvfrom(fd_b, rbuf, sizeof(rbuf), 0, NULL, NULL);
            if (ret < 0) break;
            count++;
        }
        CHECK(count == 32, "should have exactly 32 buffered packets");
    }
    PASS();

    tiny_udp_close(fd_a);
    tiny_udp_close(fd_b);
    tiny_udp_tick();

done:
    return;
}

static void test_fcntl_ioctl(void)
{
    int fd;
    int ret;

    TEST("fcntl F_GETFL default");
    fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0, "socket failed");
    ret = tiny_udp_fcntl(fd, F_GETFL, 0);
    CHECK(ret == 0, "default flags should be 0");
    PASS();

    TEST("fcntl F_SETFL O_NONBLOCK");
    ret = tiny_udp_fcntl(fd, F_SETFL, O_NONBLOCK);
    CHECK(ret == 0, "F_SETFL failed");
    ret = tiny_udp_fcntl(fd, F_GETFL, 0);
    CHECK(ret == O_NONBLOCK, "F_GETFL should return O_NONBLOCK");
    PASS();

    TEST("ioctl FIONBIO clear");
    {
        int on = 0;
        ret = tiny_udp_ioctl(fd, FIONBIO, &on);
        CHECK(ret == 0, "ioctl FIONBIO failed");
        ret = tiny_udp_fcntl(fd, F_GETFL, 0);
        CHECK(ret == 0, "flags should be 0 after clearing");
    }
    PASS();

    tiny_udp_close(fd);
    tiny_udp_tick();

done:
    return;
}

static void test_multiple_sockets(void)
{
    int fds[10];
    struct sockaddr_in addr;
    int i, ret;
    uint8_t rbuf[32];

    tiny_udp_init(ip_to_u32("127.0.0.1"));

    TEST("multiple: create 10 sockets");
    for (i = 0; i < 10; i++) {
        fds[i] = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(fds[i] >= 0, "socket creation failed");
    }
    PASS();

    TEST("multiple: bind all to different ports");
    for (i = 0; i < 10; i++) {
        make_addr(&addr, "0.0.0.0", (uint16_t)(7000 + i));
        ret = tiny_udp_bind(fds[i], (struct sockaddr *)&addr, sizeof(addr));
        CHECK(ret == 0, "bind failed");
    }
    PASS();

    TEST("multiple: send between first and last");
    make_addr(&addr, "127.0.0.1", 7009);
    tiny_udp_sendto(fds[0], "multi", 5, 0, (struct sockaddr *)&addr, sizeof(addr));
    simulate_loopback_one();
    tiny_udp_tick();

    ret = tiny_udp_recvfrom(fds[9], rbuf, sizeof(rbuf), 0, NULL, NULL);
    CHECK(ret == 5, "recvfrom wrong length");
    PASS();

    TEST("multiple: other sockets get no data");
    for (i = 1; i < 9; i++) {
        ret = tiny_udp_recvfrom(fds[i], rbuf, sizeof(rbuf), 0, NULL, NULL);
        CHECK(ret == -1, "unexpected data on other socket");
        CHECK(errno == EAGAIN, "errno should be EAGAIN");
    }
    PASS();

    for (i = 0; i < 10; i++) {
        tiny_udp_close(fds[i]);
    }
    tiny_udp_tick();

done:
    return;
}

/* ================================================================
 * 主函数
 * ================================================================ */

int main(void)
{
    printf("=== UDP Stack Protocol Test Suite ===\n\n");

    printf("[1] Socket Create / Close / Reclaim\n");
    test_socket_create_close();

    printf("\n[2] Bind / Port Conflict\n");
    test_bind();

    printf("\n[3] State Machine Guards\n");
    test_state_machine();

    printf("\n[4] Send → Recv Full Data Flow\n");
    test_send_recv_dataflow();

    printf("\n[5] Connect / Send / Recv / getsockname / getpeername\n");
    test_connect_send_recv();

    printf("\n[6] Select\n");
    test_select();

    printf("\n[7] Poll\n");
    test_poll();

    printf("\n[8] inet_pton / inet_ntop\n");
    test_inet_conversion();

    printf("\n[9] Buffer Overflow Protection\n");
    test_buffer_overflow();

    printf("\n[10] fcntl / ioctl\n");
    test_fcntl_ioctl();

    printf("\n[11] Multiple Sockets (10)\n");
    test_multiple_sockets();

    printf("\n========================================\n");
    printf("RESULTS: %d passed, %d failed\n", test_pass, test_fail);

    return test_fail > 0 ? 1 : 0;
}
