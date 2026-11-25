/*
 * catalog.c - public product catalogue served by Apache (CGI).
 *   /ors-cgi/catalog.cgi?q=mouse&category=Electronics
 * Uses the read-only database account ors_web.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql.h>

#include "cgi_util.h"

static char *escape_like(MYSQL *db, const char *in)
{
    size_t len = strlen(in);
    char *tmp = malloc(len * 2 + 1);
    char *out = malloc(len * 4 + 1);
    if (!tmp || !out) {
        free(tmp);
        free(out);
        return NULL;
    }
    mysql_real_escape_string(db, tmp, in, (unsigned long)len);
    char *q = out;
    for (const char *p = tmp; *p; p++) {
        if (*p == '%' || *p == '_')
            *q++ = '\\';
        *q++ = *p;
    }
    *q = '\0';
    free(tmp);
    return out;
}

int main(void)
{
    char q[64], cat[64];
    cgi_param("q", q, sizeof q);
    cgi_param("category", cat, sizeof cat);

    cgi_begin("Product catalogue", "catalog");
    printf("<h1>Product catalogue</h1>\n");

    MYSQL *db = cgi_db_connect();
    if (!db) {
        cgi_error("The store database is not reachable right now. Please try again later.");
        cgi_end();
        return 0;
    }

    /* search form with the category list from the database */
    printf("<form method=\"get\"><input name=\"q\" placeholder=\"Search products\" value=\"");
    html(q);
    printf("\"><select name=\"category\"><option value=\"\">All categories</option>");
    if (mysql_query(db, "SELECT DISTINCT category FROM products WHERE is_active = 1 ORDER BY category") == 0) {
        MYSQL_RES *res = mysql_store_result(db);
        MYSQL_ROW row;
        while (res && (row = mysql_fetch_row(res))) {
            printf("<option%s value=\"", !strcmp(row[0], cat) ? " selected" : "");
            html(row[0]);
            printf("\">");
            html(row[0]);
            printf("</option>");
        }
        if (res)
            mysql_free_result(res);
    }
    printf("</select><button type=\"submit\">Search</button></form>\n");

    char *eq = escape_like(db, q);
    char *ec = escape_like(db, cat);
    char sql[1024];
    snprintf(sql, sizeof sql,
             "SELECT name, category, price, stock, reorder_level, COALESCE(description, '') "
             "FROM products WHERE is_active = 1 "
             "AND (name LIKE '%%%s%%' OR description LIKE '%%%s%%') AND category LIKE '%s%s' "
             "ORDER BY category, name",
             eq ? eq : "", eq ? eq : "", ec ? ec : "", (ec && *ec) ? "" : "%");
    free(eq);
    free(ec);

    if (mysql_query(db, sql) != 0) {
        cgi_error("Could not load products.");
    } else {
        MYSQL_RES *res = mysql_store_result(db);
        MYSQL_ROW row;
        unsigned long long n = res ? mysql_num_rows(res) : 0;
        printf("<p class=\"muted\">%llu product(s)</p>\n<div class=\"cards\">\n", n);
        while (res && (row = mysql_fetch_row(res))) {
            long stock = atol(row[3]), reorder = atol(row[4]);
            printf("<div class=\"card\"><div class=\"k\">");
            html(row[1]);
            printf("</div><h3 style=\"margin:4px 0\">");
            html(row[0]);
            printf("</h3><div class=\"price\">Rs ");
            html(row[2]);
            printf("</div><p class=\"muted\" style=\"min-height:42px\">");
            html(row[5]);
            printf("</p>");
            if (stock == 0)
                printf("<span class=\"badge bad\">Out of stock</span>");
            else if (stock <= reorder)
                printf("<span class=\"badge warn\">Only %ld left</span>", stock);
            else
                printf("<span class=\"badge ok\">In stock</span>");
            printf("</div>\n");
        }
        printf("</div>\n<p class=\"muted\">To place an order, use the ORS client: "
               "<code>./bin/ors_client</code></p>\n");
        if (res)
            mysql_free_result(res);
    }

    mysql_close(db);
    cgi_end();
    return 0;
}
