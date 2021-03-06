/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "nc_util.h"

static uint32_t nfree_connq;       /* # free conn q */
static struct conn_tqh free_connq; /* free conn q */

rstatus_t conn_recv(struct conn *conn);
rstatus_t conn_send(struct conn *conn);
rstatus_t conn_close(struct conn *conn);

static struct conn *
_conn_get(void)
{
    struct conn *conn;

    if (!TAILQ_EMPTY(&free_connq)) {
        ASSERT(nfree_connq > 0);

        conn = TAILQ_FIRST(&free_connq);
        nfree_connq--;
        TAILQ_REMOVE(&free_connq, conn, conn_tqe);
    } else {
        conn = nc_alloc(sizeof(*conn));
        if (conn == NULL) {
            return NULL;
        }
    }

    STAILQ_INIT(&conn->recv_queue);
    STAILQ_INIT(&conn->send_queue);
    conn->recv_queue_bytes = 0;
    conn->send_queue_bytes = 0;

    conn->owner = NULL;
    conn->data = NULL;

    conn->fd = -1;

    conn->send_bytes = 0;
    conn->recv_bytes = 0;

    conn->events = 0;
    conn->err = 0;
    conn->recv_active = 0;
    conn->recv_ready = 0;
    conn->send_active = 0;
    conn->send_ready = 0;

    conn->eof = 0;
    conn->done = 0;

    /* for client conn */
    conn->recv = conn_recv;
    conn->send = conn_send;
    conn->close = conn_close;

    return conn;
}

struct conn *
conn_get(void *owner)
{
    struct conn *conn;

    conn = _conn_get();
    if (conn == NULL) {
        return NULL;
    }

    conn->owner = owner;

    log_verb("get conn %p", conn);
    return conn;
}

static void
conn_free(struct conn *conn)
{
    log_verb("free conn %p", conn);
    nc_free(conn);
}

void
conn_put(struct conn *conn)
{
    ASSERT(conn->fd < 0);
    ASSERT(STAILQ_EMPTY(&conn->recv_queue));
    ASSERT(STAILQ_EMPTY(&conn->send_queue));

    log_verb("put conn %p", conn);
    /* TODO: free mbuf here */

    nfree_connq++;
    TAILQ_INSERT_HEAD(&free_connq, conn, conn_tqe);
}

void
conn_init(void)
{
    log_debug("conn size %d", sizeof(struct conn));
    nfree_connq = 0;
    TAILQ_INIT(&free_connq);
}

void
conn_deinit(void)
{
    struct conn *conn, *nconn; /* current and next connection */

    for (conn = TAILQ_FIRST(&free_connq); conn != NULL;
         conn = nconn, nfree_connq--) {
        ASSERT(nfree_connq > 0);
        nconn = TAILQ_NEXT(conn, conn_tqe);
        conn_free(conn);
    }
    ASSERT(nfree_connq == 0);
}

/*
 * TODO: return size_t , but actually we return NC_ERROR
 * */
static ssize_t
conn_recv_buf(struct conn *conn, void *buf, size_t size)
{
    ssize_t n;

    ASSERT(buf != NULL);
    ASSERT(size > 0);
    ASSERT(conn->recv_ready);

    for (;;) {
        n = nc_read(conn->fd, buf, size);

        log_verb("recv on conn:%p, fd:%d got %zd/%zu", conn, conn->fd, n, size);

        if (n > 0) {
            if (n < (ssize_t)size) {
                conn->recv_ready = 0;
            }
            conn->recv_bytes += (size_t)n;
            conn->recv_queue_bytes += (size_t)n;
            return n;
        }

        if (n == 0) {
            conn->recv_ready = 0;
            conn->eof = 1;
            log_info("recv on conn:%p fd:%d eof rb %zu sb %zu", conn, conn->fd,
                      conn->recv_bytes, conn->send_bytes);
            return n;
        }

        if (errno == EINTR) {
            log_verb("recv on conn:%p fd:%d not ready - eintr", conn, conn->fd);
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            conn->recv_ready = 0;
            log_verb("recv on conn:%p fd:%d not ready - eagain", conn, conn->fd);
            return NC_EAGAIN;
        } else {
            conn->recv_ready = 0;
            conn->err = errno;
            log_error("recv on conn:%p fd:%d failed: %s", conn, conn->fd, strerror(errno));
            return NC_ERROR;
        }
    }

    NOT_REACHED();
    return NC_ERROR;
}

static rstatus_t
conn_recv_queue(struct conn *conn)
{
    struct mbuf *mbuf;
    size_t msize;         /* current mbuf size */
    ssize_t n;

    mbuf = STAILQ_LAST(&conn->recv_queue, mbuf, next);
    if (mbuf == NULL || mbuf_full(mbuf)) {
        mbuf = mbuf_get();
        if (mbuf == NULL) {
            return NC_ENOMEM;
        }
        mbuf_insert(&conn->recv_queue, mbuf);
    }

    msize = mbuf_size(mbuf);
    ASSERT(msize > 0);

    n = conn_recv_buf(conn, mbuf->last, msize);
    if (n < 0) {
        if (n == NC_EAGAIN) {
            return NC_OK;
        }
        return NC_ERROR;
    }

    ASSERT((mbuf->last + n) <= mbuf->end);
    mbuf->last += n;
    return NC_OK;
}

rstatus_t
conn_recv(struct conn *conn)
{
    rstatus_t status;
    server_t *srv = conn->owner;

    conn->recv_ready = 1;
    do {
        status = conn_recv_queue(conn);
        if (status != NC_OK) {
            return status;
        }
    } while (conn->recv_ready);

    return srv->recv_done(conn);
}

