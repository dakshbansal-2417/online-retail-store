/*
 * server.h - server-wide statistics shared between client threads.
 * All counters are protected by a mutex inside server.c.
 */
#ifndef ORS_SERVER_H
#define ORS_SERVER_H

#include <time.h>

typedef struct {
    int           active_clients;
    int           max_clients;
    unsigned long total_connections;
    unsigned long rejected_busy;
    unsigned long total_requests;
    unsigned long orders_placed;
    unsigned long failed_logins;
    time_t        started;
    long          pid;
} server_stats_t;

void stats_snapshot(server_stats_t *out);
void stats_request(void);
void stats_order_placed(void);
void stats_failed_login(void);

#endif /* ORS_SERVER_H */
