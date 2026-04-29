/*
 * udp_stack.h — Tiny UDP Socket Protocol Stack
 *
 * 纯 C 实现，针对嵌入式裸机环境（MIPS / Loongson 2K1000）。
 * 无动态内存分配，全部使用静态数组。
 * 所有对外 API 使用 tiny_udp_ 前缀。
 */

#ifndef UDP_STACK_H
#define UDP_STACK_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 基础类型定义（裸机环境无标准头文件）
 * ================================================================ */

/*
 * 基础类型：嵌入式裸机环境无标准头文件，需自行定义。
 * 在托管环境（有标准头文件）中通过 #ifndef 避免重定义冲突。
 */
#ifndef UINT8_MAX
typedef unsigned char           uint8_t;
#endif
#ifndef UINT16_MAX
typedef unsigned short          uint16_t;
#endif
#ifndef UINT32_MAX
typedef unsigned int            uint32_t;
#endif
#ifndef INT32_MAX
typedef int                     int32_t;
#endif
#ifndef _SIZE_T
typedef unsigned int            size_t;
#endif
#ifndef _SSIZE_T
typedef int                     ssize_t;
#endif
#ifndef _SOCKLEN_T
typedef unsigned int            socklen_t;
#endif
#ifndef _NFDS_T
typedef unsigned int            nfds_t;
#endif
#ifndef _SUSECONDS_T
typedef long                    suseconds_t;
#endif

/* NULL 指针 */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* RTOS 适配层（裸机模式提供 no-op 回退） */
#include "udp_rtos_port.h"

/* 接收超时默认值 */
#ifdef UDP_RTOS
#define UDP_RECV_TIMEOUT_DEFAULT    UINT32_MAX  /* RTOS 模式默认无限阻塞 */
#else
#define UDP_RECV_TIMEOUT_DEFAULT    0           /* 裸机模式默认立即返回 */
#endif

/* ================================================================
 * 网络地址结构
 * 仅当系统未提供时定义（裸机环境），托管环境中跳过以避免
 * 与系统头文件冲突。
 * ================================================================ */

#if !defined(_NETINET_IN_H_) && !defined(_STRUCT_IN_ADDR)
struct in_addr {
    uint32_t s_addr;            /* 网络字节序 */
};
#define _STRUCT_IN_ADDR
#endif

#if !defined(_SYS_SOCKET_H_) && !defined(_STRUCT_SOCKADDR)
struct sockaddr {
    uint8_t     sa_len;         /* 总长度 */
    uint8_t     sa_family;      /* 地址族，AF_INET */
    char        sa_data[14];    /* 地址数据 */
};
#define _STRUCT_SOCKADDR
#endif

#if !defined(_NETINET_IN_H_) && !defined(_STRUCT_SOCKADDR_IN)
struct sockaddr_in {
    uint8_t     sin_len;        /* sizeof(struct sockaddr_in) */
    uint8_t     sin_family;     /* AF_INET */
    uint16_t    sin_port;       /* 端口号，网络字节序 */
    struct in_addr sin_addr;    /* IP 地址，网络字节序 */
    char        sin_zero[8];    /* 填充 */
};
#define _STRUCT_SOCKADDR_IN
#endif

/* ---- IPv6 address structure (for wolfSSL compatibility) ---- */

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

/* s6_addr 访问宏：仅在系统未定义时提供 */
#ifndef s6_addr
#define s6_addr  __in6_u.__s6_addr
#endif
#ifndef s6_addr16
#define s6_addr16 __in6_u.__s6_addr16
#endif
#ifndef s6_addr32
#define s6_addr32 __in6_u.__s6_addr32
#endif

#if !defined(_NETINET6_IN6_H_) && !defined(_STRUCT_SOCKADDR_IN6)
struct sockaddr_in6 {
    uint8_t         sin6_len;       /* sizeof(struct sockaddr_in6) */
    uint8_t         sin6_family;    /* AF_INET6 */
    uint16_t        sin6_port;      /* 端口号，网络字节序 */
    uint32_t        sin6_flowinfo;  /* flow info */
    struct in6_addr sin6_addr;      /* IPv6 地址 */
    uint32_t        sin6_scope_id;  /* scope ID */
};
#define _STRUCT_SOCKADDR_IN6
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN  46
#endif

