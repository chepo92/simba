/**
 * @file socket.c
 *
 * @section License
 * Copyright (C) 2016, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERSOCKTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

/** TCP socket type. */
#define SOCKET_TYPE_STREAM     1

/** UDP socket type. */
#define SOCKET_TYPE_DGRAM      2

/** RAW socket type. */
#define SOCKET_TYPE_RAW        3

#define STATE_IDLE             0
#define STATE_RECVFROM         1
#define STATE_ACCEPT           2
#define STATE_SENDTO           3

#if !defined(ARCH_LINUX)

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/raw.h"

#if defined(ARCH_ESP)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern xSemaphoreHandle thrd_idle_sem;

#endif

struct module_t {
    int initialized;
    struct fs_counter_t udp_rx_bytes;
    struct fs_counter_t udp_tx_bytes;
    struct fs_counter_t tcp_accepts;
    struct fs_counter_t tcp_rx_bytes;
    struct fs_counter_t tcp_tx_bytes;
    struct fs_counter_t raw_rx_bytes;
    struct fs_counter_t raw_tx_bytes;
};

struct send_to_args_t {
    const void *buf_p;
    size_t size;
    int flags;
    const struct inet_addr_t *remote_addr_p;
    struct {
        size_t left;
    } extra;
};

struct recv_from_args_t {
    void *buf_p;
    size_t size;
    int flags;
    struct inet_addr_t *remote_addr_p;
    struct {
        size_t left;
    } extra;
};

struct tcp_accept_args_t {
    struct socket_t *accepted_p;
    struct inet_addr_t *addr_p;
};

static struct module_t module;

static void init(struct socket_t *self_p,
                 int type,
                 void *pcb_p)
{
    /* Channel functions. */
    chan_init(&self_p->base,
              (chan_read_fn_t)socket_read,
              (chan_write_fn_t)socket_write,
              (chan_size_fn_t)socket_size);

    self_p->type = type;
    self_p->pcb_p = pcb_p;
    self_p->cb.state = STATE_IDLE;
    self_p->input.recvfrom.pbuf_p = NULL;
    self_p->input.recvfrom.left = 0;
}

/**
 * Call given callback from the LwIP thread.
 */
static int tcpip_call(struct socket_t *self_p,
                      void (*callback)(void *ctx_p),
                      void *args_p)
{
    self_p->cb.args_p = args_p;
    self_p->cb.thrd_p = thrd_self();

    tcpip_callback_with_block(callback, self_p, 0);

    return (thrd_suspend(NULL));
}

static void resume_thrd(struct thrd_t *thrd_p, int res)
{
    /* Resume the reading thread. */
    sys_lock();
    thrd_resume_isr(thrd_p, res);
    sys_unlock();

#if defined(ARCH_ESP)
    xSemaphoreGive(thrd_idle_sem);
#endif
}

static void resume_if_polled(struct socket_t *socket_p)
{
    int polled;

    /* Resume any polling thread. */
    sys_lock();

    polled = chan_is_polled_isr(&socket_p->base);

    if (polled == 1) {
        thrd_resume_isr(socket_p->base.reader_p, 0);
        socket_p->base.reader_p = NULL;
    }

    sys_unlock();

#if defined(ARCH_ESP)
    if (polled == 1) {
        xSemaphoreGive(thrd_idle_sem);
    }
#endif
}

/*
 * All callback functions below are called from the LwIP-thread. For
 * ESP, this is the FreeRTOS LwIP-thread.
 */

/**
 * Copy data to the reading threads' buffer and resume the thread.
 */
static void udp_recv_from_copy_resume(struct socket_t *socket_p,
                                      struct pbuf *pbuf_p)
{
    struct recv_from_args_t *args_p;
    ssize_t size;

    args_p = socket_p->cb.args_p;
    size = args_p->size;

    if (size > pbuf_p->tot_len) {
        size = pbuf_p->tot_len;
    }

    fs_counter_increment(&module.udp_rx_bytes, size);
    pbuf_copy_partial(pbuf_p, args_p->buf_p, size, 0);
    pbuf_free(pbuf_p);

