/*
 * udp_stack.c — Tiny UDP Socket Protocol Stack 实现
 *
 * 纯 C 实现，针对嵌入式裸机环境。
 * 无动态内存分配，全部使用静态数组。
 * MIPS 架构对齐安全：所有多字节读写通过 memcpy 完成。
 */

#include "udp_stack.h"

/* ================================================================
 * 内部工具：避免非对齐访问
 * ================================================================ */

static uint16_t read_u16(const uint8_t *p)
{
    uint16_t v;
    /* 逐字节读取避免对齐陷阱 */
    v  = (uint16_t)p[0] << 8;
    v |= (uint16_t)p[1];
    return v;
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t v;
    v  = (uint32_t)p[0] << 24;
    v |= (uint32_t)p[1] << 16;
    v |= (uint32_t)p[2] << 8;
    v |= (uint32_t)p[3];
    return v;
}

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* 安全的 memcpy 实现（避免使用标准库） */
static void safe_memcpy(void *dst, const void *src, size_t n)
{
    size_t i;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void safe_memset(void *dst, int c, size_t n)
{
    size_t i;
    uint8_t *d = (uint8_t *)dst;
    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
}

/* ================================================================
 * IP / UDP 头部偏移量常量
 * ================================================================ */

#define IP_HDR_OFF_VERSION_IHL      0
#define IP_HDR_OFF_TOTAL_LEN        2
#define IP_HDR_OFF_ID               4
#define IP_HDR_OFF_FLAGS_FRAG       6
#define IP_HDR_OFF_TTL              8
#define IP_HDR_OFF_PROTOCOL         9
#define IP_HDR_OFF_CHECKSUM         10
#define IP_HDR_OFF_SRC_IP           12
#define IP_HDR_OFF_DST_IP           16
#define IP_HDR_MIN_LEN              20

#define UDP_HDR_OFF_SRC_PORT        0
#define UDP_HDR_OFF_DST_PORT        2
#define UDP_HDR_OFF_LENGTH          4
#define UDP_HDR_OFF_CHECKSUM        6
#define UDP_HDR_LEN                 8

/* ================================================================
 * 全局变量
 * ================================================================ */

/* 本机 IP 地址（网络字节序） */
static uint32_t g_local_ip;

/* 全局错误码 */
int errno;

/* 收发包共享内存池 */
uint8_t rx_pool[RX_POOL_SIZE][RX_PKT_MAXLEN];
uint8_t tx_pool[TX_POOL_SIZE][TX_PKT_MAXLEN];

/* 收发包池索引 */
volatile uint32_t g_rx_write_idx;   /* 外部系统写入 */
volatile uint32_t g_rx_read_idx;    /* 协议栈读取 */
volatile uint32_t g_tx_write_idx;   /* 协议栈写入 */
volatile uint32_t g_tx_read_idx;    /* 外部系统读取 */

/* ================================================================
 * Socket 控制块
 * ================================================================ */

struct udp_socket {
    uint8_t     used;           /* 是否被占用 */
    uint8_t     state;          /* udp_socket_state */
    uint16_t    local_port;     /* bind 的本地端口（网络字节序） */
    uint32_t    peer_ip;        /* connect 后的远端 IP（网络字节序） */
    uint16_t    peer_port;      /* connect 后的远端端口（网络字节序） */
    uint16_t    flags;          /* O_NONBLOCK 等 */
    uint32_t    recv_timeout_ms;/* SO_RCVTIMEO 超时毫秒 */

    /* 收包循环缓冲 */
    uint8_t     pkt_buf[UDP_SOCKET_BUF_DEPTH][UDP_PKT_BUF_SIZE];
    uint16_t    pkt_len[UDP_SOCKET_BUF_DEPTH];
    uint32_t    pkt_src_ip[UDP_SOCKET_BUF_DEPTH];
    uint16_t    pkt_src_port[UDP_SOCKET_BUF_DEPTH];
    uint8_t     pkt_rp;         /* 读指针 */
    uint8_t     pkt_wp;         /* 写指针 */
    uint8_t     pkt_count;      /* 当前缓冲中的包数 */

    udp_sem_t   recv_sem;       /* RTOS 接收信号量（裸机下为 no-op） */
};

static struct udp_socket g_sockets[UDP_SOCKET_MAX];

/* RTOS 同步原语（裸机模式为 no-op） */
static udp_mutex_t g_mutex;     /* 全局协议栈锁 */
static udp_sem_t   g_rx_sem;    /* 全局收包事件（select/poll 超时用） */

/* ================================================================
 * IP 校验和计算
 * ================================================================ */

/**
 * @brief 计算 IP 头部校验和（标准 16 位补码求和）
 * @param hdr  IP 头部数据指针（20 字节）
 * @return 校验和（网络字节序）
 */
static uint16_t ip_checksum(const uint8_t *hdr, int hdr_len)
{
    uint32_t sum = 0;
    int i;

    /* 按 16 位累加 */
    for (i = 0; i < hdr_len; i += 2) {
        sum += read_u16(hdr + i);
    }

    /* 折叠进位 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

/* ================================================================
 * Socket 管理
 * ================================================================ */

/**
 * @brief 检查 fd 是否有效（在数组范围内且已被使用）
 */
static int socket_valid(int fd)
{
    if (fd < 0 || fd >= UDP_SOCKET_MAX) return 0;
    return g_sockets[fd].used;
}

/**
 * @brief 查找空闲 socket 槽位
 * @return fd 索引，-1 表示无空闲
 */
static int socket_alloc(void)
{
    int i;
    for (i = 0; i < UDP_SOCKET_MAX; i++) {
        if (!g_sockets[i].used) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 检查端口是否已被绑定
 * @return 1 = 已被绑定，0 = 未绑定
 */
static int port_bound(uint16_t port)
{
    int i;
    for (i = 0; i < UDP_SOCKET_MAX; i++) {
        if (!g_sockets[i].used) continue;
        if (g_sockets[i].state >= UDP_STATE_BOUND &&
            g_sockets[i].state != UDP_STATE_CLOSING &&
            g_sockets[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 清空 socket 的收包缓冲区
 */
static void socket_flush_buf(struct udp_socket *sk)
{
    sk->pkt_rp = 0;
    sk->pkt_wp = 0;
    sk->pkt_count = 0;
}

/**
 * @brief 向 socket 的收包缓冲区写入一个包
 * @return 0 = 成功，-1 = 缓冲区满
 */
static int socket_enqueue_pkt(struct udp_socket *sk,
                              const uint8_t *data, uint16_t len,
                              uint32_t src_ip, uint16_t src_port)
{
    if (sk->pkt_count >= UDP_SOCKET_BUF_DEPTH) {
        return -1;  /* 缓冲区满，丢包 */
    }
    safe_memcpy(sk->pkt_buf[sk->pkt_wp], data, len);
    sk->pkt_len[sk->pkt_wp]      = len;
    sk->pkt_src_ip[sk->pkt_wp]   = src_ip;
    sk->pkt_src_port[sk->pkt_wp] = src_port;
    sk->pkt_wp = (uint8_t)((sk->pkt_wp + 1) % UDP_SOCKET_BUF_DEPTH);
    sk->pkt_count++;

    /* 唤醒阻塞在此 socket 上的 recvfrom，以及 select/poll */
    udp_sem_signal(&sk->recv_sem);
    udp_sem_signal(&g_rx_sem);

    return 0;
}

/**
 * @brief 从 socket 的收包缓冲区读取一个包
 * @return 实际读取的字节数，0 = 无数据
 */
static uint16_t socket_dequeue_pkt(struct udp_socket *sk,
                                   uint8_t *buf, uint16_t buf_len,
                                   uint32_t *src_ip, uint16_t *src_port,
                                   uint8_t *consume)
{
    uint16_t pkt_len;
    if (sk->pkt_count == 0) {
        return 0;
    }

    pkt_len = sk->pkt_len[sk->pkt_rp];
    if (buf_len < pkt_len) {
        /* 用户缓冲区太小，截断 */
        pkt_len = buf_len;
    }
    safe_memcpy(buf, sk->pkt_buf[sk->pkt_rp], pkt_len);
    *src_ip  = sk->pkt_src_ip[sk->pkt_rp];
    *src_port = sk->pkt_src_port[sk->pkt_rp];
    *consume = 1;
    return pkt_len;
}

/* ================================================================
 * 数据包解析与分发
 * ================================================================ */

/**
 * @brief 解析收到的 IP 包并分发到对应 socket
 */
static void process_rx_packet(const uint8_t *pkt, uint16_t pkt_len)
{
    uint8_t  version_ihl;
    uint8_t  ihl;
    uint16_t total_len;
    uint16_t ip_hdr_len;
    const uint8_t *udp_hdr;
    uint16_t udp_len;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t src_ip;
    const uint8_t *payload;
    uint16_t payload_len;
    int i;

    /* 最小长度检查 */
    if (pkt_len < (IP_HDR_MIN_LEN + UDP_HDR_LEN)) {
        return;
    }

    /* 解析 IP 头 */
    version_ihl = pkt[IP_HDR_OFF_VERSION_IHL];
    /* 只处理 IPv4 */
    if ((version_ihl >> 4) != 4) {
        return;
    }

    ihl = version_ihl & 0x0F;
    ip_hdr_len = (uint16_t)ihl * 4;
    if (ip_hdr_len < IP_HDR_MIN_LEN || ip_hdr_len > pkt_len) {
        return;
    }

    total_len = read_u16(pkt + IP_HDR_OFF_TOTAL_LEN);
    if (total_len > pkt_len) {
        return;
    }

    /* 只处理 UDP */
    if (pkt[IP_HDR_OFF_PROTOCOL] != IP_PROTO_UDP) {
        return;
    }

    src_ip = read_u32(pkt + IP_HDR_OFF_SRC_IP);

    /* 解析 UDP 头 */
    udp_hdr = pkt + ip_hdr_len;
    src_port  = read_u16(udp_hdr + UDP_HDR_OFF_SRC_PORT);
    dst_port  = read_u16(udp_hdr + UDP_HDR_OFF_DST_PORT);
    udp_len   = read_u16(udp_hdr + UDP_HDR_OFF_LENGTH);

    if (udp_len < UDP_HDR_LEN) {
        return;
    }

    payload     = udp_hdr + UDP_HDR_LEN;
    payload_len = udp_len - UDP_HDR_LEN;
    if ((uint16_t)(ip_hdr_len + udp_len) > total_len) {
        return;
    }

    /* 按目标端口分发到已绑定的 socket */
    for (i = 0; i < UDP_SOCKET_MAX; i++) {
        struct udp_socket *sk = &g_sockets[i];
        if (!sk->used) continue;
        if (sk->state < UDP_STATE_BOUND) continue;
        if (sk->state == UDP_STATE_CLOSING) continue;
        if (sk->local_port != dst_port) continue;

        /* CONNECTED 状态只接受来自已 connect 对端的数据 */
        if (sk->state == UDP_STATE_CONNECTED) {
            if (sk->peer_ip != src_ip || sk->peer_port != src_port) {
                continue;
            }
        }

        /* 入队 */
        socket_enqueue_pkt(sk, payload, payload_len, src_ip, src_port);
    }
}

/* ================================================================
 * 数据包构造与发送
 * ================================================================ */

/**
 * @brief 构造并发送一个 UDP 数据包
 * @param src_port  源端口（网络字节序）
 * @param dst_ip    目标 IP（网络字节序）
 * @param dst_port  目标端口（网络字节序）
 * @param data      负载数据
 * @param data_len  负载长度
 * @return 0 = 成功，-1 = 失败
 */
static int build_and_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *data, uint16_t data_len)
{
    uint8_t pkt[TX_PKT_MAXLEN];
    uint16_t ip_total_len;
    uint16_t udp_len;
    uint16_t cksum;
    uint32_t tx_slot;

    ip_total_len = IP_HDR_MIN_LEN + UDP_HDR_LEN + data_len;
    if (ip_total_len > TX_PKT_MAXLEN) {
        return -1;
    }

    udp_len = UDP_HDR_LEN + data_len;

    /* 清零 */
    safe_memset(pkt, 0, IP_HDR_MIN_LEN + UDP_HDR_LEN);

    /* ---- 构造 IP 头 ---- */
    pkt[IP_HDR_OFF_VERSION_IHL] = 0x45;  /* Version=4, IHL=5 */
    /* DSCP+ECN = 0 */
    write_u16(pkt + IP_HDR_OFF_TOTAL_LEN, ip_total_len);
    /* Identification = 0 (暂不维护递增 ID) */
    /* Flags+FragOffset = 0 */
    pkt[IP_HDR_OFF_TTL]      = 64;
    pkt[IP_HDR_OFF_PROTOCOL] = IP_PROTO_UDP;
    /* Header Checksum 先填 0 再计算 */
    write_u32(pkt + IP_HDR_OFF_SRC_IP, g_local_ip);
    write_u32(pkt + IP_HDR_OFF_DST_IP, dst_ip);

    /* 计算 IP 头校验和 */
    cksum = ip_checksum(pkt, IP_HDR_MIN_LEN);
    write_u16(pkt + IP_HDR_OFF_CHECKSUM, cksum);

    /* ---- 构造 UDP 头 ---- */
    write_u16(pkt + IP_HDR_MIN_LEN + UDP_HDR_OFF_SRC_PORT, src_port);
    write_u16(pkt + IP_HDR_MIN_LEN + UDP_HDR_OFF_DST_PORT, dst_port);
    write_u16(pkt + IP_HDR_MIN_LEN + UDP_HDR_OFF_LENGTH,  udp_len);
    /* UDP 校验和填 0（嵌入式场景常见做法） */
    write_u16(pkt + IP_HDR_MIN_LEN + UDP_HDR_OFF_CHECKSUM, 0);

    /* ---- 复制 payload ---- */
    safe_memcpy(pkt + IP_HDR_MIN_LEN + UDP_HDR_LEN, data, data_len);

    /* ---- 写入 tx_pool ---- */
    tx_slot = g_tx_write_idx % TX_POOL_SIZE;
    safe_memcpy(tx_pool[tx_slot], pkt, ip_total_len);
    g_tx_write_idx++;

    return 0;
}

/* ================================================================
 * 公开 API 实现
 * ================================================================ */

void tiny_udp_init(uint32_t local_ip_addr)
{
    int i;
    g_local_ip = local_ip_addr;

    g_rx_write_idx = 0;
    g_rx_read_idx  = 0;
    g_tx_write_idx = 0;
    g_tx_read_idx  = 0;

    safe_memset(rx_pool, 0, sizeof(rx_pool));
    safe_memset(tx_pool, 0, sizeof(tx_pool));
    safe_memset(g_sockets, 0, sizeof(g_sockets));

    for (i = 0; i < UDP_SOCKET_MAX; i++) {
        g_sockets[i].state = UDP_STATE_CLOSED;
        g_sockets[i].used  = 0;
        g_sockets[i].recv_timeout_ms = UDP_RECV_TIMEOUT_DEFAULT;
    }

    udp_mutex_init(&g_mutex);
    udp_sem_init(&g_rx_sem, 256, 0);

    errno = 0;
}

/* ---- 核心 API ---- */

int tiny_udp_socket(int domain, int type, int protocol)
{
    int fd;
    struct udp_socket *sk;

    if (domain != AF_INET || type != SOCK_DGRAM || protocol != IPPROTO_UDP) {
        errno = EINVAL;
        return -1;
    }

    udp_mutex_lock(&g_mutex);

    fd = socket_alloc();
    if (fd < 0) {
        udp_mutex_unlock(&g_mutex);
        errno = ENOMEM;
        return -1;
    }

    sk = &g_sockets[fd];
    safe_memset(sk, 0, sizeof(*sk));
    sk->used  = 1;
    sk->state = UDP_STATE_OPENED;
    sk->flags = 0;
    sk->recv_timeout_ms = UDP_RECV_TIMEOUT_DEFAULT;
    socket_flush_buf(sk);
    udp_sem_init(&sk->recv_sem, UDP_SOCKET_BUF_DEPTH, 0);

    udp_mutex_unlock(&g_mutex);
    return fd;
}

int tiny_udp_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    struct udp_socket *sk;
    const struct sockaddr_in *addr_in;
    uint16_t port;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_OPENED) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    if (!name || namelen < sizeof(struct sockaddr_in)) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    addr_in = (const struct sockaddr_in *)name;
    if (addr_in->sin_family != AF_INET) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    port = addr_in->sin_port;
    if (port_bound(port)) {
        udp_mutex_unlock(&g_mutex);
        errno = EADDRINUSE;
        return -1;
    }

    sk->local_port = port;
    sk->state = UDP_STATE_BOUND;

    udp_mutex_unlock(&g_mutex);
    return 0;
}

int tiny_udp_sendto(int s, const void *data, size_t size, int flags,
                    const struct sockaddr *to, socklen_t tolen)
{
    struct udp_socket *sk;
    const struct sockaddr_in *addr_in;
    uint32_t dst_ip;
    uint16_t dst_port;
    (void)flags;  /* flags 在 sendto 中不影响（UDP 无连接，直接发送） */

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_BOUND && sk->state != UDP_STATE_CONNECTED) {
        udp_mutex_unlock(&g_mutex);
        errno = ENOTCONN;
        return -1;
    }

    if (!data || size == 0) {
        udp_mutex_unlock(&g_mutex);
        return 0;
    }

    if (size > (TX_PKT_MAXLEN - IP_HDR_MIN_LEN - UDP_HDR_LEN)) {
        udp_mutex_unlock(&g_mutex);
        errno = EMSGSIZE;
        return -1;
    }

    if (!to || tolen < sizeof(struct sockaddr_in)) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    addr_in = (const struct sockaddr_in *)to;
    if (addr_in->sin_family != AF_INET) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    dst_ip   = addr_in->sin_addr.s_addr;
    dst_port = addr_in->sin_port;

    if (build_and_send(sk->local_port, dst_ip, dst_port,
                       (const uint8_t *)data, (uint16_t)size) != 0) {
        udp_mutex_unlock(&g_mutex);
        errno = EMSGSIZE;
        return -1;
    }

    udp_mutex_unlock(&g_mutex);
    return (int)size;
}

int tiny_udp_recvfrom(int s, void *mem, size_t len, int flags,
                      struct sockaddr *from, socklen_t *fromlen)
{
    struct udp_socket *sk;
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t  consume;
    uint16_t copied;
    int     ret;
    uint32_t timeout_ms;
    (void)flags;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_BOUND && sk->state != UDP_STATE_CONNECTED) {
        errno = ENOTCONN;
        return -1;
    }

    if (!mem || len == 0) {
        errno = EINVAL;
        return -1;
    }

    /* 决定超时值 */
    timeout_ms = sk->recv_timeout_ms;

    /* 尝试获取数据，无数据时阻塞等待 */
    udp_mutex_lock(&g_mutex);

    while (sk->pkt_count == 0) {
        if (timeout_ms == 0) {
            udp_mutex_unlock(&g_mutex);
            errno = EAGAIN;
            return -1;
        }

        /* 释放锁后阻塞等待信号量 */
        udp_mutex_unlock(&g_mutex);
        ret = udp_sem_wait(&sk->recv_sem, timeout_ms);
        udp_mutex_lock(&g_mutex);

        if (ret < 0) {
            /* 超时 */
            udp_mutex_unlock(&g_mutex);
            errno = EAGAIN;
            return -1;
        }

        /* close 可能会唤醒我们 — 检查 socket 是否仍然有效 */
        if (sk->state == UDP_STATE_CLOSING || sk->state == UDP_STATE_CLOSED) {
            udp_mutex_unlock(&g_mutex);
            errno = EBADF;
            return -1;
        }
        /* 被唤醒后重新检查（可能有其他线程抢先消费了数据） */
    }

    copied = socket_dequeue_pkt(sk, (uint8_t *)mem, (uint16_t)len,
                                &src_ip, &src_port, &consume);

    /* 消费这个包（必须在锁内完成，保证与 udp_tick 不竞争） */
    sk->pkt_rp = (uint8_t)((sk->pkt_rp + 1) % UDP_SOCKET_BUF_DEPTH);
    sk->pkt_count--;

    udp_mutex_unlock(&g_mutex);

    /* 填充发送方地址（锁外完成，仅访问局部变量） */
    if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)from;
        addr_in->sin_family      = AF_INET;
        addr_in->sin_port        = src_port;
        addr_in->sin_addr.s_addr = src_ip;
        addr_in->sin_len         = sizeof(struct sockaddr_in);
        *fromlen                 = sizeof(struct sockaddr_in);
    }

    return (int)copied;
}


int tiny_udp_close(int s)
{
    struct udp_socket *sk;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    sk->state = UDP_STATE_CLOSING;

    /* 唤醒阻塞在此 socket 上的 recvfrom 线程 */
    udp_sem_signal(&sk->recv_sem);

    udp_mutex_unlock(&g_mutex);
    return 0;
}

/* ---- Socket 选项 ---- */

int tiny_udp_setsockopt(int s, int level, int optname,
                        const void *optval, socklen_t optlen)
{
    struct udp_socket *sk;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_BROADCAST:
            if (!optval) {
                udp_mutex_unlock(&g_mutex);
                errno = EINVAL;
                return -1;
            }
            udp_mutex_unlock(&g_mutex);
            return 0;

        case SO_RCVTIMEO:
            if (!optval || optlen < sizeof(struct timeval)) {
                udp_mutex_unlock(&g_mutex);
                errno = EINVAL;
                return -1;
            }
            {
                struct timeval tv;
                safe_memcpy(&tv, optval, sizeof(tv));
                sk->recv_timeout_ms = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                if (sk->recv_timeout_ms == 0 && (tv.tv_sec > 0 || tv.tv_usec > 0)) {
                    sk->recv_timeout_ms = 1;  /* 至少 1ms */
                }
            }
            udp_mutex_unlock(&g_mutex);
            return 0;

        case SO_SNDTIMEO:
            if (!optval || optlen < sizeof(struct timeval)) {
                udp_mutex_unlock(&g_mutex);
                errno = EINVAL;
                return -1;
            }
            udp_mutex_unlock(&g_mutex);
            return 0;

        default:
            udp_mutex_unlock(&g_mutex);
            errno = EOPNOTSUPP;
            return -1;
        }
    }

    udp_mutex_unlock(&g_mutex);
    errno = EOPNOTSUPP;
    return -1;
}