/* sockaddr_storage — enough space for both IPv4 and IPv6 */
#if !defined(_SYS_SOCKET_H_) && !defined(_STRUCT_SOCKADDR_STORAGE)
struct sockaddr_storage {
    uint8_t     ss_len;
    uint8_t     ss_family;
    char        __ss_pad[126];
};
#define _STRUCT_SOCKADDR_STORAGE
#endif

/* ================================================================
 * 字节序转换（网络序 = 大端）
 * 仅在裸机环境（无系统头文件）时定义，托管环境使用系统提供版本。
 * ================================================================ */

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
 * select 相关（添加 guard 防止与系统头文件冲突）
 * ================================================================ */

#ifndef FD_SETSIZE
#define FD_SETSIZE  64
#endif

#ifndef _FD_SET  /* 系统头文件内部 guard (macOS/Linux) */
typedef struct {
    uint32_t fds_bits[((FD_SETSIZE + 31) / 32 > 0) ? (FD_SETSIZE + 31) / 32 : 2];
} fd_set;
#endif

#ifndef _STRUCT_TIMEVAL
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

/* ================================================================
 * poll 相关
 * ================================================================ */

#ifndef POLLIN
#define POLLIN          0x001
#endif
#ifndef POLLOUT
#define POLLOUT         0x004
#endif
#ifndef POLLERR
#define POLLERR         0x008
#endif

#ifndef _SYS_POLL_H_
struct pollfd {
    int     fd;
    short   events;
    short   revents;
};
#endif

/* ================================================================
 * 协议族 / socket 类型 / 协议
 * ================================================================ */

#ifndef AF_INET
#define AF_INET         2
#endif
#ifndef AF_INET6
#define AF_INET6        10
#endif
#ifndef PF_INET
#define PF_INET         AF_INET
#endif
#ifndef PF_INET6
#define PF_INET6        AF_INET6
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM      2
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP     17
#endif

/* ================================================================
 * Socket 选项层级和选项名
 * ================================================================ */

#ifndef SOL_SOCKET
#define SOL_SOCKET      1
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST    6
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO     20
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO     21
#endif
#ifndef SO_TYPE
#define SO_TYPE         17      /* wolfSSL NUCLEUS_PLUS_2_3 兼容值 */
#endif

/* ================================================================
 * ioctl / fcntl 命令和标志
 * ================================================================ */

#ifndef FIONBIO
#define FIONBIO         0x5421
#endif
#ifndef F_SETFL
#define F_SETFL         4
#endif
#ifndef F_GETFL
#define F_GETFL         3
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK      0x800
#endif

/* ================================================================
 * 地址常量
 * ================================================================ */

#ifndef INADDR_ANY
#define INADDR_ANY      0x00000000
#endif

/* ================================================================
 * recvfrom / recv / sendto / send flags
 * ================================================================ */

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT    0x40
#endif

/* ================================================================
 * 错误码
 * ================================================================ */

/* 错误码：仅在系统未提供时定义（兼容裸机与托管环境） */
#ifndef EAGAIN
#define EAGAIN          11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK     EAGAIN
#endif
#ifndef EBADF
#define EBADF           9
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

/* ================================================================
 * IP 协议号
 * ================================================================ */

#define IP_PROTO_UDP    17

/* ================================================================
 * 资源限制
 * ================================================================ */

#define UDP_SOCKET_MAX          64
#define UDP_SOCKET_BUF_DEPTH    32
#define UDP_PKT_BUF_SIZE        0x600

/* ================================================================
 * 收发包共享内存池（外部提供）
 * ================================================================ */

#define RX_POOL_SIZE    256
#define RX_PKT_MAXLEN   0x600

#define TX_POOL_SIZE    256
#define TX_PKT_MAXLEN   0x600

extern uint8_t rx_pool[RX_POOL_SIZE][RX_PKT_MAXLEN];
extern uint8_t tx_pool[TX_POOL_SIZE][TX_PKT_MAXLEN];

/*
 * 收发包池索引（供协议栈与外部系统共享）
 * g_rx_write_idx / g_tx_read_idx 由外部系统维护
 * g_rx_read_idx  / g_tx_write_idx 由协议栈维护
 */
extern volatile uint32_t g_rx_write_idx;
extern volatile uint32_t g_rx_read_idx;
extern volatile uint32_t g_tx_write_idx;
extern volatile uint32_t g_tx_read_idx;

