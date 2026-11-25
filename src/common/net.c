/*
 * net.c - socket helpers: buffered line reading and complete writes.
 */
#include "net.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

void net_reader_init(net_reader_t *r, int fd)
{
    r->fd  = fd;
    r->pos = 0;
    r->len = 0;
}

int net_read_line(net_reader_t *r, char *out, size_t size)
{
    size_t n = 0;
    int overflow = 0;
    int got_any = 0;

    for (;;) {
        if (r->pos == r->len) {
            ssize_t k;
            do {
                k = read(r->fd, r->buf, sizeof r->buf);
            } while (k < 0 && errno == EINTR);

            if (k < 0)
                return -1;
            if (k == 0) {
                if (!got_any)
                    return 0;      /* clean EOF                      */
                break;             /* last line had no trailing '\n' */
            }
            r->pos = 0;
            r->len = (size_t)k;
        }

        char c = r->buf[r->pos++];
        got_any = 1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n + 1 < size)
            out[n++] = c;
        else
            overflow = 1;
    }

    out[n] = '\0';
    return overflow ? -2 : 1;
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        /* MSG_NOSIGNAL: a dead peer gives EPIPE instead of killing us with SIGPIPE */
        ssize_t k = send(fd, p, len, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += k;
        len -= (size_t)k;
    }
    return 0;
}