int tiny_udp_getsockopt(int s, int level, int optname,
                        void *optval, socklen_t *optlen)
{
    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_BROADCAST: {
            int val = 0;
            if (!optval || !optlen || *optlen < sizeof(int)) {
                udp_mutex_unlock(&g_mutex);
                errno = EINVAL;
                return -1;
            }
            safe_memcpy(optval, &val, sizeof(int));
            *optlen = sizeof(int);
            udp_mutex_unlock(&g_mutex);
            return 0;
        }
        default:
            udp_mutex_unlock(&g_mutex);
            errno = EOPNOTSUPP;
            return -1;
        }
    }

    udp_mutex_unlock(&g_mutex);
    errno = EOPNOTSUPP;
    return -1;
}

/* ---- connect 后简化收发 ---- */

int tiny_udp_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    struct udp_socket *sk;
    const struct sockaddr_in *addr_in;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_BOUND && sk->state != UDP_STATE_OPENED) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    if (!name || namelen < sizeof(struct sockaddr_in)) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    addr_in = (const struct sockaddr_in *)name;
    if (addr_in->sin_family != AF_INET) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    if (sk->state == UDP_STATE_OPENED) {
        uint16_t ephemeral = 49152;
        while (port_bound(ephemeral) && ephemeral < 65535) {
            ephemeral++;
        }
        if (ephemeral >= 65535) {
            udp_mutex_unlock(&g_mutex);
            errno = EADDRINUSE;
            return -1;
        }
        sk->local_port = ephemeral;
        sk->state = UDP_STATE_BOUND;
    }

    sk->peer_ip   = addr_in->sin_addr.s_addr;
    sk->peer_port = addr_in->sin_port;
    sk->state     = UDP_STATE_CONNECTED;

    udp_mutex_unlock(&g_mutex);
    return 0;
}

