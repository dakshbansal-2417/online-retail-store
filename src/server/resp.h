/*
 * resp.h - builds a protocol response (status line + body + terminator).
 */
#ifndef ORS_RESP_H
#define ORS_RESP_H

#include <stddef.h>
#include <mysql.h>

typedef struct {
    int    ok;
    char   msg[512];
    char  *body;
    size_t len;
    size_t cap;
} resp_t;

void resp_init(resp_t *r);
void resp_free(resp_t *r);

void resp_ok(resp_t *r, const char *fmt, ...)  __attribute__((format(printf, 2, 3)));
void resp_err(resp_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void resp_heading(resp_t *r, const char *title);
void resp_row(resp_t *r, int n, const char *const *fields);
void resp_rowv(resp_t *r, int n, ...);            /* n const char* arguments */
void resp_result(resp_t *r, MYSQL_RES *res);      /* header row + all rows   */

int  resp_send(int fd, const resp_t *r);

#endif /* ORS_RESP_H */