    if (args_p->remote_addr_p != NULL) {
        *args_p->remote_addr_p = socket_p->input.recvfrom.remote_addr;
    }

    resume_thrd(socket_p->cb.thrd_p, size);
}

/**
 * An UDP packet has been received.
 */
static void on_udp_recv(void *arg_p,
                        struct udp_pcb *pcb_p,
                        struct pbuf *pbuf_p,
                        ip_addr_t *addr_p,
                        uint16_t port)
{
    struct socket_t *socket_p = arg_p;

    /* Discard the packet if there is already a packed waiting. */
    if (socket_p->input.recvfrom.pbuf_p != NULL) {
        pbuf_free(pbuf_p);
        return;
    }

    /* Save the remote address and port. */
    socket_p->input.recvfrom.remote_addr.ip.number = addr_p->addr;
    socket_p->input.recvfrom.remote_addr.port = port;

    /* Copy the data to the receive buffer if there is one. */
    if (socket_p->cb.state == STATE_RECVFROM) {
        socket_p->cb.state = STATE_IDLE;
        socket_p->input.recvfrom.pbuf_p = NULL;
        udp_recv_from_copy_resume(socket_p, pbuf_p);
    } else {
        socket_p->input.recvfrom.pbuf_p = pbuf_p;
        socket_p->input.recvfrom.left = pbuf_p->tot_len;
        resume_if_polled(socket_p);
    }
}

static void udp_open_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    int res;
    void *pcb_p = NULL;

    /* Create and initiate the UDP pcb. */
    res = -1;
    pcb_p = udp_new();

    if (pcb_p != NULL) {
        udp_recv(pcb_p, on_udp_recv, socket_p);
        init(socket_p, SOCKET_TYPE_DGRAM, pcb_p);
        res = 0;
    }

    resume_thrd(socket_p->cb.thrd_p, res);
}

static void udp_close_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;

    udp_recv(socket_p->pcb_p, NULL, NULL);
    udp_remove(socket_p->pcb_p);

    resume_thrd(socket_p->cb.thrd_p, 0);
}

static void udp_bind_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    const struct inet_addr_t *local_addr_p;
    int res;
    ip_addr_t ip;

    local_addr_p = socket_p->cb.args_p;
    ip.addr = local_addr_p->ip.number;
    res = udp_bind(socket_p->pcb_p, &ip, local_addr_p->port);

    resume_thrd(socket_p->cb.thrd_p, res);
}

static void udp_connect_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    const struct inet_addr_t *remote_addr_p;
    int res;
    ip_addr_t ip;

    remote_addr_p = socket_p->cb.args_p;
    ip.addr = remote_addr_p->ip.number;
    res = udp_connect(socket_p->pcb_p, &ip, remote_addr_p->port);

    resume_thrd(socket_p->cb.thrd_p, res);
}

static void udp_send_to_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    struct send_to_args_t *args_p;
    ssize_t res;
    struct pbuf *pbuf_p;
    ip_addr_t ip;

    args_p = socket_p->cb.args_p;

    /* Copy the data to a pbuf.*/
    pbuf_p = pbuf_alloc(PBUF_TRANSPORT, args_p->size, PBUF_RAM);
    res = -1;

    if (pbuf_p != NULL) {
        memcpy(pbuf_p->payload, args_p->buf_p, args_p->size);
        res = args_p->size;

        if (args_p->remote_addr_p != NULL) {
            ip.addr = args_p->remote_addr_p->ip.number;

            if (udp_sendto(socket_p->pcb_p,
                           pbuf_p,
                           &ip,
                           args_p->remote_addr_p->port) != ERR_OK) {
                res = -1;
            }
        } else {
            if (udp_send(socket_p->pcb_p, pbuf_p) != ERR_OK) {
                res = -1;
            }
        }

        pbuf_free(pbuf_p);
    }

    if (res > 0) {
        fs_counter_increment(&module.udp_tx_bytes, args_p->size);
    }

    resume_thrd(socket_p->cb.thrd_p, res);
}