int tiny_udp_send(int s, const void *data, size_t size, int flags)
{
    struct udp_socket *sk;
    (void)flags;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_CONNECTED) {
        udp_mutex_unlock(&g_mutex);
        errno = ENOTCONN;
        return -1;
    }

    if (!data || size == 0) {
        udp_mutex_unlock(&g_mutex);
        return 0;
    }

    if (size > (TX_PKT_MAXLEN - IP_HDR_MIN_LEN - UDP_HDR_LEN)) {
        udp_mutex_unlock(&g_mutex);
        errno = EMSGSIZE;
        return -1;
    }

    if (build_and_send(sk->local_port, sk->peer_ip, sk->peer_port,
                       (const uint8_t *)data, (uint16_t)size) != 0) {
        udp_mutex_unlock(&g_mutex);
        errno = EMSGSIZE;
        return -1;
    }

    udp_mutex_unlock(&g_mutex);
    return (int)size;
}

int tiny_udp_recv(int s, void *mem, size_t len, int flags)
{
    struct udp_socket *sk;
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t  consume;
    uint16_t copied;
    int     ret;
    uint32_t timeout_ms;
    (void)flags;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_CONNECTED) {
        errno = ENOTCONN;
        return -1;
    }

    if (!mem || len == 0) {
        errno = EINVAL;
        return -1;
    }

    timeout_ms = sk->recv_timeout_ms;

    udp_mutex_lock(&g_mutex);

    while (sk->pkt_count == 0) {
        if (timeout_ms == 0) {
            udp_mutex_unlock(&g_mutex);
            errno = EAGAIN;
            return -1;
        }

        udp_mutex_unlock(&g_mutex);
        ret = udp_sem_wait(&sk->recv_sem, timeout_ms);
        udp_mutex_lock(&g_mutex);

        if (ret < 0) {
            udp_mutex_unlock(&g_mutex);
            errno = EAGAIN;
            return -1;
        }

        if (sk->state == UDP_STATE_CLOSING || sk->state == UDP_STATE_CLOSED) {
            udp_mutex_unlock(&g_mutex);
            errno = EBADF;
            return -1;
        }
    }

    copied = socket_dequeue_pkt(sk, (uint8_t *)mem, (uint16_t)len,
                                &src_ip, &src_port, &consume);

    sk->pkt_rp = (uint8_t)((sk->pkt_rp + 1) % UDP_SOCKET_BUF_DEPTH);
    sk->pkt_count--;

    udp_mutex_unlock(&g_mutex);
    return (int)copied;
}