static ssize_t
conn_send_buf(struct conn *conn, void *buf, size_t size)
{
    ssize_t n;

    ASSERT(buf != NULL);
    ASSERT(size > 0);
    ASSERT(conn->send_ready);

    for (;;) {
        n = nc_write(conn->fd, buf, size);

        log_verb("sendv on fd %d %zd of %zu",
                  conn->fd, n, size);

        if (n > 0) {
            if (n < (ssize_t)size) {
                conn->send_ready = 0;
            }
            conn->send_bytes += (size_t)n;
            return n;
        }

        if (n == 0) {
            log_warn("sendv on fd %d returned zero", conn->fd);
            conn->send_ready = 0;
            return 0;
        }

        if (errno == EINTR) {
            log_verb("sendv on fd %d not ready - eintr", conn->fd);
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            conn->send_ready = 0;
            log_verb("sendv on fd %d not ready - eagain", conn->fd);
            return NC_EAGAIN;
        } else {
            conn->send_ready = 0;
            conn->err = errno;
            log_error("sendv on fd %d failed: %s", conn->fd, strerror(errno));
            return NC_ERROR;
        }
    }

    NOT_REACHED();

    return NC_ERROR;
}

static rstatus_t
conn_send_queue(struct conn *conn)
{
    struct mbuf *mbuf, *nbuf;                   /* current and next mbuf */
    size_t mlen;                                /* current mbuf data length */
    ssize_t n;

    for (mbuf = STAILQ_FIRST(&conn->send_queue); mbuf != NULL; mbuf = nbuf) {
        nbuf = STAILQ_NEXT(mbuf, next);

        if (mbuf_empty(mbuf)) {
            continue;
        }

        mlen = mbuf_length(mbuf);
        n = conn_send_buf(conn, mbuf->pos, mlen);

        if (n < 0) {
            if (n == NC_EAGAIN) {
                return NC_OK;
            }
            return NC_ERROR;
        }

        mbuf->pos += n;
        if (n < mlen) {
            ASSERT(mbuf->pos < mbuf->end);
            return NC_OK;
        }

        ASSERT(mbuf->pos == mbuf->last);

        mbuf_remove(&conn->send_queue, mbuf);
        mbuf_put(mbuf);
    }

    conn->send_ready = 0;
    return NC_OK;
}

rstatus_t
conn_send(struct conn *conn)
{
    rstatus_t status;
    server_t *srv = conn->owner;

    ASSERT(conn->send_active);

    conn->send_ready = 1;
    do {
        status = conn_send_queue(conn);
        if (status != NC_OK) {
            return status;
        }
    } while (conn->send_ready);

    return srv->send_done(conn);
}

rstatus_t
conn_close(struct conn *conn)
{
    rstatus_t status;
    struct mbuf *mbuf, *nbuf;            /* current and next mbuf */

    if (conn->fd < 0) {
        conn_put(conn);
        return NC_OK;
    }

    if (!STAILQ_EMPTY(&conn->recv_queue)) {
        log_warn("close conn %d discard data in send_queue", conn->fd);
        for (mbuf = STAILQ_FIRST(&conn->recv_queue); mbuf != NULL; mbuf = nbuf) {
            nbuf = STAILQ_NEXT(mbuf, next);
            mbuf_remove(&conn->recv_queue, mbuf);
            mbuf_put(mbuf);
        }
    }

    if (!STAILQ_EMPTY(&conn->send_queue)) {
        log_warn("close conn %d discard data in send_queue", conn->fd);
        for (mbuf = STAILQ_FIRST(&conn->send_queue); mbuf != NULL; mbuf = nbuf) {
            nbuf = STAILQ_NEXT(mbuf, next);
            mbuf_remove(&conn->send_queue, mbuf);
            mbuf_put(mbuf);
        }
    }

    status = close(conn->fd);
    if (status < 0) {
        log_error("close c %d failed, ignored: %s", conn->fd, strerror(errno));
    }
    conn->fd = -1;

    conn_put(conn);
    return NC_OK;
}

rstatus_t
conn_add_out(struct conn *conn)
{
    rstatus_t status;
    server_t *srv = conn->owner;

    status = event_add_out(srv->evb, conn);
    if (status != NC_OK) {
        conn->err = errno;
    }
    return status;
}

rstatus_t
conn_del_out(struct conn *conn)
{
    rstatus_t status;
    server_t *srv = conn->owner;

    status = event_del_out(srv->evb, conn);
    if (status != NC_OK) {
        conn->err = errno;
    }
    return status;
}

/*
 * note: we should not call conn_add_out here.
 * because we may call append many times.
 */
rstatus_t
conn_sendq_append(struct conn *conn, char *pos, size_t n)
{
    struct mbuf *mbuf;
    size_t bytes = 0;
    size_t len;

    while (bytes < n) {
        mbuf = STAILQ_LAST(&conn->send_queue, mbuf, next);
        if ((mbuf == NULL) || mbuf_full(mbuf)) {
            mbuf = mbuf_get();
            if (mbuf == NULL) {
                return NC_ENOMEM;
            }
            mbuf_insert(&conn->send_queue, mbuf);
        }

        len = MIN(mbuf_size(mbuf), n - bytes);
        mbuf_copy(mbuf, (uint8_t *)pos + bytes, len);
        bytes += len;
    }

    return NC_OK;
}
