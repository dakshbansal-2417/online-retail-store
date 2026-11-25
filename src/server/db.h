/*
 * db.h - thin wrapper around the MySQL C API (libmysqlclient).
 * Every client thread owns its own MYSQL connection.
 */
#ifndef ORS_DB_H
#define ORS_DB_H

#include <mysql.h>
#include "config.h"

MYSQL *db_connect(const ors_config_t *cfg);

/* Escape a string for use inside '...' in SQL. Returns malloc'd memory. */
char *db_escape(MYSQL *db, const char *in);

/* printf-style statement execution. 0 on success, -1 on error (logged). */
int db_exec(MYSQL *db, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* printf-style SELECT. Returns a stored result (caller frees) or NULL. */
MYSQL_RES *db_query(MYSQL *db, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* First column of first row as integer; dflt on error / no rows. */
long long db_scalar(MYSQL *db, long long dflt, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int  db_begin(MYSQL *db);
int  db_commit(MYSQL *db);
void db_rollback(MYSQL *db);

#endif /* ORS_DB_H */