static void udp_recv_from_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    struct pbuf *pbuf_p;

    pbuf_p = socket_p->input.recvfrom.pbuf_p;

    /* Return if no buffer is available. The reading thread is resumed
       when data is received. */
    if (pbuf_p != NULL) {
        socket_p->input.recvfrom.pbuf_p = NULL;
        udp_recv_from_copy_resume(socket_p, pbuf_p);
    } else {
        socket_p->cb.state = STATE_RECVFROM;
    }
}

static ssize_t udp_send_to(struct socket_t *self_p,
                           const void *buf_p,
                           size_t size,
                           int flags,
                           const struct inet_addr_t *remote_addr_p)
{
    struct send_to_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.remote_addr_p = remote_addr_p;

    return (tcpip_call(self_p, udp_send_to_cb, &args));
}

static ssize_t udp_recv_from(struct socket_t *self_p,
                             void *buf_p,
                             size_t size,
                             int flags,
                             struct inet_addr_t *remote_addr_p)
{
    struct recv_from_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.remote_addr_p = remote_addr_p;

    return (tcpip_call(self_p, udp_recv_from_cb, &args));
}

/**
 * Copy data to the reading threads' buffer and resume the thread if
 * all requested data has been read.
 */
static void tcp_recv_buffer(struct socket_t *socket_p)
{
    struct recv_from_args_t *args_p;
    size_t size;
    struct pbuf *pbuf_p;

    pbuf_p = socket_p->input.recvfrom.pbuf_p;
    args_p = socket_p->cb.args_p;

    /* Copy data from pbuf_p to the read buffer. */
    size = MIN(socket_p->input.recvfrom.left, args_p->extra.left);
    pbuf_copy_partial(pbuf_p,
                      args_p->buf_p,
                      size,
                      pbuf_p->tot_len - socket_p->input.recvfrom.left);
    args_p->extra.left -= size;
    args_p->buf_p += size;
    socket_p->input.recvfrom.left -= size;

    /* Free pbuf_p if all data has been read. */
    if (socket_p->input.recvfrom.left == 0) {
        tcp_recved(socket_p->pcb_p, pbuf_p->tot_len);
        pbuf_free(pbuf_p);
        socket_p->input.recvfrom.pbuf_p = NULL;
    }

    /* Resume the reader when the receive buffer is full. */
    if (args_p->extra.left == 0) {
        socket_p->cb.state = STATE_IDLE;
        fs_counter_increment(&module.tcp_rx_bytes, args_p->size);
        resume_thrd(socket_p->cb.thrd_p, args_p->size);
    } else {
        socket_p->cb.state = STATE_RECVFROM;
    }
}

static err_t on_tcp_sent(void *arg_p,
                         struct tcp_pcb *pcb_p,
                         u16_t len)
{
    struct socket_t *socket_p = arg_p;
    struct send_to_args_t *args_p;
    size_t size;

    if (socket_p->cb.state == STATE_SENDTO) {
        args_p = socket_p->cb.args_p;
        size = MIN(args_p->extra.left,
                   tcp_sndbuf(((struct tcp_pcb *)socket_p->pcb_p)));

        if (tcp_write(socket_p->pcb_p,
                      args_p->buf_p,
                      size,
                      TCP_WRITE_FLAG_COPY) == ERR_OK) {
            args_p->buf_p += size;
            args_p->extra.left -= size;

            /* Resume if all data has been written. */
            if (args_p->extra.left == 0) {
                tcp_output(socket_p->pcb_p);
                socket_p->cb.state = STATE_IDLE;
                fs_counter_increment(&module.tcp_tx_bytes, args_p->size);
                resume_thrd(socket_p->cb.thrd_p, args_p->size);
            } else {
                socket_p->cb.state = STATE_SENDTO;
            }
        } else {
            resume_thrd(socket_p->cb.thrd_p, 0);
        }
    }

    return (ERR_OK);
}

/**
 * TCP data is available in the lwip stack.
 */
