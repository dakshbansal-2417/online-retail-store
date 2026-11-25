/*
 * cgi_util.h - helpers for the Apache CGI programs (HTML output, query
 * string parsing, read-only database access).
 */
#ifndef ORS_CGI_UTIL_H
#define ORS_CGI_UTIL_H

#include <stddef.h>
#include <mysql.h>

/* Connect using the file named by $ORS_WEB_CONFIG (default /etc/ors/web.conf). */
MYSQL *cgi_db_connect(void);

/* Copy the URL-decoded value of a query-string parameter into buf ("" if absent). */
void cgi_param(const char *name, char *buf, size_t size);

void html(const char *s);                    /* print HTML-escaped text */
void cgi_begin(const char *title, const char *active);
void cgi_end(void);
void cgi_error(const char *msg);

/* Render a result set as an HTML table; numeric columns are right-aligned. */
void cgi_table(MYSQL_RES *res);

/* Run a query and render it; prints an error box on failure. */
void cgi_query_table(MYSQL *db, const char *title, const char *sql);

#endif /* ORS_CGI_UTIL_H */
