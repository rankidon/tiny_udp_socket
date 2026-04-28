# Tiny UDP Socket Protocol Stack

纯 C 实现的轻量级 UDP/IP 协议栈，面向嵌入式裸机 / RTOS 环境（MIPS Loongson 2K1000）。

## 特性

- **零动态内存分配** — 全部使用静态数组，无 `malloc`
- **BSD Socket 风格 API** — `tiny_udp_socket` / `bind` / `sendto` / `recvfrom` / `select` / `poll` ...
- **MIPS 对齐安全** — 逐字节读写，避免非对齐访问陷阱
- **双模式运行** — 支持裸机轮询模式和 RTOS 阻塞模式
- **线程安全** — RTOS 模式下全局互斥锁保护，支持多线程并发收发
- **阻塞 / 超时 / 非阻塞** — 完整的 `SO_RCVTIMEO`、`O_NONBLOCK`、`select`/`poll` 超时
- **纯头文件 RTOS 适配** — 7 个函数即可移植到任意 RTOS

## 资源规格

| 参数 | 值 |
|------|-----|
| 最大 socket 数 | 64 |
| 每 socket 缓冲深度 | 32 包 |
| 包缓冲区大小 | 1536 字节 (0x600) |
| 收发池大小 | 各 256 槽 |
| 代码体积 | ~1400 行 C |

## 文件结构

```
tiny_udp_socket/
├── src/
│   ├── udp_stack.h              # 协议栈主头文件（类型、宏、API 声明）
│   ├── udp_stack.c              # 协议栈实现
│   ├── udp_rtos_port.h          # RTOS 适配层（接口 + 裸机 no-op + 移植参考）
│   ├── udp_rtos_stub.h          # pthread 桩声明（测试用）
│   ├── udp_rtos_stub.c          # pthread 桩实现（测试用）
│   └── udp_socket_portable.h    # 无前缀名称映射（socket → tiny_udp_socket）
├── test/
│   ├── Makefile                 # make baremetal / make rtos / make clean
│   ├── test_udp_stack.c         # 裸机测试（41 项）
│   ├── test_rtos_udp_stack.c    # RTOS 阻塞 / 线程安全测试（5 项）
│   └── TEST_REPORT.md           # 详细测试报告
├── udp_stack_prompt.md          # 设计规格
└── README.md
```

## API 概览

所有公开 API 使用 `tiny_udp_` 前缀。如需使用标准 BSD 名称，包含 `udp_socket_portable.h` 即可（宏映射）。

### 核心 API

```c
void tiny_udp_init(uint32_t local_ip_addr);

int  tiny_udp_socket(int domain, int type, int protocol);
int  tiny_udp_bind(int s, const struct sockaddr *name, socklen_t namelen);
int  tiny_udp_sendto(int s, const void *data, size_t size, int flags,
                     const struct sockaddr *to, socklen_t tolen);
int  tiny_udp_recvfrom(int s, void *mem, size_t len, int flags,
                       struct sockaddr *from, socklen_t *fromlen);
int  tiny_udp_close(int s);
```

### Connect 后简化收发

```c
int  tiny_udp_connect(int s, const struct sockaddr *name, socklen_t namelen);
int  tiny_udp_send(int s, const void *data, size_t size, int flags);
int  tiny_udp_recv(int s, void *mem, size_t len, int flags);
```

### 多路复用

```c
int  tiny_udp_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                     fd_set *exceptset, struct timeval *timeout);
int  tiny_udp_poll(struct pollfd *fds, nfds_t nfds, int timeout);
```

### Socket 选项 / IO 控制

```c
int  tiny_udp_setsockopt(int s, int level, int optname,
                         const void *optval, socklen_t optlen);
int  tiny_udp_getsockopt(int s, int level, int optname,
                         void *optval, socklen_t *optlen);
int  tiny_udp_ioctl(int s, long cmd, void *argp);
int  tiny_udp_fcntl(int s, int cmd, int val);
```

### 地址查询 / 转换