static err_t on_tcp_recv(void *arg_p,
                         struct tcp_pcb *pcb_p,
                         struct pbuf *pbuf_p,
                         err_t err)
{
    struct socket_t *socket_p = arg_p;

    /* In the process of being accepted. */
    if (socket_p == NULL) {
        return (ERR_MEM);
    }

    /* Ready for the next buffer? */
    if (socket_p->input.recvfrom.pbuf_p != NULL) {
        return (ERR_MEM);
    }

    if (pbuf_p != NULL) {
        socket_p->input.recvfrom.pbuf_p = pbuf_p;
        socket_p->input.recvfrom.left = pbuf_p->tot_len;

        if (socket_p->cb.state == STATE_RECVFROM) {
            tcp_recv_buffer(socket_p);
        } else {
            resume_if_polled(socket_p);
        }
    } else {
        /* Socket closed. */
        socket_p->input.recvfrom.left = -1;

        if (socket_p->cb.state == STATE_RECVFROM) {
            resume_thrd(socket_p->cb.thrd_p, 0);
        } else {
            resume_if_polled(socket_p);
        }
    }

    return (ERR_OK);
}

/**
 * Accept the client and resume accepting thread.
 */
static void tcp_accept_resume(struct socket_t *socket_p)
{
    struct tcp_accept_args_t *args_p;
    struct tcp_pcb *pcb_p;

    fs_counter_increment(&module.tcp_accepts, 1);

    pcb_p = socket_p->input.accept.pcb_p;
    socket_p->input.accept.left = 0;
    socket_p->input.accept.pcb_p = NULL;

    args_p = socket_p->cb.args_p;
    tcp_arg(pcb_p, args_p->accepted_p);
    tcp_recv(pcb_p, on_tcp_recv);
    tcp_sent(pcb_p, on_tcp_sent);
    init(args_p->accepted_p, SOCKET_TYPE_STREAM, pcb_p);
    tcp_accepted(((struct tcp_pcb *)socket_p->pcb_p));
    resume_thrd(socket_p->cb.thrd_p, 0);
}

/**
 * Open a TCP socket from the lwip thread.
 */
static void tcp_open_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    void *pcb_p;

    /* Create and initiate the TCP pcb. */
    pcb_p = tcp_new();
    tcp_arg(pcb_p, socket_p);
    tcp_recv(pcb_p, on_tcp_recv);
    tcp_sent(pcb_p, on_tcp_sent);
    init(socket_p, SOCKET_TYPE_STREAM, pcb_p);

    resume_thrd(socket_p->cb.thrd_p, 0);
}

/**
 * Close a TCP socket from the lwip thread.
 */
static void tcp_close_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;

    tcp_close(socket_p->pcb_p);
    resume_thrd(socket_p->cb.thrd_p, 0);
}

static void tcp_bind_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    const struct inet_addr_t *local_addr_p;
    int res;
    ip_addr_t ip;

    local_addr_p = socket_p->cb.args_p;
    ip.addr = local_addr_p->ip.number;
    res = tcp_bind(socket_p->pcb_p, &ip, local_addr_p->port);

    resume_thrd(socket_p->cb.thrd_p, res);
}

static err_t on_tcp_accept(void *arg_p,
                           struct tcp_pcb *new_pcb_p,
                           err_t err)
{
    struct socket_t *socket_p = arg_p;

    /* Do not accept a new connection if there is one already
       pending. */
    if (socket_p->input.accept.pcb_p != NULL) {
        return (ERR_CONN);
    }

    socket_p->input.accept.left = 1;
    socket_p->input.accept.pcb_p = new_pcb_p;

    tcp_arg(new_pcb_p, NULL);
    tcp_recv(new_pcb_p, on_tcp_recv);

    if (socket_p->cb.state == STATE_ACCEPT) {
        socket_p->cb.state = STATE_IDLE;
        tcp_accept_resume(socket_p);
    } else {
        resume_if_polled(socket_p);
    }

    return (ERR_OK);
}

static void tcp_listen_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    int *backlog_p;

    backlog_p = socket_p->cb.args_p;
    socket_p->pcb_p = tcp_listen_with_backlog(socket_p->pcb_p, *backlog_p);
    socket_p->input.accept.left = 0;
    socket_p->input.accept.pcb_p = NULL;
    tcp_accept(socket_p->pcb_p, on_tcp_accept);

    resume_thrd(socket_p->cb.thrd_p, 0);
}