/* ---- 地址查询 ---- */

int tiny_udp_getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    struct udp_socket *sk;
    struct sockaddr_in *addr_in;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state < UDP_STATE_BOUND) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    if (!name || !namelen || *namelen < sizeof(struct sockaddr_in)) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    addr_in = (struct sockaddr_in *)name;
    safe_memset(addr_in, 0, sizeof(*addr_in));
    addr_in->sin_len         = sizeof(struct sockaddr_in);
    addr_in->sin_family      = AF_INET;
    addr_in->sin_port        = sk->local_port;
    addr_in->sin_addr.s_addr = g_local_ip;
    *namelen                 = sizeof(struct sockaddr_in);

    udp_mutex_unlock(&g_mutex);
    return 0;
}

int tiny_udp_getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    struct udp_socket *sk;
    struct sockaddr_in *addr_in;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    if (sk->state != UDP_STATE_CONNECTED) {
        udp_mutex_unlock(&g_mutex);
        errno = ENOTCONN;
        return -1;
    }

    if (!name || !namelen || *namelen < sizeof(struct sockaddr_in)) {
        udp_mutex_unlock(&g_mutex);
        errno = EINVAL;
        return -1;
    }

    addr_in = (struct sockaddr_in *)name;
    safe_memset(addr_in, 0, sizeof(*addr_in));
    addr_in->sin_len         = sizeof(struct sockaddr_in);
    addr_in->sin_family      = AF_INET;
    addr_in->sin_port        = sk->peer_port;
    addr_in->sin_addr.s_addr = sk->peer_ip;
    *namelen                 = sizeof(struct sockaddr_in);

    udp_mutex_unlock(&g_mutex);
    return 0;
}

