/*
 * db.c - MySQL helpers with printf-style query building.
 *
 * All user-supplied strings must go through db_escape() before being placed
 * in a query, and all numbers are parsed/validated before formatting, which
 * protects against SQL injection.
 */
#include "db.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

MYSQL *db_connect(const ors_config_t *cfg)
{
    MYSQL *db = mysql_init(NULL);
    if (!db) {
        LOG_ERROR("mysql_init failed (out of memory)");
        return NULL;
    }

    unsigned int timeout = 5;
    mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(db, cfg->db_host, cfg->db_user, cfg->db_pass,
                            cfg->db_name, cfg->db_port, NULL, 0)) {
        LOG_ERROR("MySQL connect to %s@%s/%s failed: %s",
                  cfg->db_user, cfg->db_host, cfg->db_name, mysql_error(db));
        mysql_close(db);
        return NULL;
    }
    mysql_autocommit(db, 1);
    return db;
}

char *db_escape(MYSQL *db, const char *in)
{
    size_t len = strlen(in);
    char *out = malloc(len * 2 + 1);
    if (out)
        mysql_real_escape_string(db, out, in, (unsigned long)len);
    return out;
}

static char *vformat(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0)
        return NULL;

    char *buf = malloc((size_t)n + 1);
    if (buf)
        vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

static int run_query(MYSQL *db, const char *fmt, va_list ap)
{
    char *sql = vformat(fmt, ap);
    if (!sql)
        return -1;

    int rc = mysql_real_query(db, sql, (unsigned long)strlen(sql));
    if (rc != 0)
        LOG_ERROR("SQL error %u: %s | %.160s", mysql_errno(db), mysql_error(db), sql);
    free(sql);
    return rc ? -1 : 0;
}

int db_exec(MYSQL *db, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = run_query(db, fmt, ap);
    va_end(ap);
    return rc;
}

MYSQL_RES *db_query(MYSQL *db, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = run_query(db, fmt, ap);
    va_end(ap);
    if (rc != 0)
        return NULL;

    MYSQL_RES *res = mysql_store_result(db);
    if (!res && mysql_field_count(db) != 0)
        LOG_ERROR("mysql_store_result: %s", mysql_error(db));
    return res;
}

long long db_scalar(MYSQL *db, long long dflt, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = run_query(db, fmt, ap);
    va_end(ap);
    if (rc != 0)
        return dflt;

    MYSQL_RES *res = mysql_store_result(db);
    if (!res)
        return dflt;

    long long v = dflt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row)
        v = row[0] ? strtoll(row[0], NULL, 10) : 0;
    mysql_free_result(res);
    return v;
}

int db_begin(MYSQL *db)
{
    return db_exec(db, "START TRANSACTION");
}

int db_commit(MYSQL *db)
{
    return db_exec(db, "COMMIT");
}

void db_rollback(MYSQL *db)
{
    if (mysql_query(db, "ROLLBACK") != 0)
        LOG_ERROR("ROLLBACK failed: %s", mysql_error(db));
}