static void tcp_accept_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;

    /* Thread will be resumed when a connection attempt is made. */
    if (socket_p->input.accept.pcb_p != NULL) {
        tcp_accept_resume(socket_p);
    } else {
        socket_p->cb.state = STATE_ACCEPT;
    }
}

static err_t on_tcp_connected(void *arg_p,
                              struct tcp_pcb *pcb_p,
                              err_t err)
{
    struct socket_t *socket_p = arg_p;

    resume_thrd(socket_p->cb.thrd_p, err);

    return (ERR_OK);
}

static void tcp_connect_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    const struct inet_addr_t *remote_addr_p;
    ip_addr_t ip;

    remote_addr_p = socket_p->cb.args_p;
    ip.addr = remote_addr_p->ip.number;

    if (tcp_connect(socket_p->pcb_p,
                    &ip,
                    remote_addr_p->port,
                    on_tcp_connected) != ERR_OK) {
        resume_thrd(socket_p->cb.thrd_p, -1);
    }
}

static void tcp_recv_from_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;

    if (socket_p->input.recvfrom.left == -1) {
        /* Socket closed. */
        resume_thrd(socket_p->cb.thrd_p, 0);
    } else if (socket_p->input.recvfrom.pbuf_p != NULL) {
        tcp_recv_buffer(socket_p);
    } else {
        socket_p->cb.state = STATE_RECVFROM;
    }
}

static void tcp_send_to_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    struct send_to_args_t *args_p;
    size_t size;

    args_p = socket_p->cb.args_p;
    size = MIN(args_p->extra.left,
               tcp_sndbuf(((struct tcp_pcb *)socket_p->pcb_p)));

    if (tcp_write(socket_p->pcb_p,
                  args_p->buf_p,
                  size,
                  TCP_WRITE_FLAG_COPY) == ERR_OK) {
        args_p->buf_p += size;
        args_p->extra.left -= size;

        /* Resume if all data has been written. Otherwise the sent
           callback will send the rest of the data and resume. */
        if (args_p->extra.left == 0) {
            tcp_output(socket_p->pcb_p);
            fs_counter_increment(&module.tcp_tx_bytes, args_p->size);
            resume_thrd(socket_p->cb.thrd_p, args_p->size);
        } else {
            socket_p->cb.state = STATE_SENDTO;
        }
    } else {
        resume_thrd(socket_p->cb.thrd_p, 0);
    }
}

static ssize_t tcp_send_to(struct socket_t *self_p,
                           const void *buf_p,
                           size_t size,
                           int flags,
                           const struct inet_addr_t *remote_addr_p)
{
    struct send_to_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.extra.left = size;

    return (tcpip_call(self_p, tcp_send_to_cb, &args));
}

static ssize_t tcp_recv_from(struct socket_t *self_p,
                             void *buf_p,
                             size_t size,
                             int flags,
                             struct inet_addr_t *remote_addr_p)
{
    struct recv_from_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.remote_addr_p = remote_addr_p;
    args.extra.left = size;

    return (tcpip_call(self_p, tcp_recv_from_cb, &args));
}

/**
 * Copy data to the reading threads' buffer and resume the thread.
 */
static void raw_recv_from_copy_resume(struct socket_t *socket_p,
                                      struct pbuf *pbuf_p)
{
    struct recv_from_args_t *args_p;
    ssize_t size;

    args_p = socket_p->cb.args_p;
    size = args_p->size;

    if (size > pbuf_p->tot_len) {
        size = pbuf_p->tot_len;
    }

    fs_counter_increment(&module.raw_rx_bytes, size);
    pbuf_copy_partial(pbuf_p, args_p->buf_p, size, 0);
    pbuf_free(pbuf_p);
    *args_p->remote_addr_p = socket_p->input.recvfrom.remote_addr;
    resume_thrd(socket_p->cb.thrd_p, size);
}

/**
 * An RAW packet has been received.
 */
