# UDP Socket Protocol Stack - Claude Code 任务

## 项目概述

在嵌入式裸机环境中，用纯 C 实现一个仅支持 UDP 的 socket 协议栈。不使用任何外部库，纯静态内存分配，无 malloc。

## 硬件资源约束

- 平台：嵌入式裸机（无 RTOS）
- CPU：MIPS 架构（Loongson 2K1000）
- 无操作系统，无堆管理器

## 内存池接口

外部系统提供两个大数组作为收发包的共享内存池：

```c
// 收包池：外部系统从网卡收到完整 IP 包后写入此池
// 这是一个循环数组，由外部系统管理写入指针
// 每个槽最大 0x600 字节
#define RX_POOL_SIZE    256
#define RX_PKT_MAXLEN   0x600
extern uint8_t rx_pool[RX_POOL_SIZE][RX_PKT_MAXLEN];

// 发包池：协议栈将待发送的完整 IP 包写入此池
// 外部系统轮询此池，发现有包就发出去
#define TX_POOL_SIZE    256
#define TX_PKT_MAXLEN   0x600
extern uint8_t tx_pool[TX_POOL_SIZE][TX_PKT_MAXLEN];
```

### 收包机制

外部系统负责：
1. 从网卡收包，解析出完整的 IP 包（含 IP 头 + UDP 头 + payload）
2. 写入 rx_pool 的下一个空闲槽，槽满时回绕（循环数组）
3. 维护一个写入索引 write_idx（自增，模 RX_POOL_SIZE）

协议栈需要：
1. 维护一个读取索引 read_idx，独立于外部系统的 write_idx
2. 轮询比较 read_idx != write_idx 来判断是否有新包
3. 从 rx_pool[read_idx] 读取完整 IP 包，解析后分发到对应 socket
4. 每处理完一个包，read_idx++（模 RX_POOL_SIZE）

### 发包机制

协议栈需要：
1. 在网络层组装完整的 IP 包（IP 头 + UDP 头 + payload）
2. 写入 tx_pool 的下一个空闲槽
3. 维护一个写入索引协议栈的 write_idx

外部系统负责：
1. 维护自己的读取索引，轮询检测 tx_pool 是否有新包
2. 发现新包后从网卡发送出去

### 轮询接口

提供一个 tick 函数供外部系统周期性调用：

```c
/**
 * @brief 协议栈轮询函数，外部系统在主循环中周期性调用
 * 
 * 功能：
 * 1. 检查 rx_pool 是否有新包（read_idx vs write_idx）
 * 2. 如果有，解析 IP 头 + UDP 头，按目标端口分发到对应 socket
 * 3. 处理发包请求（如果有待发包，构造 IP 头并写入 tx_pool）
 * 
 * 此函数不能阻塞
 */
void udp_tick(void);
```

## 功能需求

### 1. 支持的 API（BSD socket 风格，前缀 `tiny_udp_`）

所有对外接口统一使用前缀 `tiny_udp_`：

```c
// ==== 核心 ====
int  tiny_udp_socket(int domain, int type, int protocol);
int  tiny_udp_bind(int s, const struct sockaddr *name, socklen_t namelen);
int  tiny_udp_sendto(int s, const void *dataptr, size_t size, int flags,
                     const struct sockaddr *to, socklen_t tolen);
int  tiny_udp_recvfrom(int s, void *mem, size_t len, int flags,
                       struct sockaddr *from, socklen_t *fromlen);
int  tiny_udp_close(int s);

// ==== Socket 选项 ====
int  tiny_udp_setsockopt(int s, int level, int optname,
                         const void *optval, socklen_t optlen);
int  tiny_udp_getsockopt(int s, int level, int optname,
                         void *optval, socklen_t *optlen);

// ==== connect 后简化收发 ====
int  tiny_udp_connect(int s, const struct sockaddr *name, socklen_t namelen);
int  tiny_udp_send(int s, const void *dataptr, size_t size, int flags);
int  tiny_udp_recv(int s, void *mem, size_t len, int flags);

// ==== 地址查询 ====
int  tiny_udp_getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int  tiny_udp_getpeername(int s, struct sockaddr *name, socklen_t *namelen);

// ==== 多路复用 ====
int  tiny_udp_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                     fd_set *exceptset, struct timeval *timeout);
int  tiny_udp_poll(struct pollfd *fds, nfds_t nfds, int timeout);

// ==== IO控制 ====
int  tiny_udp_ioctl(int s, long cmd, void *argp);
int  tiny_udp_fcntl(int s, int cmd, int val);

// ==== 地址转换工具 ====
const char *tiny_udp_inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         tiny_udp_inet_pton(int af, const char *src, void *dst);
```

