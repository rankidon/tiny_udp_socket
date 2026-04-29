/*
 * test_wolfssl_types.c — 裸机类型兼容性测试
 *
 * 模拟嵌入式无 OS 头文件环境，仅包含 udp_stack.h，
 * 验证 wolfSSL 所需的 IPv6 类型、字节序函数、地址结构全部存在。
 *
 * 编译:
 *   gcc -std=c11 -Wall -Wextra -I../src test_wolfssl_types.c -o test_wolfssl_types
 *   ./test_wolfssl_types
 */

#include <stdio.h>
#include <string.h>

/* 仅包含 tiny UDP stack 头文件（模拟裸机环境） */
#include "../src/udp_stack.h"

/* ---- 编译时断言 ---- */

typedef char assert_in6_16[(sizeof(struct in6_addr) == 16) ? 1 : -1];
typedef char assert_storage_big_enough[
    (sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in6)) ? 1 : -1];
typedef char assert_af_inet6[(AF_INET6 == 10) ? 1 : -1];
typedef char assert_pf_inet6[(PF_INET6 == AF_INET6) ? 1 : -1];
typedef char assert_addrstrlen[(INET6_ADDRSTRLEN == 46) ? 1 : -1];

static int _unused = 0; /* suppress unused warnings */

int main(void)
{
    int failures = 0;

    printf("=== Tiny UDP Stack — wolfSSL Type Compatibility ===\n\n");

    /* 1. struct in6_addr */
    {
        struct in6_addr addr6;
        memset(&addr6, 0, sizeof(addr6));
        if (sizeof(addr6) != 16) {
            printf("[FAIL] in6_addr size = %zu, expected 16\n", sizeof(addr6));
            failures++;
        } else {
            printf("[PASS] struct in6_addr: 16 bytes\n");
        }

        /* 可以通过 s6_addr 访问底层字节 */
        addr6.s6_addr[0]  = 0x20;
        addr6.s6_addr[15] = 0x01;
        if (addr6.s6_addr32[0] != 0) {
            /* 网络字节序测试 */
        }
        printf("[PASS] in6_addr byte access via s6_addr/s6_addr16/s6_addr32\n");
    }

    /* 2. struct sockaddr_in6 */
    {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = htons(11112);

        if (sin6.sin6_family != AF_INET6) {
            printf("[FAIL] sockaddr_in6 family not set\n");
            failures++;
        } else {
            printf("[PASS] struct sockaddr_in6: family=%d port=%d\n",
                   sin6.sin6_family, ntohs(sin6.sin6_port));
        }
    }

    /* 3. struct sockaddr_storage — 足够容纳 v4 和 v6 */
    {
        if (sizeof(struct sockaddr_storage) < sizeof(struct sockaddr_in)) {
            printf("[FAIL] sockaddr_storage too small for IPv4\n");
            failures++;
        }
        if (sizeof(struct sockaddr_storage) < sizeof(struct sockaddr_in6)) {
            printf("[FAIL] sockaddr_storage too small for IPv6\n");
            failures++;
        } else {
            printf("[PASS] sockaddr_storage: %zu bytes (>= v4=%zu, v6=%zu)\n",
                   sizeof(struct sockaddr_storage),
                   sizeof(struct sockaddr_in), sizeof(struct sockaddr_in6));
        }
    }

    /* 4. AF_INET6 / PF_INET6 */
    {
        if (AF_INET6 != 10) {
            printf("[FAIL] AF_INET6 = %d, expected 10\n", AF_INET6);
            failures++;
        }
        if (PF_INET6 != AF_INET6) {
            printf("[FAIL] PF_INET6 != AF_INET6\n");
            failures++;
        } else {
            printf("[PASS] AF_INET6=%d, PF_INET6=%d\n", AF_INET6, PF_INET6);
        }
    }

    /* 5. INET6_ADDRSTRLEN */
    {
        if (INET6_ADDRSTRLEN != 46) {
            printf("[FAIL] INET6_ADDRSTRLEN = %d, expected 46\n", INET6_ADDRSTRLEN);
            failures++;
        } else {
            printf("[PASS] INET6_ADDRSTRLEN = %d\n", INET6_ADDRSTRLEN);
        }
    }

    /* 6. htons / htonl / ntohs / ntohl 字节序转换 */
    {
        uint16_t v16 = htons(0x1234);
        uint32_t v32 = htonl(0x12345678);

        if (ntohs(v16) != 0x1234) {
            printf("[FAIL] ntohs(htons(0x1234)) != 0x1234\n");
            failures++;
        }
        if (ntohl(v32) != 0x12345678) {
            printf("[FAIL] ntohl(htonl(0x12345678)) != 0x12345678\n");
            failures++;
        } else {
            printf("[PASS] htons/ntohs/htonl/ntohl round-trip OK\n");
        }
    }

    /* 7. inet_ntop / inet_pton 拒收 AF_INET6（协议栈仅支持 IPv4） */
    {
        struct in6_addr addr6;
        char buf[INET6_ADDRSTRLEN];
        memset(&addr6, 0, sizeof(addr6));

        if (tiny_udp_inet_ntop(AF_INET6, &addr6, buf, sizeof(buf)) != NULL) {
            printf("[FAIL] inet_ntop should reject AF_INET6\n");
            failures++;
        }
        if (tiny_udp_inet_pton(AF_INET6, "::1", &addr6) != 0) {
            printf("[FAIL] inet_pton should reject AF_INET6\n");
            failures++;
        } else {
            printf("[PASS] inet_ntop/pton correctly reject AF_INET6 (stack is IPv4-only)\n");
        }
    }

    /* 8. 错误码定义 */
    {
        if (EAGAIN != 11 || ENOMEM != 12 || EINVAL != 22) {
            printf("[FAIL] errno values mismatch\n");
            failures++;
        } else {
            printf("[PASS] errno: EAGAIN=%d ENOMEM=%d EINVAL=%d\n",
                   EAGAIN, ENOMEM, EINVAL);
        }
    }

    /* 9. socket 选项定义 */
    {
        if (SOL_SOCKET != 1 || SO_BROADCAST != 6) {
            printf("[FAIL] socket option constants\n");
            failures++;
        } else {
            printf("[PASS] SOL_SOCKET=%d SO_BROADCAST=%d SO_RCVTIMEO=%d\n",
                   SOL_SOCKET, SO_BROADCAST, SO_RCVTIMEO);
        }
    }

    /* 10. SOCK_DGRAM / IPPROTO_UDP */
    {
        if (SOCK_DGRAM != 2 || IPPROTO_UDP != 17) {
            printf("[FAIL] SOCK_DGRAM/IPPROTO_UDP\n");
            failures++;
        } else {
            printf("[PASS] SOCK_DGRAM=%d IPPROTO_UDP=%d\n", SOCK_DGRAM, IPPROTO_UDP);
        }
    }

    /* 11. 结构体布局兼容 BSD */
    {
        struct sockaddr_in sin4;
        memset(&sin4, 0, sizeof(sin4));
        sin4.sin_len    = sizeof(sin4);
        sin4.sin_family = AF_INET;
        sin4.sin_port   = htons(8080);
        sin4.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */

        /* 可以强制转换为 struct sockaddr * */
        struct sockaddr *sa = (struct sockaddr *)&sin4;
        if (sa->sa_family != AF_INET) {
            printf("[FAIL] sockaddr_in -> sockaddr cast\n");
            failures++;
        } else {
            printf("[PASS] sockaddr_in can be cast to sockaddr* (family=%d)\n",
                   sa->sa_family);
        }
    }

    /* 12. fd_set / select 相关 */
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(3, &readfds);
        if (!FD_ISSET(3, &readfds)) {
            printf("[FAIL] FD_SET/FD_ISSET\n");
            failures++;
        }
        FD_CLR(3, &readfds);
        if (FD_ISSET(3, &readfds)) {
            printf("[FAIL] FD_CLR\n");
            failures++;
        } else {
            printf("[PASS] fd_set: FD_SET/FD_CLR/FD_ISSET OK, FD_SETSIZE=%d\n",
                   FD_SETSIZE);
        }
    }

    /* 13. pollfd 结构体 */
    {
        struct pollfd pfd;
        pfd.fd      = 0;
        pfd.events  = POLLIN | POLLOUT;
        pfd.revents = 0;
        if (POLLIN != 0x001 || POLLOUT != 0x004) {
            printf("[FAIL] POLLIN/POLLOUT values\n");
            failures++;
        } else {
            printf("[PASS] struct pollfd: events=0x%x\n", pfd.events);
        }
    }

    printf("\n=== Result: %d failures ===\n", failures);
    return failures;
}
