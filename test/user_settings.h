/*
 * user_settings.h — wolfSSL bare-metal embedded configuration
 *
 * 无操作系统依赖，无文件系统依赖。
 * 供 tiny_udp_socket DTLS 测试使用。
 * 生产环境请根据实际硬件配置调整。
 */

#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* ================================================================
 * 平台：无操作系统 / 无文件系统
 * ================================================================ */

#define NO_FILESYSTEM           /* 无文件 I/O（证书通过 buffer 加载） */
#define NO_WRITEV               /* 无 scatter/gather I/O */
#define WOLFSSL_NO_SOCK         /* 无系统 socket 头文件，完全裸机类型 */
#define USE_WOLFSSL_IO          /* 启用 wolfSSL 默认 Embed* I/O（WOLFSSL_NO_SOCK 时不自动定义） */
#define NO_DEV_RANDOM           /* 无 /dev/random */
#define WOLFSSL_GENSEED_FORTEST /* wolfSSL 内置测试种子生成器（生产需替换为硬件 RNG） */
#define SINGLE_THREADED         /* 单线程 */
#define WOLFSSL_SMALL_STACK     /* 堆栈优化 */
#define NO_MAIN_DRIVER          /* 无 wolfSSL 自带 main/test 驱动 */

/* 不做操作系统底层的信号/进程操作 */
#define NO_SIGPIPE

/* ================================================================
 * 裸机 socket 类型（替代系统 <sys/socket.h>）
 * 定义 HAVE_SOCKADDR 并在此之前提供 struct 定义，
 * 使 wolfSSL 的 wolfio.h 能生成 SOCKADDR_S / SOCKADDR_IN 等 typedef。
 * 与 udp_stack.h 中的定义使用相同的 #ifndef guard，避免重复定义。
 * ================================================================ */

#include <stddef.h>

#ifndef _STDINT_H
typedef unsigned char           uint8_t;
typedef unsigned short          uint16_t;
typedef unsigned int            uint32_t;
#endif

#if !defined(_NETINET_IN_H_) && !defined(_STRUCT_IN_ADDR)
struct in_addr {
    uint32_t s_addr;
};
#define _STRUCT_IN_ADDR
#endif

#if !defined(_SYS_SOCKET_H_) && !defined(_STRUCT_SOCKADDR)
struct sockaddr {
    uint8_t     sa_len;
    uint8_t     sa_family;
    char        sa_data[14];
};
#define _STRUCT_SOCKADDR
#endif

#if !defined(_NETINET_IN_H_) && !defined(_STRUCT_SOCKADDR_IN)
struct sockaddr_in {
    uint8_t     sin_len;
    uint8_t     sin_family;
    uint16_t    sin_port;
    struct in_addr sin_addr;
    char        sin_zero[8];
};
#define _STRUCT_SOCKADDR_IN
#endif

#if !defined(_NETINET6_IN6_H_) && !defined(_STRUCT_IN6_ADDR)
struct in6_addr {
    union {
        uint8_t  __s6_addr[16];
        uint16_t __s6_addr16[8];
        uint32_t __s6_addr32[4];
    } __in6_u;
};
#define _STRUCT_IN6_ADDR
#endif
#ifndef s6_addr
#define s6_addr  __in6_u.__s6_addr
#endif

#if !defined(_NETINET6_IN6_H_) && !defined(_STRUCT_SOCKADDR_IN6)
struct sockaddr_in6 {
    uint8_t         sin6_len;
    uint8_t         sin6_family;
    uint16_t        sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};
#define _STRUCT_SOCKADDR_IN6
#endif

#if !defined(_SYS_SOCKET_H_) && !defined(_STRUCT_SOCKADDR_STORAGE)
struct sockaddr_storage {
    uint8_t     ss_len;
    uint8_t     ss_family;
    char        __ss_pad[126];
};
#define _STRUCT_SOCKADDR_STORAGE
#endif

/* wolfSSL socket type aliases */
#ifndef HAVE_SOCKADDR
#define HAVE_SOCKADDR
#endif

#ifndef _SOCKLEN_T
typedef unsigned int socklen_t;
#define _SOCKLEN_T
#endif

#ifndef XSOCKLENT
#define XSOCKLENT socklen_t
#endif

/* wolfSSL socket 类型别名（WOLFSSL_NO_SOCK 时 wolfio.h 不生成，需手动提供） */
#ifndef HAVE_SOCKADDR_DEFINED
typedef struct sockaddr         SOCKADDR;
#define HAVE_SOCKADDR_DEFINED
#endif
typedef struct sockaddr_storage SOCKADDR_S;
typedef struct sockaddr_in      SOCKADDR_IN;
typedef struct sockaddr_in6     SOCKADDR_IN6;

/* errno — 裸机环境用简单全局变量 */
#ifndef errno
extern int errno;
#endif

