/*
 * test_wolfssl_dtls_baremetal.c — wolfSSL DTLS over Tiny UDP Socket 裸机模拟
 *
 * 使用 wolfSSL 默认 I/O (EmbedSendTo/EmbedReceiveFrom)，
 * 通过链接器级别包装函数将 socket 调用重定向到 tiny_udp_* API。
 *
 * 架构:
 *   进程A (Server, IP=10.0.0.1)         进程B (Client, IP=10.0.0.2)
 *   ┌────────────────────────┐          ┌────────────────────────┐
 *   │ wolfSSL DTLS server    │          │ wolfSSL DTLS client    │
 *   │   ↓ EmbedSendTo/RecvFrom│          │   ↓ EmbedSendTo/RecvFrom│
 *   │   ↓ sendto/recvfrom    │          │   ↓ sendto/recvfrom    │
 *   │   ↓ [linker wrapper]   │          │   ↓ [linker wrapper]   │
 *   │ tiny_udp_sendto/recvfrom│          │ tiny_udp_sendto/recvfrom│
 *   │   ↓ tx_pool    ↑ rx_pool│          │   ↓ tx_pool    ↑ rx_pool│
 *   │   ↓ relay      ↑ relay │          │   ↓ relay      ↑ relay │
 *   └────────┬───────────────┘          └────────┬───────────────┘
 *            │         共享内存 (mmap)           │
 *            │  channel[0]: Server→Client        │
 *            │  channel[1]: Client→Server        │
 *            └──────────────────────────────────┘
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

/* ---- 裸机 errno：在系统头文件之后，udp_stack.h 之前覆盖 ----
   系统头文件（如 <sys/errno.h>）会将 errno 定义为 (*__error()) 宏。
   裸机模拟中需统一使用 int errno 全局变量，保证与 wolfSSL（WOLFSSL_NO_SOCK 编译）一致。 */
#ifdef errno
#undef errno
#endif
extern int errno;

/* ---- Tiny UDP Stack (纯裸机类型，无系统依赖) ---- */
#include "../src/udp_stack.h"

/* ---- wolfSSL (裸机编译：WOLFSSL_NO_SOCK + Embed* I/O) ---- */
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

/* ================================================================
 * 链接器级别 socket 包装函数
 *
 * wolfSSL 的 EmbedSendTo/EmbedReceiveFrom 调用标准 socket 函数。
 * 以下包装函数在链接时覆盖系统符号，将调用重定向到 tiny_udp_* API。
 * ================================================================ */

ssize_t sendto(int s, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen)
{
    ssize_t ret = (ssize_t)tiny_udp_sendto(s, buf, len, flags, to, tolen);
    fprintf(stderr, "  [WRAP] sendto(fd=%d, len=%zu) = %zd (errno=%d)\n", s, len, ret, ret < 0 ? errno : 0);
    return ret;
}

ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t ret = (ssize_t)tiny_udp_recvfrom(s, buf, len, flags, from, fromlen);
    fprintf(stderr, "  [WRAP] recvfrom(fd=%d, len=%zu) = %zd (errno=%d)\n", s, len, ret, ret < 0 ? errno : 0);
    return ret;
}

int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    int ret = tiny_udp_getsockopt(s, level, optname, optval, optlen);
    fprintf(stderr, "  [WRAP] getsockopt(fd=%d, level=%d, optname=%d) = %d\n", s, level, optname, ret);
    return ret;
}

int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    int ret = tiny_udp_setsockopt(s, level, optname, optval, optlen);
    fprintf(stderr, "  [WRAP] setsockopt(fd=%d, level=%d, optname=%d) = %d\n", s, level, optname, ret);
    return ret;
}

int getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    int ret = tiny_udp_getpeername(s, name, namelen);
    fprintf(stderr, "  [WRAP] getpeername(fd=%d) = %d\n", s, ret);
    return ret;
}

/* ================================================================
 * 辅助：将文件读入内存缓冲区（仅测试使用）
 * ================================================================ */

static int read_file_to_buffer(const char *path,
                               unsigned char **out_buf, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    long len;
    unsigned char *buf;

    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > (1024 * 1024)) { fclose(fp); return -1; }

    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(fp); return -1; }

    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);

    *out_buf = buf;
    *out_len = (size_t)len;
    return 0;
}

/* ================================================================
 * 共享内存布局（模拟网络线路）
 * ================================================================ */

#define CHANNEL_SLOTS   128

