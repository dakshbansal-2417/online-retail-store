/*
 * config.h - key=value configuration file loader (server and CGI programs).
 */
#ifndef ORS_CONFIG_H
#define ORS_CONFIG_H

typedef struct {
    /* database */
    char         db_host[128];
    unsigned int db_port;
    char         db_user[64];
    char         db_pass[128];
    char         db_name[64];

    /* server */
    int  server_port;
    int  max_clients;
    int  idle_timeout;          /* seconds before an idle client is dropped */
    char log_file[256];
    char pid_file[256];

    /* first-run bootstrap admin account */
    char admin_user[64];
    char admin_password[128];
} ors_config_t;

void config_defaults(ors_config_t *cfg);

/* Load "key = value" lines from path on top of the current values.
 * Returns 0 on success, -1 if the file cannot be opened. */
int config_load(const char *path, ors_config_t *cfg);

#endif /* ORS_CONFIG_H */