/* 错误码（裸机环境自定义值，与 Linux 标准值一致） */
#ifndef EAGAIN
#define EAGAIN          11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK     EAGAIN
#endif
#ifndef EINVAL
#define EINVAL          22
#endif
#ifndef ENOMEM
#define ENOMEM          12
#endif
#ifndef ENOTCONN
#define ENOTCONN        107
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP      95
#endif
#ifndef EADDRINUSE
#define EADDRINUSE      98
#endif
#ifndef EISCONN
#define EISCONN         106
#endif
#ifndef ENOTSOCK
#define ENOTSOCK        88
#endif
#ifndef EMSGSIZE
#define EMSGSIZE        90
#endif
#ifndef EBADF
#define EBADF           9
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT       110
#endif
#ifndef ECONNRESET
#define ECONNRESET      104
#endif
#ifndef EINTR
#define EINTR           4
#endif
#ifndef EPIPE
#define EPIPE           32
#endif
#ifndef ECONNABORTED
#define ECONNABORTED    103
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED    111
#endif

/* ssize_t（裸机环境可能无 <sys/types.h>） */
#ifndef _SSIZE_T
typedef long ssize_t;
#define _SSIZE_T
#endif

/* socket 函数原型（wolfSSL wolfio.c 编译时需要） */
ssize_t send(int s, const void *buf, size_t len, int flags);
ssize_t recv(int s, void *buf, size_t len, int flags);
ssize_t sendto(int s, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int getpeername(int s, struct sockaddr *name, socklen_t *namelen);

/* wolfSSL wolfio.c 需要的额外类型（WOLFSSL_NO_SOCK 时不自动定义） */
#ifndef XSOCKOPT_TYPE_OPTVAL_TYPE
#define XSOCKOPT_TYPE_OPTVAL_TYPE void*
#endif

/* Socket 选项 */
#ifndef SOL_SOCKET
#define SOL_SOCKET      1
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO     20
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO     21
#endif
#ifndef SO_TYPE
#define SO_TYPE         17
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST    6
#endif

/* IO 控制 */
#ifndef FIONBIO
#define FIONBIO         0x5421
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK      0x800
#endif

/* 协议族 */
#ifndef AF_INET
#define AF_INET         2
#endif
#ifndef AF_INET6
#define AF_INET6        10
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM      2
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP     17
#endif
#ifndef INADDR_ANY
#define INADDR_ANY      0x00000000
#endif

/* 字节序 */
#ifndef htons
static inline uint16_t htons(uint16_t hostshort)
{
    return (uint16_t)((hostshort >> 8) | (hostshort << 8));
}
#endif
#ifndef htonl
static inline uint32_t htonl(uint32_t hostlong)
{
    return ((hostlong >> 24) & 0x000000FF) |
           ((hostlong >>  8) & 0x0000FF00) |
           ((hostlong <<  8) & 0x00FF0000) |
           ((hostlong << 24) & 0xFF000000);
}
#endif
#ifndef ntohs
#define ntohs htons
#endif
#ifndef ntohl
#define ntohl htonl
#endif

/* ================================================================
 * 协议支持
 * ================================================================ */

#define WOLFSSL_DTLS            /* DTLS 1.2 */
/* DTLS 1.3 需要 WOLFSSL_TLS13，暂不开启以减小体积 */
/* #define WOLFSSL_DTLS13 */
#define HAVE_TLS_EXTENSIONS     /* TLS 扩展（DTLS 必需） */

/* ================================================================
 * 密钥交换：仅 ECC
 * ================================================================ */

#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define TFM_TIMING_RESISTANT
#define ECC_SHAMIR
#define HAVE_ECC256             /* NIST P-256 */
#define HAVE_SUPPORTED_CURVES   /* Supported Curves 扩展 */

/* ================================================================
 * 对称加密 / 哈希
 * ================================================================ */

#define HAVE_AESGCM             /* AES-GCM（DTLS 1.2 cipher suite） */
#define HAVE_HKDF               /* HKDF（TLS 1.3 / DTLS 1.3 必需） */

/* 关闭不需要的算法（减小体积） */
#define NO_DH
#define NO_DSA
#define NO_RSA
#define NO_HC128
#define NO_RABBIT
#define NO_PSK
#define NO_RC4
#define NO_DES3
#define NO_IDEA
#define NO_MD4
#define NO_MD5
#define NO_SHA                  /* SHA-1（禁用，仅用 SHA-256/384） */
#define NO_SHA224
#define NO_OLD_TLS              /* 无需 TLS 1.0/1.1 */

/* ================================================================
 * 内存 / 日志
 * ================================================================ */

#define WOLFSSL_ERR_STR         /* 错误描述字符串（调试时开启） */

/* 测试环境使用系统 malloc/free；裸机需替换为静态内存池 */
/* #define WOLFSSL_NO_MALLOC */

#endif /* WOLFSSL_USER_SETTINGS_H */
