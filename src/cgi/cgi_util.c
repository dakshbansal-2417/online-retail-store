/*
 * cgi_util.c - shared code for the CGI programs run by Apache.
 *
 * A CGI program is started by Apache as a new process for every HTTP
 * request: the request arrives in environment variables (QUERY_STRING,
 * REMOTE_USER, ...) and whatever the program writes to stdout is sent back
 * to the browser.
 */
#include "cgi_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

MYSQL *cgi_db_connect(void)
{
    const char *path = getenv("ORS_WEB_CONFIG");
    ors_config_t cfg;
    config_defaults(&cfg);
    snprintf(cfg.db_user, sizeof cfg.db_user, "ors_web");
    if (config_load(path ? path : "/etc/ors/web.conf", &cfg) < 0)
        return NULL;

    MYSQL *db = mysql_init(NULL);
    if (!db)
        return NULL;
    unsigned int timeout = 5;
    mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(db, cfg.db_host, cfg.db_user, cfg.db_pass, cfg.db_name, cfg.db_port, NULL, 0)) {
        fprintf(stderr, "ors cgi: MySQL connect failed: %s\n", mysql_error(db));   /* Apache error log */
        mysql_close(db);
        return NULL;
    }
    return db;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void cgi_param(const char *name, char *buf, size_t size)
{
    buf[0] = '\0';
    const char *qs = getenv("QUERY_STRING");
    if (!qs || size == 0)
        return;

    size_t nlen = strlen(name);
    const char *p = qs;
    while (*p) {
        if (!strncmp(p, name, nlen) && p[nlen] == '=') {
            p += nlen + 1;
            size_t n = 0;
            while (*p && *p != '&' && n + 1 < size) {
                if (*p == '+') {
                    buf[n++] = ' ';
                    p++;
                } else if (*p == '%' && hexval((unsigned char)p[1]) >= 0 && hexval((unsigned char)p[2]) >= 0) {
                    char c = (char)(hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]));
                    if (c != '\0')
                        buf[n++] = c;
                    p += 3;
                } else {
                    buf[n++] = *p++;
                }
            }
            buf[n] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (!p)
            return;
        p++;
    }
}

void html(const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;", stdout); break;
        case '<':  fputs("&lt;", stdout); break;
        case '>':  fputs("&gt;", stdout); break;
        case '"':  fputs("&quot;", stdout); break;
        case '\'': fputs("&#39;", stdout); break;
        default:   putchar(*s);
        }
    }
}

