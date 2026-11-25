/*
 * resp.c - response builder. Values are sanitised so they can never break
 * the line-based protocol ('|' and newlines are replaced) and rows that begin
 * with '.' or '#' are escaped with an extra leading '.'.
 */
#include "resp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

void resp_init(resp_t *r)
{
    memset(r, 0, sizeof *r);
    r->ok = 1;
    snprintf(r->msg, sizeof r->msg, "Done");
}

void resp_free(resp_t *r)
{
    free(r->body);
    memset(r, 0, sizeof *r);
}

static void set_msg(resp_t *r, int ok, const char *fmt, va_list ap)
{
    r->ok = ok;
    vsnprintf(r->msg, sizeof r->msg, fmt, ap);
    for (char *p = r->msg; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
}

void resp_ok(resp_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_msg(r, 1, fmt, ap);
    va_end(ap);
}

void resp_err(resp_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_msg(r, 0, fmt, ap);
    va_end(ap);
    r->len = 0;                 /* an error discards any partial body */
    if (r->body)
        r->body[0] = '\0';
}

static int ensure(resp_t *r, size_t extra)
{
    if (r->len + extra + 1 <= r->cap)
        return 0;
    size_t cap = r->cap ? r->cap : 1024;
    while (cap < r->len + extra + 1)
        cap *= 2;
    char *b = realloc(r->body, cap);
    if (!b)
        return -1;
    r->body = b;
    r->cap  = cap;
    return 0;
}

static void append(resp_t *r, const char *s, size_t n)
{
    if (ensure(r, n) < 0)
        return;
    memcpy(r->body + r->len, s, n);
    r->len += n;
    r->body[r->len] = '\0';
}

static void append_clean(resp_t *r, const char *s)
{
    size_t n = strlen(s);
    if (ensure(r, n) < 0)
        return;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '|')
            c = '/';
        else if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        r->body[r->len++] = c;
    }
    r->body[r->len] = '\0';
}

void resp_heading(resp_t *r, const char *title)
{
    append(r, "#", 1);
    append_clean(r, title);
    append(r, "\n", 1);
}

void resp_row(resp_t *r, int n, const char *const *fields)
{
    if (n > 0 && fields[0] && (fields[0][0] == '.' || fields[0][0] == '#'))
        append(r, ".", 1);
    for (int i = 0; i < n; i++) {
        if (i)
            append(r, "|", 1);
        append_clean(r, fields[i] ? fields[i] : "-");
    }
    append(r, "\n", 1);
}

void resp_rowv(resp_t *r, int n, ...)
{
    const char *f[32];
    va_list ap;
    if (n > 32)
        n = 32;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        f[i] = va_arg(ap, const char *);
    va_end(ap);
    resp_row(r, n, f);
}

void resp_result(resp_t *r, MYSQL_RES *res)
{
    unsigned int nf = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    const char **v = malloc(nf * sizeof *v);
    if (!v)
        return;

    for (unsigned int i = 0; i < nf; i++)
        v[i] = fields[i].name;
    resp_row(r, (int)nf, v);

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        for (unsigned int i = 0; i < nf; i++)
            v[i] = row[i];
        resp_row(r, (int)nf, v);
    }
    free(v);
}

int resp_send(int fd, const resp_t *r)
{
    char head[600];
    int hn = snprintf(head, sizeof head, "%s %s\n", r->ok ? "OK" : "ERR", r->msg);
    if (hn < 0)
        return -1;
    if ((size_t)hn >= sizeof head)
        hn = (int)sizeof head - 1;

    if (net_send_all(fd, head, (size_t)hn) < 0)
        return -1;
    if (r->len && net_send_all(fd, r->body, r->len) < 0)
        return -1;
    return net_send_all(fd, ORS_END_LINE "\n", 2);
}
