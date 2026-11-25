/*
 * net.h - small socket helpers shared by the server, client and load tester.
 */
#ifndef ORS_NET_H
#define ORS_NET_H

#include <stddef.h>
#include "protocol.h"

/* Buffered line reader on top of read(2). */
typedef struct {
    int    fd;
    size_t pos;
    size_t len;
    char   buf[ORS_MAX_LINE];
} net_reader_t;

void net_reader_init(net_reader_t *r, int fd);

/*
 * Read one line (without the trailing "\n" / "\r\n") into out.
 * Returns  1 line read
 *          0 peer closed the connection (EOF)
 *         -1 read error (errno set, e.g. EAGAIN on SO_RCVTIMEO timeout)
 *         -2 line longer than size-1 (line consumed and truncated)
 */
int net_read_line(net_reader_t *r, char *out, size_t size);

/* Write the whole buffer, retrying on partial writes / EINTR. 0 ok, -1 error. */
int net_send_all(int fd, const void *buf, size_t len);

#endif /* ORS_NET_H */