/* ================================================================
 * 全局错误码
 *
 * 托管环境（有系统 <errno.h>）中 errno 通常是宏（如 (*__error())），
 * 此时直接使用系统 errno，不做替换。
 * 裸机环境（无系统 errno）中提供自己的全局 int 变量。
 * ================================================================ */

#ifndef errno
extern int errno;
#endif

/* ================================================================
 * Socket 状态
 * ================================================================ */

enum udp_socket_state {
    UDP_STATE_CLOSED    = 0,
    UDP_STATE_OPENED    = 1,
    UDP_STATE_BOUND     = 2,
    UDP_STATE_CONNECTED = 3,
    UDP_STATE_CLOSING   = 4
};

/* ================================================================
 * fd_set 操作宏
 * ================================================================ */

#ifndef FD_ZERO
#define FD_ZERO(set)                                                          \
    do {                                                                      \
        unsigned int _i;                                                      \
        for (_i = 0; _i < ((FD_SETSIZE + 31) / 32); _i++)                    \
            (set)->fds_bits[_i] = 0;                                          \
    } while (0)
#endif

#ifndef FD_SET
#define FD_SET(fd, set)                                                       \
    ((set)->fds_bits[(unsigned int)(fd) >> 5] |= (1u << ((unsigned int)(fd) & 31)))
#endif

#ifndef FD_CLR
#define FD_CLR(fd, set)                                                       \
    ((set)->fds_bits[(unsigned int)(fd) >> 5] &= ~(1u << ((unsigned int)(fd) & 31)))
#endif

#ifndef FD_ISSET
#define FD_ISSET(fd, set)                                                     \
    ((set)->fds_bits[(unsigned int)(fd) >> 5] & (1u << ((unsigned int)(fd) & 31)))
#endif

/* ================================================================
 * API 声明
 * ================================================================ */

/**
 * @brief 初始化协议栈，设置本机 IP 地址
 * @param local_ip_addr  本机 IP 地址（网络字节序）
 * @param local_mac      本机 MAC 地址（6 字节），预留参数
 */
void tiny_udp_init(uint32_t local_ip_addr);

/* ---- 核心 API ---- */

/**
 * @brief 创建 UDP socket
 * @param domain    AF_INET
 * @param type      SOCK_DGRAM
 * @param protocol  IPPROTO_UDP
 * @return 成功返回 fd (0-63)，失败返回 -1
 */
int  tiny_udp_socket(int domain, int type, int protocol);

/**
 * @brief 绑定本地端口
 * @param s         socket fd
 * @param name      目标地址（struct sockaddr_in *）
 * @param namelen   sizeof(struct sockaddr_in)
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_bind(int s, const struct sockaddr *name, socklen_t namelen);

/**
 * @brief 发送 UDP 数据报
 * @param s      socket fd
 * @param data   数据指针
 * @param size   数据长度
 * @param flags  标志（0 或 MSG_DONTWAIT）
 * @param to     目标地址
 * @param tolen  地址长度
 * @return 成功返回发送字节数，失败返回 -1
 */
int  tiny_udp_sendto(int s, const void *data, size_t size, int flags,
                     const struct sockaddr *to, socklen_t tolen);

/**
 * @brief 接收 UDP 数据报
 * @param s       socket fd
 * @param mem     接收缓冲区
 * @param len     缓冲区大小
 * @param flags   标志（0 或 MSG_DONTWAIT）
 * @param from    发送方地址（可为 NULL）
 * @param fromlen 地址长度指针（可为 NULL）
 * @return 成功返回接收字节数，无数据返回 -1（errno=EAGAIN）
 */
int  tiny_udp_recvfrom(int s, void *mem, size_t len, int flags,
                       struct sockaddr *from, socklen_t *fromlen);

/**
 * @brief 关闭 socket，释放资源
 * @param s  socket fd
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_close(int s);

/* ---- Socket 选项 ---- */

/**
 * @brief 设置 socket 选项
 * @param s       socket fd
 * @param level   选项层级（SOL_SOCKET）
 * @param optname 选项名
 * @param optval  选项值指针
 * @param optlen  选项值长度
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_setsockopt(int s, int level, int optname,
                         const void *optval, socklen_t optlen);

/**
 * @brief 获取 socket 选项
 * @param s       socket fd
 * @param level   选项层级（SOL_SOCKET）
 * @param optname 选项名
 * @param optval  选项值指针
 * @param optlen  选项值长度指针
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_getsockopt(int s, int level, int optname,
                         void *optval, socklen_t *optlen);

/* ---- connect 后简化收发 ---- */