/* ---- 多路复用 ---- */

int tiny_udp_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                    fd_set *exceptset, struct timeval *timeout)
{
    int i;
    int ready = 0;
    uint32_t timeout_ms;
    (void)exceptset;

    /* 计算超时毫秒 */
    if (timeout) {
        timeout_ms = (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    } else {
        timeout_ms = UINT32_MAX;  /* NULL = 无限等待 */
    }

    udp_mutex_lock(&g_mutex);

    for (;;) {
        ready = 0;

        if (readset) {
            fd_set result;
            FD_ZERO(&result);
            for (i = 0; i < maxfdp1 && i < UDP_SOCKET_MAX; i++) {
                if (FD_ISSET(i, readset) && socket_valid(i)) {
                    struct udp_socket *sk = &g_sockets[i];
                    if ((sk->state == UDP_STATE_BOUND ||
                         sk->state == UDP_STATE_CONNECTED) &&
                        sk->pkt_count > 0) {
                        FD_SET(i, &result);
                        ready++;
                    }
                }
            }
            *readset = result;
        }

        if (writeset) {
            fd_set result;
            FD_ZERO(&result);
            for (i = 0; i < maxfdp1 && i < UDP_SOCKET_MAX; i++) {
                if (FD_ISSET(i, writeset) && socket_valid(i)) {
                    struct udp_socket *sk = &g_sockets[i];
                    if (sk->state == UDP_STATE_BOUND ||
                        sk->state == UDP_STATE_CONNECTED) {
                        FD_SET(i, &result);
                        ready++;
                    }
                }
            }
            *writeset = result;
        }

        if (ready > 0 || timeout_ms == 0) {
            break;
        }

        /* 无就绪 fd，等待全局收包事件 */
        {
            int sem_ret;
            udp_mutex_unlock(&g_mutex);
            sem_ret = udp_sem_wait(&g_rx_sem, timeout_ms);
            udp_mutex_lock(&g_mutex);

            if (sem_ret < 0) {
                /* 超时，返回 0 */
                if (readset)  FD_ZERO(readset);
                if (writeset) FD_ZERO(writeset);
                udp_mutex_unlock(&g_mutex);
                return 0;
            }
            /* 被唤醒，重新扫描 fds */
        }
    }

    udp_mutex_unlock(&g_mutex);
    return ready;
}

int tiny_udp_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    nfds_t i;
    int ready = 0;
    uint32_t timeout_ms;

    if (!fds) {
        errno = EINVAL;
        return -1;
    }

    if (timeout < 0) {
        timeout_ms = UINT32_MAX;  /* -1 = 无限等待 */
    } else {
        timeout_ms = (uint32_t)timeout;
    }

    udp_mutex_lock(&g_mutex);

    for (;;) {
        ready = 0;

        for (i = 0; i < nfds; i++) {
            fds[i].revents = 0;

            if (!socket_valid(fds[i].fd)) {
                fds[i].revents |= POLLERR;
                ready++;
                continue;
            }

            {
                struct udp_socket *sk = &g_sockets[fds[i].fd];

                if ((fds[i].events & POLLIN) &&
                    (sk->state == UDP_STATE_BOUND ||
                     sk->state == UDP_STATE_CONNECTED) &&
                    sk->pkt_count > 0) {
                    fds[i].revents |= POLLIN;
                }

                if ((fds[i].events & POLLOUT) &&
                    (sk->state == UDP_STATE_BOUND ||
                     sk->state == UDP_STATE_CONNECTED)) {
                    fds[i].revents |= POLLOUT;
                }
            }

            if (fds[i].revents != 0) {
                ready++;
            }
        }

        if (ready > 0 || timeout_ms == 0) {
            break;
        }

        /* 无就绪 fd，等待全局收包事件 */
        {
            int sem_ret;
            udp_mutex_unlock(&g_mutex);
            sem_ret = udp_sem_wait(&g_rx_sem, timeout_ms);
            udp_mutex_lock(&g_mutex);

            if (sem_ret < 0) {
                /* 超时，清零所有 revents 并返回 0 */
                for (i = 0; i < nfds; i++) {
                    fds[i].revents = 0;
                }
                udp_mutex_unlock(&g_mutex);
                return 0;
            }
        }
    }

    udp_mutex_unlock(&g_mutex);
    return ready;
}

