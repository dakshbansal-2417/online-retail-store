/*
 * reports.c - admin sales / revenue / inventory dashboard served by Apache.
 *   /ors-cgi/reports.cgi
 * Protected with HTTP Basic authentication in apache/ors.conf and uses the
 * read-only database account ors_web.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql.h>

#include "cgi_util.h"

static void summary_cards(MYSQL *db)
{
    static const char *sql =
        "SELECT "
        " (SELECT COALESCE(SUM(total_amount),0) FROM orders WHERE status <> 'CANCELLED'),"
        " (SELECT COALESCE(SUM(total_amount),0) FROM orders WHERE status <> 'CANCELLED' AND DATE(order_date) = CURDATE()),"
        " (SELECT COUNT(*) FROM orders),"
        " (SELECT COUNT(*) FROM orders WHERE status IN ('PLACED','CONFIRMED','SHIPPED','OUT_FOR_DELIVERY')),"
        " (SELECT COUNT(*) FROM users WHERE role = 'CUSTOMER'),"
        " (SELECT COALESCE(ROUND(AVG(total_amount),2),0) FROM orders WHERE status <> 'CANCELLED'),"
        " (SELECT COUNT(*) FROM products WHERE is_active = 1 AND stock <= reorder_level),"
        " (SELECT COALESCE(SUM(price * stock),0) FROM products WHERE is_active = 1)";
    static const char *labels[] = {
        "Total revenue (Rs)", "Revenue today (Rs)", "Orders", "Awaiting delivery",
        "Customers", "Average order (Rs)", "Low / out of stock", "Inventory value (Rs)"
    };

    if (mysql_query(db, sql) != 0) {
        cgi_error("Could not load summary.");
        return;
    }
    MYSQL_RES *res = mysql_store_result(db);
    MYSQL_ROW row = res ? mysql_fetch_row(res) : NULL;
    if (row) {
        printf("<div class=\"cards\">\n");
        for (int i = 0; i < 8; i++) {
            printf("<div class=\"card\"><div class=\"k\">%s</div><div class=\"v%s\">",
                   labels[i], (i == 6 && atol(row[i]) > 0) ? " warn" : "");
            html(row[i]);
            printf("</div></div>\n");
        }
        printf("</div>\n");
    }
    if (res)
        mysql_free_result(res);
}

static void daily_chart(MYSQL *db)
{
    printf("<h2>Revenue - last 14 days</h2>\n");
    if (mysql_query(db,
            "SELECT DATE_FORMAT(sale_date, '%d %b'), orders, units, revenue FROM v_daily_sales "
            "WHERE sale_date > CURDATE() - INTERVAL 14 DAY ORDER BY sale_date") != 0) {
        cgi_error("Could not load sales.");
        return;
    }
    MYSQL_RES *res = mysql_store_result(db);
    if (!res)
        return;

    double max = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        if (atof(row[3]) > max)
            max = atof(row[3]);
    mysql_data_seek(res, 0);

    printf("<div class=\"scroll\"><table>\n<tr><th>Date</th><th class=\"n\">Orders</th>"
           "<th class=\"n\">Units</th><th class=\"n\">Revenue (Rs)</th><th style=\"width:45%%\"></th></tr>\n");
    unsigned long long n = 0;
    while ((row = mysql_fetch_row(res))) {
        n++;
        double pct = max > 0 ? atof(row[3]) * 100.0 / max : 0;
        printf("<tr><td>");
        html(row[0]);
        printf("</td><td class=\"n\">%s</td><td class=\"n\">%s</td><td class=\"n\">%s</td>"
               "<td><div class=\"bar\" style=\"width:%.1f%%\"></div></td></tr>\n",
               row[1], row[2], row[3], pct);
    }
    if (n == 0)
        printf("<tr><td colspan=\"5\" class=\"muted\">No sales in this period</td></tr>\n");
    printf("</table></div>\n");
    mysql_free_result(res);
}

int main(void)
{
    cgi_begin("Admin reports", "reports");

    char when[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%d %b %Y, %H:%M", &tm);
    const char *who = getenv("REMOTE_USER");
    printf("<h1>Sales, revenue and inventory</h1>\n<p class=\"muted\">Generated %s", when);
    if (who) {
        printf(" for ");
        html(who);
    }
    printf("</p>\n");

    MYSQL *db = cgi_db_connect();
    if (!db) {
        cgi_error("The store database is not reachable right now.");
        cgi_end();
        return 0;
    }

    summary_cards(db);
    daily_chart(db);

    cgi_query_table(db, "Top 10 products",
        "SELECT name AS Product, category AS Category, units_sold AS 'Units sold', revenue AS 'Revenue (Rs)' "
        "FROM v_product_sales WHERE units_sold > 0 ORDER BY units_sold DESC, revenue DESC LIMIT 10");

    cgi_query_table(db, "Sales by category",
        "SELECT category AS Category, SUM(units_sold) AS 'Units sold', SUM(revenue) AS 'Revenue (Rs)' "
        "FROM v_product_sales GROUP BY category ORDER BY SUM(revenue) DESC");

    cgi_query_table(db, "Products that need restocking",
        "SELECT product_id AS ID, name AS Product, stock AS Stock, reorder_level AS 'Reorder level', "
        "IF(stock = 0, 'OUT OF STOCK', 'LOW') AS Status FROM products "
        "WHERE is_active = 1 AND stock <= reorder_level ORDER BY stock, name");

    cgi_query_table(db, "Orders by status",
        "SELECT status AS Status, COUNT(*) AS Orders, SUM(total_amount) AS 'Value (Rs)' "
        "FROM orders GROUP BY status ORDER BY FIELD(status,'PLACED','CONFIRMED','SHIPPED',"
        "'OUT_FOR_DELIVERY','DELIVERED','CANCELLED')");

    cgi_query_table(db, "Latest 15 orders",
        "SELECT o.order_id AS 'Order', DATE_FORMAT(o.order_date, '%d-%m-%Y %H:%i') AS Placed, "
        "u.username AS Customer, o.total_amount AS 'Total (Rs)', o.status AS Status, "
        "o.payment_status AS Payment FROM orders o JOIN users u ON u.user_id = o.user_id "
        "ORDER BY o.order_date DESC LIMIT 15");

    mysql_close(db);
    cgi_end();
    return 0;
}