static uint8_t on_raw_recv(void *arg_p,
                           struct raw_pcb *pcb_p,
                           struct pbuf *pbuf_p,
                           ip_addr_t *addr_p)
{
    struct socket_t *socket_p = arg_p;

    /* Discard the packet if there is already a packed waiting. */
    if (socket_p->input.recvfrom.pbuf_p != NULL) {
        pbuf_free(pbuf_p);
        return (1);
    }

    /* Save the remote address and port. */
    socket_p->input.recvfrom.remote_addr.ip.number = addr_p->addr;

    /* Copy the data to the receive buffer if there is one. */
    if (socket_p->cb.state == STATE_RECVFROM) {
        socket_p->cb.state = STATE_IDLE;
        socket_p->input.recvfrom.pbuf_p = NULL;
        raw_recv_from_copy_resume(socket_p, pbuf_p);
    } else {
        socket_p->input.recvfrom.pbuf_p = pbuf_p;
        socket_p->input.recvfrom.left = pbuf_p->tot_len;
        resume_if_polled(socket_p);
    }

    return (1);
}

static void raw_send_to_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    struct send_to_args_t *args_p;
    ssize_t res;
    struct pbuf *pbuf_p;
    ip_addr_t ip;

    args_p = socket_p->cb.args_p;

    /* Allocate a buffer.*/
    pbuf_p = pbuf_alloc(PBUF_IP, args_p->size, PBUF_RAM);
    res = -1;

    if (pbuf_p != NULL) {
        memcpy(pbuf_p->payload, args_p->buf_p, args_p->size);
        ip.addr = args_p->remote_addr_p->ip.number;
        res = raw_sendto(socket_p->pcb_p, pbuf_p, &ip);
        pbuf_free(pbuf_p);
    }

    resume_thrd(socket_p->cb.thrd_p, res);
}

static void raw_recv_from_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    struct pbuf *pbuf_p;

    pbuf_p = socket_p->input.recvfrom.pbuf_p;

    /* Return if no buffer is available. The reading thread is resumed
       when data is received. */
    if (pbuf_p != NULL) {
        socket_p->input.recvfrom.pbuf_p = NULL;
        raw_recv_from_copy_resume(socket_p, pbuf_p);
    } else {
        socket_p->cb.state = STATE_RECVFROM;
    }
}

static void raw_open_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;
    void *pcb_p;

    /* Create and initiate the UDP pcb. */
    pcb_p = raw_new(IP_PROTO_ICMP);

    if (pcb_p != NULL) {
        raw_recv(pcb_p, on_raw_recv, socket_p);
        init(socket_p, SOCKET_TYPE_RAW, pcb_p);
        resume_thrd(socket_p->cb.thrd_p, 0);
    } else {
        resume_thrd(socket_p->cb.thrd_p, -1);
    }
}

static void raw_close_cb(void *ctx_p)
{
    struct socket_t *socket_p = ctx_p;

    raw_recv(socket_p->pcb_p, NULL, NULL);
    raw_remove(socket_p->pcb_p);
    resume_thrd(socket_p->cb.thrd_p, 0);
}

static ssize_t raw_send_to(struct socket_t *self_p,
                           const void *buf_p,
                           size_t size,
                           int flags,
                           const struct inet_addr_t *remote_addr_p)
{
    struct send_to_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.remote_addr_p = remote_addr_p;

    return (tcpip_call(self_p, raw_send_to_cb, &args));
}

static ssize_t raw_recv_from(struct socket_t *self_p,
                             void *buf_p,
                             size_t size,
                             int flags,
                             struct inet_addr_t *remote_addr_p)
{
    struct recv_from_args_t args;

    args.buf_p = buf_p;
    args.size = size;
    args.flags = flags;
    args.remote_addr_p = remote_addr_p;
    args.extra.left = size;

    return (tcpip_call(self_p, raw_recv_from_cb, &args));
}