/* ---- IO 控制 ---- */

int tiny_udp_ioctl(int s, long cmd, void *argp)
{
    struct udp_socket *sk;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    switch (cmd) {
    case FIONBIO: {
        int on;
        if (!argp) {
            udp_mutex_unlock(&g_mutex);
            errno = EINVAL;
            return -1;
        }
        {
            uint8_t *p = (uint8_t *)argp;
            on = (int)p[0] | ((int)p[1] << 8) | ((int)p[2] << 16) | ((int)p[3] << 24);
        }
        if (on) {
            sk->flags |= (uint16_t)O_NONBLOCK;
            sk->recv_timeout_ms = 0;  /* 非阻塞 → 0 超时 */
        } else {
            sk->flags &= (uint16_t)(~O_NONBLOCK);
            sk->recv_timeout_ms = UDP_RECV_TIMEOUT_DEFAULT;  /* 恢复默认 */
        }
        udp_mutex_unlock(&g_mutex);
        return 0;
    }
    default:
        udp_mutex_unlock(&g_mutex);
        errno = EOPNOTSUPP;
        return -1;
    }
}

int tiny_udp_fcntl(int s, int cmd, int val)
{
    struct udp_socket *sk;

    if (!socket_valid(s)) {
        errno = EBADF;
        return -1;
    }

    udp_mutex_lock(&g_mutex);
    sk = &g_sockets[s];

    switch (cmd) {
    case F_GETFL: {
        int flags = (int)sk->flags;
        udp_mutex_unlock(&g_mutex);
        return flags;
    }

    case F_SETFL:
        sk->flags = (uint16_t)val;
        if (val & O_NONBLOCK) {
            sk->recv_timeout_ms = 0;
        }
        udp_mutex_unlock(&g_mutex);
        return 0;

    default:
        udp_mutex_unlock(&g_mutex);
        errno = EOPNOTSUPP;
        return -1;
    }
}