/**
 * @brief 关联远端地址（UDP connect 只记录地址，不建立连接）
 * @param s       socket fd
 * @param name    远端地址
 * @param namelen 地址长度
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_connect(int s, const struct sockaddr *name, socklen_t namelen);

/**
 * @brief connect 后使用 send 发数据（无需每次指定目标地址）
 * @param s     socket fd
 * @param data  数据指针
 * @param size  数据长度
 * @param flags 标志（0 或 MSG_DONTWAIT）
 * @return 成功返回发送字节数，失败返回 -1
 */
int  tiny_udp_send(int s, const void *data, size_t size, int flags);

/**
 * @brief connect 后使用 recv 收数据（只接收来自已 connect 对端的数据）
 * @param s     socket fd
 * @param mem   接收缓冲区
 * @param len   缓冲区大小
 * @param flags 标志（0 或 MSG_DONTWAIT）
 * @return 成功返回接收字节数，无数据返回 -1
 */
int  tiny_udp_recv(int s, void *mem, size_t len, int flags);

/* ---- 地址查询 ---- */

/**
 * @brief 获取 socket 本地地址
 * @param s       socket fd
 * @param name    地址结构输出
 * @param namelen 地址长度指针（输入/输出）
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_getsockname(int s, struct sockaddr *name, socklen_t *namelen);

/**
 * @brief 获取 socket 远端地址（connect 后有效）
 * @param s       socket fd
 * @param name    地址结构输出
 * @param namelen 地址长度指针（输入/输出）
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_getpeername(int s, struct sockaddr *name, socklen_t *namelen);

/* ---- 多路复用 ---- */

/**
 * @brief select 多路复用
 * @param maxfdp1   最大 fd + 1
 * @param readset   可读集合
 * @param writeset  可写集合
 * @param exceptset 异常集合（未实现）
 * @param timeout   超时（NULL = 不超时）
 * @return 就绪 fd 数，超时返回 0，失败返回 -1
 */
int  tiny_udp_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                     fd_set *exceptset, struct timeval *timeout);

/**
 * @brief poll 多路复用
 * @param fds     pollfd 数组
 * @param nfds    数组元素个数
 * @param timeout 超时毫秒数（-1 = 无限，0 = 立即返回）
 * @return 就绪 fd 数，超时返回 0，失败返回 -1
 */
int  tiny_udp_poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* ---- IO 控制 ---- */

/**
 * @brief IO 控制
 * @param s    socket fd
 * @param cmd  命令（如 FIONBIO）
 * @param argp 参数指针
 * @return 成功返回 0，失败返回 -1
 */
int  tiny_udp_ioctl(int s, long cmd, void *argp);

/**
 * @brief 文件控制（设置标志如 O_NONBLOCK）
 * @param s   socket fd
 * @param cmd 命令（F_SETFL / F_GETFL）
 * @param val 参数值
 * @return 成功返回当前标志或 0，失败返回 -1
 */
int  tiny_udp_fcntl(int s, int cmd, int val);

/* ---- 地址转换工具 ---- */

/**
 * @brief 将网络字节序 IP 转换为点分十进制字符串
 * @param af   地址族（AF_INET）
 * @param src  源 IP 地址指针（网络字节序 uint32_t）
 * @param dst  输出字符串缓冲区（至少 16 字节）
 * @param size 输出缓冲区大小
 * @return 成功返回 dst，失败返回 NULL
 */
const char *tiny_udp_inet_ntop(int af, const void *src, char *dst, socklen_t size);

/**
 * @brief 将点分十进制字符串转换为网络字节序 IP
 * @param af  地址族（AF_INET）
 * @param src 源字符串（如 "192.168.1.1"）
 * @param dst 输出 IP 地址指针（uint32_t，网络字节序）
 * @return 成功返回 1，失败返回 0
 */
int tiny_udp_inet_pton(int af, const char *src, void *dst);

/* ---- 协议栈 tick ---- */

/**
 * @brief 协议栈轮询函数
 *
 * 外部系统在主循环中周期性调用。功能：
 * 1. 检查 rx_pool 是否有新包，解析后分发到对应 socket
 * 2. 处理外部系统已发送完成的 tx 槽回收（如有需要）
 *
 * 此函数不可阻塞，每次调用完成一轮处理即返回。
 */
void tiny_udp_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* UDP_STACK_H */