struct shm_channel {
    uint8_t  buf[CHANNEL_SLOTS][UDP_PKT_BUF_SIZE];
    uint16_t len[CHANNEL_SLOTS];
    volatile uint32_t write_idx;  /* 发送方写入后递增 */
    volatile uint32_t read_idx;   /* 接收方读取后递增 */
};

struct udp_shm {
    struct shm_channel s2c;  /* Server → Client */
    struct shm_channel c2s;  /* Client → Server */
    volatile int server_ready;
    volatile int client_ready;
    volatile int running;
};

static int shm_channel_write(struct shm_channel *ch,
                             const uint8_t *pkt, uint16_t pkt_len)
{
    uint32_t w = ch->write_idx;
    uint32_t r = ch->read_idx;

    if (w - r >= CHANNEL_SLOTS) return -1;

    uint32_t slot = w % CHANNEL_SLOTS;
    memcpy(ch->buf[slot], pkt, pkt_len);
    ch->len[slot] = pkt_len;
    __sync_synchronize();
    ch->write_idx = w + 1;
    return 0;
}

static int shm_channel_read(struct shm_channel *ch,
                            uint8_t *pkt, uint16_t *pkt_len)
{
    uint32_t r = ch->read_idx;
    uint32_t w = ch->write_idx;

    if (r >= w) return -1;

    uint32_t slot = r % CHANNEL_SLOTS;
    *pkt_len = ch->len[slot];
    memcpy(pkt, ch->buf[slot], *pkt_len);
    __sync_synchronize();
    ch->read_idx = r + 1;
    return 0;
}

/* ================================================================
 * Relay: 把 tx_pool 发出去，把对面发来的数据放进 rx_pool
 * ================================================================ */

static struct udp_shm *g_shm = NULL;
static int g_is_server = 0;

static void relay_drain_tx(void)
{
    struct shm_channel *ch = g_is_server ? &g_shm->s2c : &g_shm->c2s;

    while (g_tx_read_idx != g_tx_write_idx) {
        uint32_t slot = g_tx_read_idx % TX_POOL_SIZE;
        uint16_t pkt_len = (uint16_t)tx_pool[slot][2] << 8
                         | (uint16_t)tx_pool[slot][3];
        if (pkt_len < 20 || pkt_len > UDP_PKT_BUF_SIZE) {
            g_tx_read_idx++;
            continue;
        }

        if (shm_channel_write(ch, tx_pool[slot], pkt_len) == 0) {
            g_tx_read_idx++;
        } else {
            break;
        }
    }
}

static void relay_fill_rx(void)
{
    struct shm_channel *ch = g_is_server ? &g_shm->c2s : &g_shm->s2c;

    while (1) {
        uint32_t w = g_rx_write_idx % RX_POOL_SIZE;
        uint16_t pkt_len;

        if (shm_channel_read(ch, rx_pool[w], &pkt_len) < 0)
            break;

        g_rx_write_idx++;
    }
}

static void relay_cycle(void)
{
    relay_drain_tx();
    relay_fill_rx();
    tiny_udp_tick();
}

/* ================================================================
 * DTLS 握手循环（非阻塞 + relay 交替）
 * ================================================================ */

static int dtls_handshake(WOLFSSL *ssl, int is_server)
{
    int ret;
    int max_rounds = 200;

    while (max_rounds-- > 0) {
        ret = is_server ? wolfSSL_accept(ssl) : wolfSSL_connect(ssl);

        if (ret == WOLFSSL_SUCCESS)
            return 0;

        int err = wolfSSL_get_error(ssl, ret);

        if (err == WOLFSSL_ERROR_WANT_READ ||
            err == WOLFSSL_ERROR_WANT_WRITE) {
            relay_cycle();
            usleep(1000);
            continue;
        }

        fprintf(stderr, "[%s] DTLS error: ret=%d err=%d (%s)\n",
                is_server ? "Server" : "Client", ret, err,
                wolfSSL_ERR_reason_error_string(err));
        return -1;
    }

    fprintf(stderr, "[%s] Handshake timeout\n",
            is_server ? "Server" : "Client");
    return -1;
}

/* ================================================================
 * Server 进程
 * ================================================================ */

