/*
 * handlers.h - per-connection session state and the request dispatcher.
 */
#ifndef ORS_HANDLERS_H
#define ORS_HANDLERS_H

#include <mysql.h>
#include "resp.h"

#define SESSION_POOL_MAX 32

typedef struct {
    int    fd;
    MYSQL *db;                       /* this thread's own DB connection   */
    long   user_id;                  /* 0 = not logged in                 */
    int    is_admin;
    int    quit;
    char   username[64];
    char   peer[64];                 /* "ip:port" of the client           */
    char  *pool[SESSION_POOL_MAX];   /* escaped strings freed per request */
    int    npool;
} session_t;

/* Parse one request line (modified in place) and fill in the response. */
void handle_request(session_t *s, char *line, resp_t *r);

#endif /* ORS_HANDLERS_H */