int socket_module_init(void)
{
    /* Return immediately if the module is already initialized. */
    if (module.initialized == 1) {
        return (0);
    }

    module.initialized = 1;

    /* UDP counters. */
    fs_counter_init(&module.udp_rx_bytes,
                    FSTR("/inet/socket/udp/rx_bytes"),
                    0);
    fs_counter_register(&module.udp_rx_bytes);

    fs_counter_init(&module.udp_tx_bytes,
                    FSTR("/inet/socket/udp/tx_bytes"),
                    0);
    fs_counter_register(&module.udp_tx_bytes);

    /* TCP counters. */
    fs_counter_init(&module.tcp_accepts,
                    FSTR("/inet/socket/tcp/accepts"),
                    0);
    fs_counter_register(&module.tcp_accepts);

    fs_counter_init(&module.tcp_rx_bytes,
                    FSTR("/inet/socket/tcp/rx_bytes"),
                    0);
    fs_counter_register(&module.tcp_rx_bytes);

    fs_counter_init(&module.tcp_tx_bytes,
                    FSTR("/inet/socket/tcp/tx_bytes"),
                    0);
    fs_counter_register(&module.tcp_tx_bytes);

    fs_counter_init(&module.raw_rx_bytes,
                    FSTR("/inet/socket/raw/rx_bytes"),
                    0);
    fs_counter_register(&module.raw_rx_bytes);

    fs_counter_init(&module.raw_tx_bytes,
                    FSTR("/inet/socket/raw/tx_bytes"),
                    0);
    fs_counter_register(&module.raw_tx_bytes);

#if !defined(ARCH_ESP)
    /* Initialize the LwIP stack. */
    tcpip_init(NULL, NULL);
#endif

    return (0);
}

int socket_open_tcp(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    return (tcpip_call(self_p, tcp_open_cb, NULL));
}

int socket_open_udp(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    return (tcpip_call(self_p, udp_open_cb, NULL));
}

int socket_open_raw(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    return (tcpip_call(self_p, raw_open_cb, NULL));
}

int socket_close(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcpip_call(self_p, tcp_close_cb, NULL));

    case SOCKET_TYPE_DGRAM:
        return (tcpip_call(self_p, udp_close_cb, NULL));

    case SOCKET_TYPE_RAW:
        return (tcpip_call(self_p, raw_close_cb, NULL));

    default:
        return (-1);
    }

    return (0);
}

int socket_bind(struct socket_t *self_p,
                const struct inet_addr_t *local_addr_p)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(local_addr_p != NULL, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcpip_call(self_p,
                           tcp_bind_cb,
                           (struct inet_addr_t *)local_addr_p));

    case SOCKET_TYPE_DGRAM:
        return (tcpip_call(self_p,
                           udp_bind_cb,
                           (struct inet_addr_t *)local_addr_p));

    default:
        return (-1);
    }
}

int socket_listen(struct socket_t *self_p, int backlog)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(backlog >= 0, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcpip_call(self_p,
                           tcp_listen_cb,
                           &backlog));

    default:
        return (-1);
    }

    return (0);
}

int socket_connect(struct socket_t *self_p,
                   const struct inet_addr_t *remote_addr_p)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(remote_addr_p != NULL, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcpip_call(self_p,
                           tcp_connect_cb,
                           (struct inet_addr_t *)remote_addr_p));

    case SOCKET_TYPE_DGRAM:
        return (tcpip_call(self_p,
                           udp_connect_cb,
                           (struct inet_addr_t *)remote_addr_p));

    default:
        return (-1);
    }

}

int socket_connect_by_hostname(struct socket_t *self_p,
                               const char *hostname_p,
                               uint16_t port)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(hostname_p != NULL, EINVAL);
    ASSERTN(self_p->type == SOCKET_TYPE_STREAM, EINVAL);

    /* struct inet_ip_addr_t ip2; */
    /* ip_addr_t ip; */

    /* if (inet_aton("216.58.211.142", &ip2) != 0) { */
    /*     return (-1); */
    /* } */

    /* ip.addr = ip2.number; */

    /* if (tcp_connect(self_p->pcb_p, &ip, port, on_tcp_connected) != ERR_OK) { */
    /*     return (-1); */
    /* } */

    /* self_p->io.thrd_p = thrd_self(); */
    /* thrd_suspend(NULL); */

    /* return (self_p->io.size == ERR_OK ? 0 : -1); */
    return (-1);
}