### 1b. 无前缀兼容头文件

提供第二个头文件 `udp_socket_portable.h`，通过宏定义将无前缀名称映射到 `tiny_udp_` 版本：

```c
// udp_socket_portable.h
// 为习惯标准 BSD socket 名称的用户提供无前缀兼容层
// 包含此文件后，可直接使用 socket()、bind() 等标准名称

#ifndef UDP_SOCKET_PORTABLE_H
#define UDP_SOCKET_PORTABLE_H

#include "udp_stack.h"

#define socket          tiny_udp_socket
#define bind            tiny_udp_bind
#define sendto          tiny_udp_sendto
#define recvfrom        tiny_udp_recvfrom
#define close           tiny_udp_close

#define setsockopt      tiny_udp_setsockopt
#define getsockopt      tiny_udp_getsockopt

#define connect         tiny_udp_connect
#define send            tiny_udp_send
#define recv            tiny_udp_recv

#define getsockname     tiny_udp_getsockname
#define getpeername     tiny_udp_getpeername

#define select          tiny_udp_select
#define poll            tiny_udp_poll

#define ioctl           tiny_udp_ioctl
#define fcntl           tiny_udp_fcntl

#define inet_ntop       tiny_udp_inet_ntop
#define inet_pton       tiny_udp_inet_pton

#endif /* UDP_SOCKET_PORTABLE_H */
```
// ==== Socket 选项 ====
int  setsockopt(int s, int level, int optname,
                const void *optval, socklen_t optlen);
int  getsockopt(int s, int level, int optname,
                void *optval, socklen_t *optlen);

// ==== connect 后简化收发 ====
int  connect(int s, const struct sockaddr *name, socklen_t namelen);
int  send(int s, const void *dataptr, size_t size, int flags);
int  recv(int s, void *mem, size_t len, int flags);

// ==== 地址查询 ====
int  getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int  getpeername(int s, struct sockaddr *name, socklen_t *namelen);

// ==== 多路复用 ====
int  select(int maxfdp1, fd_set *readset, fd_set *writeset,
            fd_set *exceptset, struct timeval *timeout);
int  poll(struct pollfd *fds, nfds_t nfds, int timeout);

// ==== IO控制 ====
int  ioctl(int s, long cmd, void *argp);
int  fcntl(int s, int cmd, int val);

// ==== 地址转换工具 ====
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int        inet_pton(int af, const char *src, void *dst);
```

### 2. flags 参数

recvfrom 和 recv 的 flags 参数支持：
- 0：阻塞模式（如果没有数据，等下一个 tick 再来检查）
- MSG_DONTWAIT：非阻塞模式（没有数据立即返回 -1，errno 设为 EWOULDBLOCK）
- 由于是裸机环境，阻塞模式实际上是"轮询等待"：在 udp_tick 中被处理前，recvfrom 通过一个信号/标志位等待，或者简单地返回 -1 并设 EAGAIN

实际上在裸机 tick 驱动模式下，**所有非阻塞**：
- flags=0：如果没有数据，返回 -1，errno = EAGAIN
- flags=MSG_DONTWAIT：同上
- 对外表现一致，但两者都应当被支持

### 3. 非阻塞模式（fcntl/ioctl）

通过 fcntl(s, F_SETFL, O_NONBLOCK) 或 ioctl(s, FIONBIO, &on) 设置非阻塞后：
- recvfrom/recv 没有数据时立即返回 -1，errno = EAGAIN
- sendto/send 立即返回（UDP 无连接，数据写入 tx_pool 即返回）

### 4. 状态机

每个 socket 的完整状态：

```
CLOSED:    初始状态，刚创建或已关闭
           可调用：socket（返回新 fd）、close（无操作）
           
OPENED:    socket() 调用成功后的状态，未绑定端口
           可调用：bind、close、setsockopt
           