static int run_server(struct udp_shm *shm)
{
    int server_fd;
    struct sockaddr_in local_addr;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char buf[256];
    int ret, ok = 0;
    const char *echo_msg = "Echo from tiny_udp DTLS server!";

    g_shm      = shm;
    g_is_server = 1;

    /* 1. 初始化协议栈 (IP=10.0.0.1) */
    tiny_udp_init(htonl(0x0A000001));

    /* 2. 创建 UDP socket */
    server_fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_fd < 0) {
        fprintf(stderr, "[Server] socket failed\n");
        return 1;
    }

    /* 3. 绑定端口 8443 */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family      = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port        = htons(8443);

    if (tiny_udp_bind(server_fd, (struct sockaddr *)&local_addr,
                      sizeof(local_addr)) < 0) {
        fprintf(stderr, "[Server] bind failed\n");
        return 1;
    }
    printf("[Server] Bound to 10.0.0.1:8443 (fd=%d)\n", server_fd);

    /* 4. 设置非阻塞 */
    int nb = 1;
    tiny_udp_ioctl(server_fd, FIONBIO, &nb);

    /* 5. wolfSSL DTLS 初始化 */
    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (!ctx) { fprintf(stderr, "[Server] CTX_new failed\n"); goto done; }

    {
        unsigned char *cert_buf = NULL, *key_buf = NULL;
        size_t cert_len = 0, key_len = 0;

        if (read_file_to_buffer("server-cert.pem", &cert_buf, &cert_len) < 0) {
            fprintf(stderr, "[Server] cert file read failed\n"); goto done;
        }
        if (read_file_to_buffer("server-key.pem", &key_buf, &key_len) < 0) {
            fprintf(stderr, "[Server] key file read failed\n");
            free(cert_buf); goto done;
        }

        if (wolfSSL_CTX_use_certificate_chain_buffer_format(ctx,
                cert_buf, (int)cert_len, WOLFSSL_FILETYPE_PEM)
            != WOLFSSL_SUCCESS) {
            fprintf(stderr, "[Server] cert load failed\n");
            free(cert_buf); free(key_buf); goto done;
        }
        if (wolfSSL_CTX_use_PrivateKey_buffer(ctx, key_buf, (int)key_len,
                WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
            fprintf(stderr, "[Server] key load failed\n");
            free(cert_buf); free(key_buf); goto done;
        }
        free(cert_buf);
        free(key_buf);
    }

    ssl = wolfSSL_new(ctx);
    if (!ssl) { fprintf(stderr, "[Server] SSL_new failed\n"); goto done; }

    /* 6. 设置 socket fd (Server 端不设 dtls_set_peer，wolfSSL 自动从首包学习) */
    wolfSSL_set_fd(ssl, server_fd);
    wolfSSL_dtls_set_using_nonblock(ssl, 1);

    shm->server_ready = 1;

    /* 7. DTLS 握手（非阻塞 + relay） */
    printf("[Server] Waiting for DTLS handshake...\n");
    if (dtls_handshake(ssl, 1) < 0) goto done;
    printf("[Server] DTLS handshake OK\n");

    /* 8. 收消息（relay 循环） */
    {
        int rounds = 100;
        memset(buf, 0, sizeof(buf));
        ret = -1;
        while (rounds-- > 0) {
            ret = wolfSSL_read(ssl, buf, sizeof(buf) - 1);
            if (ret > 0) break;
            int err = wolfSSL_get_error(ssl, ret);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                relay_cycle();
                usleep(1000);
                continue;
            }
            break;
        }
        if (ret > 0) {
            printf("[Server] Received (%d bytes): \"%s\"\n", ret, buf);

            /* 9. 回显 */
            ret = wolfSSL_write(ssl, echo_msg, (int)strlen(echo_msg));
            if (ret > 0) {
                int r2 = 50;
                while (r2-- > 0) {
                    relay_cycle();
                    usleep(1000);
                }
                printf("[Server] Echo sent: %d bytes\n", ret);
                ok = 1;
            }
        } else {
            fprintf(stderr, "[Server] read failed: %d\n", ret);
        }
    }

done:
    if (ssl) { wolfSSL_shutdown(ssl); wolfSSL_free(ssl); }
    if (ctx) wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    shm->running = 0;
    return ok ? 0 : 1;
}

/* ================================================================
 * Client 进程
 * ================================================================ */