/* ---- 地址转换工具 ---- */

const char *tiny_udp_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    uint32_t ip;
    int pos;

    if (af != AF_INET) {
        return NULL;
    }

    if (!src || !dst || size < 16) {
        return NULL;
    }

    safe_memcpy(&ip, src, sizeof(ip));

    pos = 0;
    {
        int i;
        for (i = 0; i < 4; i++) {
            uint8_t octet = (uint8_t)(ip >> (24 - i * 8));
            if (octet >= 100) {
                dst[pos++] = (char)('0' + octet / 100);
                octet %= 100;
                dst[pos++] = (char)('0' + octet / 10);
                octet %= 10;
                dst[pos++] = (char)('0' + octet);
            } else if (octet >= 10) {
                dst[pos++] = (char)('0' + octet / 10);
                octet %= 10;
                dst[pos++] = (char)('0' + octet);
            } else {
                dst[pos++] = (char)('0' + octet);
            }
            if (i < 3) {
                dst[pos++] = '.';
            }
        }
    }
    dst[pos] = '\0';

    return dst;
}

int tiny_udp_inet_pton(int af, const char *src, void *dst)
{
    uint32_t val = 0;
    int octet = 0;
    int count = 0;
    const char *p;

    if (af != AF_INET) {
        return 0;
    }

    if (!src || !dst) {
        return 0;
    }

    p = src;

    while (*p) {
        if (*p >= '0' && *p <= '9') {
            octet = octet * 10 + (*p - '0');
            if (octet > 255) return 0;
        } else if (*p == '.') {
            val = (val << 8) | (uint32_t)octet;
            octet = 0;
            count++;
            if (count > 3) return 0;
        } else {
            return 0;
        }
        p++;
    }
    val = (val << 8) | (uint32_t)octet;
    count++;
    if (count != 4) return 0;

    /* val 已是网络字节序的 uint32_t，直接复制 */
    safe_memcpy(dst, &val, sizeof(val));

    return 1;
}