void cgi_begin(const char *title, const char *active)
{
    printf("Content-Type: text/html; charset=utf-8\r\n"
           "Cache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\n\r\n");
    printf("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    html(title);
    printf(" - ORS</title>\n<style>\n"
           ":root{--bg:#f6f7f9;--card:#fff;--ink:#1d2330;--muted:#5d6675;--line:#e1e4ea;"
           "--accent:#1f6feb;--ok:#1a7f37;--warn:#b35900;--bad:#c62828}\n"
           "@media (prefers-color-scheme:dark){:root{--bg:#11151c;--card:#1a2029;--ink:#e6e9ef;"
           "--muted:#9aa4b2;--line:#2c3440;--accent:#58a6ff;--ok:#3fb950;--warn:#d29922;--bad:#f85149}}\n"
           "*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;"
           "background:var(--bg);color:var(--ink)}\n"
           "header{background:var(--card);border-bottom:1px solid var(--line);padding:12px 16px;display:flex;"
           "gap:20px;align-items:center;flex-wrap:wrap}\n"
           "header b{font-size:18px}nav a{color:var(--muted);text-decoration:none;margin-right:14px}"
           "nav a.on{color:var(--accent);font-weight:600}\n"
           "main{max-width:1100px;margin:0 auto;padding:20px 16px}h1{font-size:24px;margin:0 0 16px}"
           "h2{font-size:18px;margin:28px 0 10px}\n"
           ".cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px}\n"
           ".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}\n"
           ".card .k{color:var(--muted);font-size:13px}.card .v{font-size:22px;font-weight:650;"
           "font-variant-numeric:tabular-nums}\n"
           ".scroll{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:10px}\n"
           "table{border-collapse:collapse;width:100%%}th,td{padding:8px 10px;border-bottom:1px solid var(--line);"
           "text-align:left;white-space:nowrap}\n"
           "th{font-size:13px;color:var(--muted);font-weight:600}td.n,th.n{text-align:right;"
           "font-variant-numeric:tabular-nums}\ntr:last-child td{border-bottom:0}\n"
           ".badge{display:inline-block;padding:1px 8px;border-radius:99px;font-size:12px;font-weight:600;"
           "border:1px solid currentColor}\n"
           ".ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.muted{color:var(--muted)}\n"
           ".bar{height:10px;background:var(--accent);border-radius:5px;min-width:2px}\n"
           "form{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px}input,select,button{font:inherit;"
           "padding:7px 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--ink)}\n"
           "button{background:var(--accent);color:#fff;border:0;cursor:pointer}\n"
           ".price{font-size:20px;font-weight:650}.err{background:var(--card);border:1px solid var(--bad);"
           "color:var(--bad);padding:12px;border-radius:10px}\n"
           "footer{color:var(--muted);font-size:13px;text-align:center;padding:24px}\n"
           "</style>\n</head>\n<body>\n<header><b>ORS Online Retail Store</b><nav>"
           "<a href=\"/ors/\"%s>Home</a>"
           "<a href=\"/ors-cgi/catalog.cgi\"%s>Catalogue</a>"
           "<a href=\"/ors-cgi/reports.cgi\"%s>Admin reports</a></nav></header>\n<main>\n",
           "",
           !strcmp(active, "catalog") ? " class=\"on\"" : "",
           !strcmp(active, "reports") ? " class=\"on\"" : "");
}

void cgi_end(void)
{
    printf("</main>\n<footer>Online Retail Store Management System - Linux Systems Programming mini project. "
           "Generated by a C CGI program (MySQL client %s).</footer>\n</body>\n</html>\n",
           mysql_get_client_info());
}

void cgi_error(const char *msg)
{
    printf("<p class=\"err\">");
    html(msg);
    printf("</p>\n");
}

void cgi_table(MYSQL_RES *res)
{
    unsigned int nf = mysql_num_fields(res);
    MYSQL_FIELD *f = mysql_fetch_fields(res);

    printf("<div class=\"scroll\"><table>\n<tr>");
    for (unsigned int i = 0; i < nf; i++) {
        printf("<th%s>", IS_NUM(f[i].type) ? " class=\"n\"" : "");
        html(f[i].name);
        printf("</th>");
    }
    printf("</tr>\n");

    MYSQL_ROW row;
    unsigned long long count = 0;
    while ((row = mysql_fetch_row(res))) {
        count++;
        printf("<tr>");
        for (unsigned int i = 0; i < nf; i++) {
            printf("<td%s>", IS_NUM(f[i].type) ? " class=\"n\"" : "");
            html(row[i] ? row[i] : "-");
            printf("</td>");
        }
        printf("</tr>\n");
    }
    if (count == 0)
        printf("<tr><td colspan=\"%u\" class=\"muted\">No data yet</td></tr>\n", nf);
    printf("</table></div>\n");
}

void cgi_query_table(MYSQL *db, const char *title, const char *sql)
{
    if (title) {
        printf("<h2>");
        html(title);
        printf("</h2>\n");
    }
    if (mysql_query(db, sql) != 0) {
        fprintf(stderr, "ors cgi: query failed: %s\n", mysql_error(db));
        cgi_error("Could not load this section.");
        return;
    }
    MYSQL_RES *res = mysql_store_result(db);
    if (!res) {
        cgi_error("Could not load this section.");
        return;
    }
    cgi_table(res);
    mysql_free_result(res);
}
