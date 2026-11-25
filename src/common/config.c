/*
 * config.c - parse a simple "key = value" configuration file.
 * Lines starting with '#' or ';' are comments.
 */
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SET_STR(field, value) snprintf((field), sizeof(field), "%s", (value))

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/*
 * "wsl-host" means: the Windows machine that hosts this WSL2 VM, i.e. the
 * default gateway. Read it from the kernel routing table /proc/net/route,
 * where addresses are stored as little-endian hex.
 */
static int default_gateway(char *out, size_t size)
{
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp)
        return -1;

    char line[256], iface[64];
    unsigned long dest, gw;
    int rc = -1;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "%63s %lx %lx", iface, &dest, &gw) == 3 && dest == 0 && gw != 0) {
            snprintf(out, size, "%lu.%lu.%lu.%lu",
                     gw & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff, (gw >> 24) & 0xff);
            rc = 0;
            break;
        }
    }
    fclose(fp);
    return rc;
}

void config_defaults(ors_config_t *c)
{
    memset(c, 0, sizeof *c);
    SET_STR(c->db_host, "localhost");
    c->db_port = 3306;
    SET_STR(c->db_user, "ors_app");
    SET_STR(c->db_name, "retail_store");

    c->server_port  = 8080;
    c->max_clients  = 32;
    c->idle_timeout = 900;
    SET_STR(c->log_file, "logs/server.log");
    SET_STR(c->pid_file, "logs/server.pid");

    SET_STR(c->admin_user, "admin");
}

int config_load(const char *path, ors_config_t *c)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[512];
    int lineno = 0;

    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "config %s:%d: missing '=' (ignored)\n", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if      (!strcmp(key, "db_host"))        SET_STR(c->db_host, val);
        else if (!strcmp(key, "db_port"))        c->db_port = (unsigned)strtoul(val, NULL, 10);
        else if (!strcmp(key, "db_user"))        SET_STR(c->db_user, val);
        else if (!strcmp(key, "db_pass"))        SET_STR(c->db_pass, val);
        else if (!strcmp(key, "db_name"))        SET_STR(c->db_name, val);
        else if (!strcmp(key, "server_port"))    c->server_port = atoi(val);
        else if (!strcmp(key, "max_clients"))    c->max_clients = atoi(val);
        else if (!strcmp(key, "idle_timeout"))   c->idle_timeout = atoi(val);
        else if (!strcmp(key, "log_file"))       SET_STR(c->log_file, val);
        else if (!strcmp(key, "pid_file"))       SET_STR(c->pid_file, val);
        else if (!strcmp(key, "admin_user"))     SET_STR(c->admin_user, val);
        else if (!strcmp(key, "admin_password")) SET_STR(c->admin_password, val);
        else
            fprintf(stderr, "config %s:%d: unknown key '%s' (ignored)\n", path, lineno, key);
    }

    fclose(fp);

    if (!strcmp(c->db_host, "wsl-host") && default_gateway(c->db_host, sizeof c->db_host) < 0)
        fprintf(stderr, "config %s: cannot find the Windows host address for db_host = wsl-host\n", path);
    return 0;
}