BOUND:     bind() 成功后，已绑定本地端口，可以收发
           可调用：sendto、recvfrom、connect、close
           
CONNECTED: connect() 后，已记录远端地址
           可调用：send、recv、sendto、recvfrom、close
           
CLOSING:   close() 正在关闭中，释放资源
           下一个 tick 自动进入 CLOSED
```

### 5. socket 内部数据结构

每个 socket 包含：
- fd（0-63 的索引号）
- 当前状态
- 绑定的本地端口号（BOUND 后有效）
- connect 后的远端 IP + 端口（CONNECTED 后有效）
- 标志位（非阻塞标志等）
- 收包缓冲区：固定 32 个包槽的循环队列
  - 每个包包含：源 IP、源端口、payload 长度、payload 数据（0x600 字节内）
- 是否已配置（避免遍历所有 64 个）

### 6. 静态内存分配

全部使用全局静态数组，无 malloc：

```c
// 最大 socket 数
#define UDP_SOCKET_MAX      64

// 每个 socket 最大缓存包数
#define UDP_SOCKET_BUF_DEPTH  32

// 包缓冲区槽大小
#define UDP_PKT_BUF_SIZE   0x600

// socket 控制块数组（静态）
static struct udp_socket {
    uint8_t     used;           // 是否已被占用
    uint8_t     state;          // CLOSED/OPENED/BOUND/CONNECTED
    uint16_t    local_port;     // bind 的本地端口
    uint32_t    peer_ip;        // connect 后的远端 IP
    uint16_t    peer_port;      // connect 后的远端端口
    uint8_t     flags;          // O_NONBLOCK 等
    
    // 收包循环缓冲
    uint8_t     pkt_buf[UDP_SOCKET_BUF_DEPTH][UDP_PKT_BUF_SIZE];
    uint16_t    pkt_len[UDP_SOCKET_BUF_DEPTH];     // 每个包的实际长度
    uint32_t    pkt_src_ip[UDP_SOCKET_BUF_DEPTH];  // 每个包的源 IP
    uint16_t    pkt_src_port[UDP_SOCKET_BUF_DEPTH];// 每个包的源端口
    uint8_t     pkt_rp;         // 读指针（应用层下次读哪个包）
    uint8_t     pkt_wp;         // 写指针（协议栈下次写哪个包）
} sockets[UDP_SOCKET_MAX];
```

### 7. IP/UDP 头处理

收到的完整包格式（以太网帧的 payload，不含以太网头）：

```
IPv4 Header (20 bytes + options):
  Version+IHL (1B) | DSCP+ECN (1B) | Total Length (2B)
  Identification (2B) | Flags+FragOffset (2B)
  TTL (1B) | Protocol (1B) | Header Checksum (2B)
  Source IP (4B) | Destination IP (4B)
  [Options if IHL > 5]

UDP Header (8 bytes):
  Source Port (2B) | Destination Port (2B)
  Length (2B) | Checksum (2B)

Payload (UDP Length - 8 bytes)
```

发送包时，协议栈需要：
1. 构造 UDP 头（源端口、目标端口、长度、校验和=0 或计算）
2. 构造 IP 头（版本=4, IHL=5, TotalLength, TTL=64, Protocol=17, 源IP=本机IP, 目标IP, 计算头部校验和）
3. 将完整 IP 包写入 tx_pool

### 8. select/poll 实现

select:
- 需要实现 fd_set 结构及相关宏（FD_ZERO, FD_SET, FD_CLR, FD_ISSET）
- readset：socket 有数据可读（收包缓冲区非空）
- writeset：UDP 永远可写（始终设置）
- exceptset：暂不实现
- timeout：NULL = 无限等待，timeval = 超时后返回

poll：
- POLLIN：有数据可读
- POLLOUT：始终可写
- timeout：毫秒，-1 = 无限等待，0 = 立即返回

注意裸机无多线程，select/poll 在同一个 tick 驱动上下文中执行：
- 检查当前各 socket 状态
- 如果没有可用的且 timeout > 0，返回 0（不阻塞等待）

### 9. 本机 IP 地址

由于本实现不含 ARP，需要外部提供一个全局变量或配置接口设置本机 IP：

```c
// 由外部在初始化时设置
extern uint32_t g_local_ip_addr;  // 网络字节序
```

也可以在协议栈内部定义一个初始化函数：
```c
void udp_init(uint32_t local_ip_addr);
```

## 文件结构

```
udp_stack.h              — 所有 API 声明（tiny_udp_ 前缀版）、数据结构、宏定义
udp_socket_portable.h    — 宏映射头文件，将无前缀名称映射到 tiny_udp_ 版本
udp_stack.c              — 所有实现
```

## 头文件需要包含的类型定义

因为是裸机环境，没有标准系统头文件，以下是协议栈自身需要定义的类型：

```c
/* 基础类型 */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef int                 int32_t;
typedef unsigned int        size_t;
typedef int                 ssize_t;

