/*
 * test_wolfssl_dtls.c — wolfSSL DTLS over UDP 集成测试
 *
 * 展示 BSD socket API 模式 + wolfSSL DTLS 的端到端流程:
 *   socket → bind → setsockopt → [wolfSSL DTLS handshake] → sendto/recvfrom → close
 *
 * 与 tiny_udp_socket 的 socket API 一一对应。
 *
 * 编译:
 *   gcc -std=c11 -Wall -Wextra -I/opt/homebrew/include \
 *       test_wolfssl_dtls.c -L/opt/homebrew/lib -lwolfssl -o test_wolfssl_dtls
 * 运行:
 *   cd test && ./test_wolfssl_dtls
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

static void dtls_test(void)
{
    int server_fd, client_fd;
    struct sockaddr_in addr;
    struct timeval tv;
    char buf[256];
    pid_t pid = 0;
    int ret;
    WOLFSSL_CTX *ctx_srv, *ctx_cli;
    WOLFSSL *ssl_srv = NULL, *ssl_cli = NULL;

    const int PORT = 34567;
    const char *msg = "Hello DTLS!";
    int ok = 1;

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    wolfSSL_Init();

    /* 1. socket() — 对应 tiny_udp_socket(AF_INET, SOCK_DGRAM, 0) */
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0 || client_fd < 0) { perror("socket"); exit(1); }

    /* 2. setsockopt — 对应 tiny_udp_setsockopt */
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 3. bind — 对应 tiny_udp_bind */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    printf("[INFO] Server bound to 127.0.0.1:%d\n", PORT);

    /* 4. wolfSSL CTX 初始化 */
    ctx_srv = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    ctx_cli = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
    if (!ctx_srv || !ctx_cli) { fprintf(stderr, "CTX_new failed\n"); exit(1); }

    ret = wolfSSL_CTX_use_certificate_chain_file(ctx_srv, "server-cert.pem");
    if (ret != WOLFSSL_SUCCESS) {
        printf("[SKIP] Cert files not in test/ dir.\n");
        printf("  Generate: openssl ecparam -name prime256v1 -genkey -noout -out server-key.pem\n");
        printf("            openssl req -new -x509 -key server-key.pem -out server-cert.pem -days 30 -subj '/CN=localhost'\n");
        goto done;
    }
    wolfSSL_CTX_use_PrivateKey_file(ctx_srv, "server-key.pem", WOLFSSL_FILETYPE_PEM);
    wolfSSL_CTX_load_verify_locations(ctx_cli, "server-cert.pem", NULL);

    ssl_srv = wolfSSL_new(ctx_srv);
    ssl_cli = wolfSSL_new(ctx_cli);
    if (!ssl_srv || !ssl_cli) { fprintf(stderr, "SSL_new failed\n"); exit(1); }

    /* DTLS 需要显式设置对端地址 */
    {
        struct sockaddr_in cli_addr;
        memset(&cli_addr, 0, sizeof(cli_addr));
        cli_addr.sin_family      = AF_INET;
        cli_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        cli_addr.sin_port        = htons(PORT);

        wolfSSL_dtls_set_peer(ssl_cli, (struct sockaddr *)&cli_addr,
                              sizeof(cli_addr));
    }

    wolfSSL_set_fd(ssl_srv, server_fd);
    wolfSSL_set_fd(ssl_cli, client_fd);

    /* 5. fork — server 子进程, client 父进程 */
    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }

    if (pid == 0) {
        /* === SERVER === */
        close(client_fd);
        wolfSSL_free(ssl_cli); wolfSSL_CTX_free(ctx_cli);

        ret = wolfSSL_accept(ssl_srv);
        if (ret != WOLFSSL_SUCCESS) {
            fprintf(stderr, "[Server] accept failed: %d\n", ret);
            goto server_done;
        }
        printf("[Server] DTLS handshake OK\n");

        /* recvfrom-style: 接收客户端消息 */
        ret = wolfSSL_read(ssl_srv, buf, sizeof(buf) - 1);
        if (ret <= 0) { fprintf(stderr, "[Server] read failed\n"); ok = 0; goto server_done; }
        buf[ret] = '\0';
        printf("[Server] Read: \"%s\"\n", buf);

        /* sendto-style: 回显 */
        ret = wolfSSL_write(ssl_srv, buf, ret);
        printf("[Server] Echo: %d bytes\n", ret);

server_done:
        wolfSSL_shutdown(ssl_srv);
        wolfSSL_free(ssl_srv); wolfSSL_CTX_free(ctx_srv);
        close(server_fd);
        exit(ok ? 0 : 1);
    }

    /* === CLIENT === */
    close(server_fd);
    wolfSSL_free(ssl_srv); wolfSSL_CTX_free(ctx_srv);

    /* DTLS 客户端无需 connect(), 已通过 dtls_set_peer 设置对端地址 */
    printf("[Client] Peer set to 127.0.0.1:%d\n", PORT);

    /* DTLS 握手 */
    ret = wolfSSL_connect(ssl_cli);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "[Client] connect() failed: %d\n", ret);
        ok = 0; goto done;
    }
    printf("[Client] DTLS handshake OK\n");

    /* DTLS 通信端点已通过 dtls_set_peer() 设置 */
    /* 对应 tiny_udp_connect 记录对端地址 */
    printf("[Client] DTLS peer: 127.0.0.1:%d\n", PORT);

    /* sendto-style: 发送消息 — 对应 wolfSSL_write → tiny_udp_sendto */
    ret = wolfSSL_write(ssl_cli, msg, (int)strlen(msg));
    printf("[Client] Sent %d bytes\n", ret);

    /* select — 对应 tiny_udp_select, 等待数据到达 */
    {
        fd_set rfds;
        struct timeval sel_tv = {3, 0};
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        ret = select(client_fd + 1, &rfds, NULL, NULL, &sel_tv);
        printf("[Client] select() = %d (readable=%d)\n",
               ret, FD_ISSET(client_fd, &rfds));
    }

    /* recvfrom-style: 接收回显 — 对应 wolfSSL_read → tiny_udp_recvfrom */
    ret = wolfSSL_read(ssl_cli, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        fprintf(stderr, "[Client] read failed: %d\n", ret);
        ok = 0; goto done;
    }
    buf[ret] = '\0';
    printf("[Client] Read: \"%s\"\n", buf);

    if (strcmp(buf, msg) == 0) {
        printf("[Client] Echo MATCH ✓\n");
    } else {
        fprintf(stderr, "[Client] Echo MISMATCH ✗\n");
        ok = 0;
    }

done:
    wolfSSL_shutdown(ssl_cli);
    wolfSSL_free(ssl_cli); wolfSSL_CTX_free(ctx_cli);
    close(client_fd);
    wolfSSL_Cleanup();

    /* 等待子进程 */
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ok = 1;
    }

    printf("\n=== DTLS test %s ===\n", ok ? "PASSED" : "FAILED");
    exit(ok ? 0 : 1);
}

int main(void)
{
    dtls_test();
    return 0;
}