int socket_accept(struct socket_t *self_p,
                  struct socket_t *accepted_p,
                  struct inet_addr_t *addr_p)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(accepted_p != NULL, EINVAL);

    struct tcp_accept_args_t args;

    args.accepted_p = accepted_p;
    args.addr_p = addr_p;

    return (tcpip_call(self_p, tcp_accept_cb, &args));
}

ssize_t socket_sendto(struct socket_t *self_p,
                      const void *buf_p,
                      size_t size,
                      int flags,
                      const struct inet_addr_t *remote_addr_p)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcp_send_to(self_p,
                            buf_p,
                            size,
                            flags,
                            remote_addr_p));

    case SOCKET_TYPE_DGRAM:
        return (udp_send_to(self_p,
                            buf_p,
                            size,
                            flags,
                            remote_addr_p));

    case SOCKET_TYPE_RAW:
        return (raw_send_to(self_p,
                            buf_p,
                            size,
                            flags,
                            remote_addr_p));

    default:
        return (-1);
    }
}

ssize_t socket_recvfrom(struct socket_t *self_p,
                        void *buf_p,
                        size_t size,
                        int flags,
                        struct inet_addr_t *remote_addr_p)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    switch (self_p->type) {

    case SOCKET_TYPE_STREAM:
        return (tcp_recv_from(self_p,
                              buf_p,
                              size,
                              flags,
                              remote_addr_p));

    case SOCKET_TYPE_DGRAM:
        return (udp_recv_from(self_p,
                              buf_p,
                              size,
                              flags,
                              remote_addr_p));

    case SOCKET_TYPE_RAW:
        return (raw_recv_from(self_p,
                              buf_p,
                              size,
                              flags,
                              remote_addr_p));

    default:
        return (-1);
    }
}

ssize_t socket_write(struct socket_t *self_p,
                     const void *buf_p,
                     size_t size)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    return (socket_sendto(self_p, buf_p, size, 0, NULL));
}

ssize_t socket_read(struct socket_t *self_p,
                    void *buf_p,
                    size_t size)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    return (socket_recvfrom(self_p, buf_p, size, 0, NULL));
}

ssize_t socket_size(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    return (self_p->input.common.left != 0);
}

#else

int socket_module_init(void)
{
    return (0);
}

int socket_open_tcp(struct socket_t *self_p)
{
    return (-1);
}

int socket_open_udp(struct socket_t *self_p)
{
    return (-1);
}

int socket_open_raw(struct socket_t *self_p)
{
    return (-1);
}

int socket_close(struct socket_t *self_p)
{
    return (-1);
}

int socket_bind(struct socket_t *self_p,
                const struct inet_addr_t *local_addr_p)
{
    return (-1);
}

int socket_listen(struct socket_t *self_p, int backlog)
{
    return (-1);
}

int socket_connect(struct socket_t *self_p,
                   const struct inet_addr_t *addr_p)
{
    return (-1);
}

int socket_accept(struct socket_t *self_p,
                  struct socket_t *accepted_p,
                  struct inet_addr_t *addr_p)
{
    return (-1);
}

ssize_t socket_sendto(struct socket_t *self_p,
                      const void *buf_p,
                      size_t size,
                      int flags,
                      const struct inet_addr_t *remote_addr_p)
{
    return (-1);
}

ssize_t socket_recvfrom(struct socket_t *self_p,
                        void *buf_p,
                        size_t size,
                        int flags,
                        struct inet_addr_t *remote_addr_p)
{
    return (-1);
}

ssize_t socket_write(struct socket_t *self_p,
                     const void *buf_p,
                     size_t size)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    return (socket_sendto(self_p, buf_p, size, 0, NULL));
}

ssize_t socket_read(struct socket_t *self_p,
                    void *buf_p,
                    size_t size)
{
    ASSERTN(self_p != NULL, EINVAL);
    ASSERTN(buf_p != NULL, EINVAL);
    ASSERTN(size > 0, EINVAL);

    return (socket_recvfrom(self_p, buf_p, size, 0, NULL));
}

ssize_t socket_size(struct socket_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL);

    return (-1);
}

#endif