/* sockaddr 相关 */
struct sockaddr {
    uint8_t     sa_len;       // 总长度
    uint8_t     sa_family;    // AF_INET
    char        sa_data[14];  // 地址数据
};

struct sockaddr_in {
    uint8_t     sin_len;
    uint8_t     sin_family;   // AF_INET
    uint16_t    sin_port;     // 网络字节序
    uint32_t    sin_addr;     // 网络字节序
    char        sin_zero[8];
};

struct in_addr {
    uint32_t s_addr;
};

/* select 相关 */
#define FD_SETSIZE  64
typedef struct fd_set {
    uint32_t fds_bits[(FD_SETSIZE + 31) / 32];
} fd_set;

#define FD_ZERO(set)       memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set)    ((set)->fds_bits[(fd) >> 5] |= (1 << ((fd) & 31)))
#define FD_CLR(fd, set)    ((set)->fds_bits[(fd) >> 5] &= ~(1 << ((fd) & 31)))
#define FD_ISSET(fd, set)  ((set)->fds_bits[(fd) >> 5] & (1 << ((fd) & 31)))

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* poll 相关 */
struct pollfd {
    int     fd;
    short   events;       // 要监听的事件：POLLIN, POLLOUT
    short   revents;      // 返回的实际事件
};

/* socket 级别和选项 */
#define SOL_SOCKET      1
#define SO_BROADCAST    6
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

/* ioctl 命令 */
#define FIONBIO         0x5421   /* 设置非阻塞 */

/* fcntl 命令和标志 */
#define F_SETFL         4
#define F_GETFL         3
#define O_NONBLOCK      0x800

/* 协议族和 socket 类型 */
#define AF_INET         2
#define SOCK_DGRAM      2
#define IPPROTO_UDP     17

/* 地址族 */
#define INADDR_ANY      0x00000000

/* flag 标志 */
#define MSG_DONTWAIT    0x40

/* poll 事件 */
#define POLLIN          0x001
#define POLLOUT         0x004
#define POLLERR         0x008

/* 错误码 */
#define EAGAIN          11
#define EWOULDBLOCK     EAGAIN
#define EBADF           9
#define EINVAL          22
#define ENOMEM          12

/* IP 协议号 */
#define IP_PROTO_UDP    17

/* 最大 socket 数和缓冲深度 */
#define UDP_SOCKET_MAX      64
#define UDP_SOCKET_BUF_DEPTH  32
#define UDP_PKT_BUF_SIZE   0x600
```

## 代码质量要求

1. 纯 C89/C99，无 C++ 特性
2. 无全局变量以外的动态内存分配
3. 所有关键路径用 assert 或参数检查保护
4. 头文件中完整注释每个 API 的参数和返回值
5. 校验和计算用标准的补码求和算法
6. IP 头校验和必须计算
7. UDP 校验和可以设为 0（在嵌入式场景常见）
8. 充分考虑 MIPS 架构的对齐访问问题（可能有非对齐访问陷阱，使用 memcpy 或逐字节访问）

## 错误处理

所有 API 应返回 -1 表示失败，并设置一个全局 errno：
```c
extern int errno;
```

返回值说明：
- socket：成功返回 fd（0-63），失败返回 -1
- bind：成功返回 0，失败返回 -1
- sendto/send：成功返回发送的字节数，失败返回 -1
- recvfrom/recv：成功返回接收的字节数，失败返回 -1（无数据时设 errno=EAGAIN）
- close：成功返回 0
- select：成功返回就绪的 fd 数量，超时返回 0，失败返回 -1
- poll：成功返回就绪的 fd 数量，超时返回 0，失败返回 -1