```c
int         tiny_udp_getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int         tiny_udp_getpeername(int s, struct sockaddr *name, socklen_t *namelen);
const char *tiny_udp_inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         tiny_udp_inet_pton(int af, const char *src, void *dst);
```

### 协议栈轮询

```c
void tiny_udp_tick(void);  // 裸机在主循环周期性调用；RTOS 可选
```

## 快速开始

### 裸机模式

```c
#include "udp_stack.h"

// 1. 初始化
tiny_udp_init(0x0100007F);  // 127.0.0.1 网络字节序

// 2. 创建 + 绑定
int fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8080) };
tiny_udp_bind(fd, (struct sockaddr *)&addr, sizeof(addr));

// 3. 主循环
while (1) {
    tiny_udp_tick();  // 处理收包

    uint8_t buf[256];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = tiny_udp_recvfrom(fd, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        // 处理收到的数据
    }
}
```

### RTOS 模式

```c
#define UDP_RTOS
#include "udp_stack.h"

// 提供 udp_mutex_init/lock/unlock 和 udp_sem_init/wait/signal 的 RTOS 实现
// （参考 src/udp_rtos_port.h 末尾的 FreeRTOS / RT-Thread 移植示例）

// 初始化后，recvfrom 将阻塞等待数据，select/poll 支持超时
tiny_udp_init(ip_addr);

int fd = tiny_udp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
// ... bind ...

// 阻塞等待，直到数据到达或超时
struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
tiny_udp_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

uint8_t buf[256];
int n = tiny_udp_recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
// 有数据返回 n>0，超时返回 -1 (errno=EAGAIN)
```

### 编译测试

```bash
cd test

# 裸机测试
make baremetal

# RTOS 测试（需要 pthread）
make rtos

# 清理
make clean
```

## RTOS 移植

实现 7 个函数即可适配到任意 RTOS：

```c
void udp_mutex_init(udp_mutex_t *m);
void udp_mutex_lock(udp_mutex_t *m);
void udp_mutex_unlock(udp_mutex_t *m);

void udp_sem_init(udp_sem_t *s, uint32_t max_count, uint32_t initial_count);
int  udp_sem_wait(udp_sem_t *s, uint32_t timeout_ms);   // 返回 0 成功，-1 超时
void udp_sem_signal(udp_sem_t *s);
```

`udp_sem_wait` 的 `timeout_ms` 约定：
- `0` — 立即返回（非阻塞）
- `UINT32_MAX` — 无限等待
- 其他值 — 等待毫秒数

详见 `src/udp_rtos_port.h` 末尾的 FreeRTOS / RT-Thread 参考实现。

## Socket 状态机

```
CLOSED → OPENED → BOUND ⇄ CONNECTED → CLOSING → CLOSED
```

- `CLOSED` — 初始 / 已回收
- `OPENED` — `socket()` 创建，未绑定端口
- `BOUND` — `bind()` 后，可收发数据
- `CONNECTED` — `connect()` 后，记有远端地址，收发过滤对端
- `CLOSING` — `close()` 标记，下一个 `tick` 回收

## 数据流

```
发送:  sendto() → 构造 IP+UDP 头 → 写入 tx_pool → 外部网卡驱动发送

接收:  外部网卡驱动 → 写入 rx_pool → tick() 解析 IP/UDP 头
       → 按端口分发到 socket 缓冲区 → recvfrom() 读取
```

## 测试覆盖

| 套件 | 项目数 | 覆盖范围 |
|------|--------|----------|
| 裸机 | 41 | socket 生命周期、bind 冲突、状态机守卫、数据流、connect、select、poll、inet_pton/ntop、缓冲区溢出、fcntl/ioctl、多 socket |
| RTOS | 5 | 阻塞接收、SO_RCVTIMEO 超时、select 超时、多线程并发安全、close 唤醒阻塞线程 |

**全部 46 项测试通过。** 详见 `test/TEST_REPORT.md`。

## License

MIT