/* ---- 协议栈 tick ---- */

void tiny_udp_tick(void)
{
    int i;

    udp_mutex_lock(&g_mutex);

    /* 1. 处理收包 */
    while (g_rx_read_idx != g_rx_write_idx) {
        uint32_t slot = g_rx_read_idx % RX_POOL_SIZE;
        uint16_t pkt_len;

        /* 读取包长度（IP 头 Total Length 字段） */
        if (rx_pool[slot][IP_HDR_OFF_VERSION_IHL] >> 4 == 4) {
            pkt_len = read_u16(rx_pool[slot] + IP_HDR_OFF_TOTAL_LEN);
            if (pkt_len > RX_PKT_MAXLEN) {
                pkt_len = RX_PKT_MAXLEN;
            }
            process_rx_packet(rx_pool[slot], pkt_len);
        }

        g_rx_read_idx++;
    }

    /* 2. 处理 CLOSING 状态的 socket 回收 */
    for (i = 0; i < UDP_SOCKET_MAX; i++) {
        struct udp_socket *sk = &g_sockets[i];
        if (sk->used && sk->state == UDP_STATE_CLOSING) {
            socket_flush_buf(sk);
            sk->state = UDP_STATE_CLOSED;
            sk->used  = 0;
            sk->local_port = 0;
            sk->peer_ip    = 0;
            sk->peer_port  = 0;
            sk->flags      = 0;
        }
    }

    udp_mutex_unlock(&g_mutex);
}
