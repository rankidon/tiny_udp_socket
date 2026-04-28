/*
 * udp_socket_portable.h — 无前缀兼容层
 *
 * 为习惯标准 BSD socket 名称的用户提供宏映射。
 * 包含此文件后可直接使用 socket()、bind() 等标准名称。
 */

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