static int run_client(struct udp_shm *shm)
{
    int client_fd;
    struct sockaddr_in local_addr;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char buf[256];
    int ret, ok = 0;
    const char *msg = "Hello from tiny_udp DTLS client!";

    g_shm      = shm;
    g_is_server = 0;

    /* 1. 初始化协议栈 (IP=10.0.0.2) */
    tiny_udp_init(htonl(0x0A000002));

    /* 2. 创建 UDP socket */
    client_fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_fd < 0) {
        fprintf(stderr, "[Client] socket failed\n");
        return 1;
    }

    /* 3. connect 到服务器（记录对端地址，分配临时端口） */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family      = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port        = htons(0);

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(0x0A000001);
    srv_addr.sin_port        = htons(8443);
    tiny_udp_connect(client_fd, (struct sockaddr *)&srv_addr,
                     sizeof(srv_addr));
    printf("[Client] Socket fd=%d, connected to 10.0.0.1:8443\n", client_fd);

    /* 4. 设置非阻塞 */
    int nb = 1;
    tiny_udp_ioctl(client_fd, FIONBIO, &nb);

    /* 5. wolfSSL DTLS 初始化 */
    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
    if (!ctx) { fprintf(stderr, "[Client] CTX_new failed\n"); goto done; }

    {
        unsigned char *ca_buf = NULL;
        size_t ca_len = 0;

        if (read_file_to_buffer("server-cert.pem", &ca_buf, &ca_len) == 0) {
            wolfSSL_CTX_load_verify_buffer(ctx, ca_buf, (int)ca_len,
                                           WOLFSSL_FILETYPE_PEM);
            free(ca_buf);
        }
    }

    ssl = wolfSSL_new(ctx);
    if (!ssl) { fprintf(stderr, "[Client] SSL_new failed\n"); goto done; }

    /* 6. 设置 socket fd 和对端地址 */
    wolfSSL_set_fd(ssl, client_fd);
    wolfSSL_dtls_set_using_nonblock(ssl, 1);
    wolfSSL_dtls_set_peer(ssl, (struct sockaddr *)&srv_addr,
                          sizeof(srv_addr));

    shm->client_ready = 1;

    /* 等待 server 就绪 */
    while (shm->server_ready == 0) usleep(1000);

    /* 7. DTLS 握手 */
    printf("[Client] Starting DTLS handshake...\n");
    if (dtls_handshake(ssl, 0) < 0) goto done;
    printf("[Client] DTLS handshake OK\n");

    /* 8. 发消息 + 收回复（relay 循环） */
    {
        int rounds = 100;
        const char *expect = "Echo from tiny_udp DTLS server!";

        ret = wolfSSL_write(ssl, msg, (int)strlen(msg));
        printf("[Client] Sent: %d bytes\n", ret);

        memset(buf, 0, sizeof(buf));
        while (rounds-- > 0) {
            relay_cycle();
            usleep(1000);
            ret = wolfSSL_read(ssl, buf, sizeof(buf) - 1);
            if (ret > 0) break;
        }

        if (ret > 0) {
            printf("[Client] Received (%d bytes): \"%s\"\n", ret, buf);
            if (strcmp(buf, expect) == 0) {
                printf("[Client] Echo MATCH\n");
                ok = 1;
            } else {
                fprintf(stderr, "[Client] Echo MISMATCH\n");
            }
        } else {
            fprintf(stderr, "[Client] read failed: ret=%d err=%d\n",
                    ret, wolfSSL_get_error(ssl, ret));
        }
    }

done:
    if (ssl) { wolfSSL_shutdown(ssl); wolfSSL_free(ssl); }
    if (ctx) wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    shm->running = 0;
    return ok ? 0 : 1;
}

/* ================================================================
 * Main: mmap 共享内存 + fork
 * ================================================================ */

int main(void)
{
    struct udp_shm *shm;
    pid_t pid;
    int server_ok = 0, client_ok = 0;

    signal(SIGPIPE, SIG_IGN);

    shm = mmap(NULL, sizeof(struct udp_shm),
               PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n"); return 1;
    }
    memset(shm, 0, sizeof(struct udp_shm));
    shm->running = 1;

    printf("=== wolfSSL DTLS over Tiny UDP Socket (Shared Memory IPC) ===\n\n");

    pid = fork();
    if (pid < 0) { fprintf(stderr, "fork failed\n"); return 1; }

    if (pid == 0) {
        return run_server(shm);
    }

    client_ok = (run_client(shm) == 0);

    {
        int status;
        waitpid(pid, &status, 0);
        server_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    munmap(shm, sizeof(struct udp_shm));

    printf("\n=== Result: Server %s, Client %s ===\n",
           server_ok ? "PASS" : "FAIL",
           client_ok ? "PASS" : "FAIL");

    return (server_ok && client_ok) ? 0 : 1;
}
