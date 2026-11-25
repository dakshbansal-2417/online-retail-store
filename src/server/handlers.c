/*
 * handlers.c - request dispatcher and the business logic of the store:
 * authentication, catalogue, cart, checkout, order tracking, inventory
 * management, customer management and reports.
 *
 * Every handler receives the session, the split request fields
 * (argv[0] = command name) and a response object to fill in.
 */
#include "handlers.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <mysqld_error.h>

#include "auth.h"
#include "db.h"
#include "logger.h"
#include "payment.h"
#include "protocol.h"
#include "server.h"

#define MAX_QTY_PER_ITEM    100
#define MAX_LOGIN_FAILURES  5
#define LOCK_MINUTES        15
#define MAX_STOCK           1000000L

/* MySQL DATE_FORMAT patterns ('%' doubled because queries are printf formats) */
#define DT  "'%%d-%%m-%%Y %%H:%%i'"
#define DAY "'%%d-%%m-%%Y'"

enum { R_ANY, R_LOGIN, R_CUSTOMER, R_ADMIN };

typedef void (*handler_fn)(session_t *s, int argc, char **argv, resp_t *r);

typedef struct {
    const char *name;
    int         min_args;       /* arguments required after the command */
    int         role;
    handler_fn  fn;
    const char *usage;
} command_t;

/* ======================================================================
 *  Helpers
 * ====================================================================== */

static const char *pool_keep(session_t *s, char *p)
{
    if (!p)
        return "";
    if (s->npool >= SESSION_POOL_MAX) {
        LOG_ERROR("escape pool exhausted");
        free(p);
        return "";
    }
    s->pool[s->npool++] = p;
    return p;
}

/* Escape a value for use inside '...' */
static const char *esc(session_t *s, const char *in)
{
    return pool_keep(s, db_escape(s->db, in));
}

/* Escape a value for use inside LIKE '%...%' (also escapes % and _) */
static const char *esc_like(session_t *s, const char *in)
{
    const char *e = esc(s, in);
    char *out = malloc(strlen(e) * 2 + 1);
    if (!out)
        return "";
    char *q = out;
    for (const char *p = e; *p; p++) {
        if (*p == '%' || *p == '_')
            *q++ = '\\';
        *q++ = *p;
    }
    *q = '\0';
    return pool_keep(s, out);
}

static int parse_long(const char *str, long min, long max, long *out)
{
    if (!str || !*str)
        return -1;
    char *end;
    errno = 0;
    long v = strtol(str, &end, 10);
    if (errno || *end || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

/* "1234.5" -> 123450 paise. Up to 2 decimals, max Rs 10 crore. */
static int parse_money(const char *str, long long *paise)
{
    if (!str || !isdigit((unsigned char)*str))
        return -1;
    long long whole = 0;
    int frac = 0, fd = 0;
    const char *p = str;
    while (isdigit((unsigned char)*p)) {
        whole = whole * 10 + (*p - '0');
        if (whole > 100000000LL)
            return -1;
        p++;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p) && fd < 2) {
            frac = frac * 10 + (*p - '0');
            fd++;
            p++;
        }
        if (fd == 0)
            return -1;
    }
    if (*p)
        return -1;
    if (fd == 1)
        frac *= 10;
    *paise = whole * 100 + frac;
    return 0;
}

static void fmt_money(long long paise, char *buf, size_t size)
{
    snprintf(buf, size, "%lld.%02lld", paise / 100, paise % 100);
}

static void str_upper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

static void db_fail(resp_t *r)
{
    resp_err(r, "Database error - please try again");
}

/* Run a SELECT and put the whole result table into the response. */
static int add_table(session_t *s, resp_t *r, const char *heading, const char *sql)
{
    MYSQL_RES *res = db_query(s->db, "%s", sql);
    if (!res)
        return -1;
    if (heading)
        resp_heading(r, heading);
    resp_result(r, res);
    int rows = (int)mysql_num_rows(res);
    mysql_free_result(res);
    return rows;
}

/* ======================================================================
 *  Session / authentication
 * ====================================================================== */

static void h_ping(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)s; (void)argc; (void)argv;
    resp_ok(r, "PONG");
}

static void h_quit(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    s->quit = 1;
    resp_ok(r, "Goodbye");
}

static void h_register(session_t *s, int argc, char **argv, resp_t *r)
{
    const char *user  = argv[1], *pw = argv[2], *name = argv[3], *email = argv[4];
    const char *phone = argc > 5 ? argv[5] : "";
    const char *addr  = argc > 6 ? argv[6] : "";
    char err[128], hash[256];

    if (s->user_id) {
        resp_err(r, "You are already logged in; log out first");
        return;
    }
    if (!auth_valid_username(user)) {
        resp_err(r, "Username must be 3-30 characters: letters, digits, '_' or '.'");
        return;
    }
    if (auth_check_policy(pw, err, sizeof err) < 0) {
        resp_err(r, "%s", err);
        return;
    }
    if (!*name || strlen(name) > 100) {
        resp_err(r, "Full name is required (max 100 characters)");
        return;
    }
    if (!auth_valid_email(email)) {
        resp_err(r, "Please enter a valid e-mail address");
        return;
    }
    if (!auth_valid_phone(phone)) {
        resp_err(r, "Phone number may contain 7-20 digits, spaces, '+' or '-'");
        return;
    }
    if (strlen(addr) > 255) {
        resp_err(r, "Address is too long (max 255 characters)");
        return;
    }

    long long taken = db_scalar(s->db, -1, "SELECT COUNT(*) FROM users WHERE username='%s'", esc(s, user));
    if (taken < 0) { db_fail(r); return; }
    if (taken > 0) {
        resp_err(r, "Username '%s' is already taken", user);
        return;
    }
    if (auth_hash_password(pw, hash, sizeof hash) < 0) {
        resp_err(r, "Internal error while securing password");
        return;
    }
    if (db_exec(s->db,
                "INSERT INTO users (username, password_hash, role, full_name, email, phone, address) "
                "VALUES ('%s','%s','CUSTOMER','%s','%s','%s','%s')",
                esc(s, user), esc(s, hash), esc(s, name), esc(s, email), esc(s, phone), esc(s, addr)) < 0) {
        if (mysql_errno(s->db) == ER_DUP_ENTRY)
            resp_err(r, "Username '%s' is already taken", user);
        else
            db_fail(r);
        return;
    }
    LOG_INFO("New customer registered: %s (id %llu) from %s",
             user, (unsigned long long)mysql_insert_id(s->db), s->peer);
    resp_ok(r, "Registration successful. You can now log in as '%s'.", user);
}

static void h_login(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    const char *user = argv[1], *pw = argv[2];

    if (s->user_id) {
        resp_err(r, "Already logged in as %s", s->username);
        return;
    }

    MYSQL_RES *res = db_query(s->db,
        "SELECT user_id, password_hash, role, full_name, is_active, "
        "       (locked_until IS NOT NULL AND locked_until > NOW()) "
        "FROM users WHERE username='%s'", esc(s, user));
    if (!res) { db_fail(r); return; }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        char tmp[256];
        auth_hash_password(pw, tmp, sizeof tmp);   /* same cost: hide whether user exists */
        mysql_free_result(res);
        stats_failed_login();
        LOG_WARN("Failed login for unknown user '%s' from %s", user, s->peer);
        resp_err(r, "Invalid username or password");
        return;
    }

    long uid = atol(row[0]);
    char hash[256], role[16], name[128];
    snprintf(hash, sizeof hash, "%s", row[1]);
    snprintf(role, sizeof role, "%s", row[2]);
    snprintf(name, sizeof name, "%s", row[3]);
    int active = atoi(row[4]);
    int locked = row[5] ? atoi(row[5]) : 0;
    mysql_free_result(res);

    if (!active) {
        resp_err(r, "This account has been disabled. Please contact the store administrator.");
        return;
    }
    if (locked) {
        resp_err(r, "Account locked after %d failed logins. Try again in %d minutes.",
                 MAX_LOGIN_FAILURES, LOCK_MINUTES);
        return;
    }

    if (!auth_verify_password(pw, hash)) {
        /* after MAX_LOGIN_FAILURES wrong passwords the account is locked
         * for LOCK_MINUTES and the counter starts again */
        db_exec(s->db,
                "UPDATE users SET "
                "  locked_until  = IF(failed_logins + 1 >= %d, NOW() + INTERVAL %d MINUTE, NULL), "
                "  failed_logins = IF(failed_logins + 1 >= %d, 0, failed_logins + 1) "
                "WHERE user_id = %ld",
                MAX_LOGIN_FAILURES, LOCK_MINUTES, MAX_LOGIN_FAILURES, uid);
        stats_failed_login();
        LOG_WARN("Failed login for '%s' from %s", user, s->peer);
        resp_err(r, "Invalid username or password");
        return;
    }

    db_exec(s->db, "UPDATE users SET failed_logins = 0, locked_until = NULL, last_login = NOW() "
                   "WHERE user_id = %ld", uid);

    s->user_id  = uid;
    s->is_admin = (strcmp(role, "ADMIN") == 0);
    snprintf(s->username, sizeof s->username, "%s", user);
    LOG_INFO("User '%s' (%s) logged in from %s", user, role, s->peer);

    resp_ok(r, "Welcome, %s!", name);
    char idbuf[24];
    snprintf(idbuf, sizeof idbuf, "%ld", uid);
    resp_rowv(r, 4, "user_id", "username", "role", "full_name");
    resp_rowv(r, 4, idbuf, user, role, name);
}

static void h_logout(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    LOG_INFO("User '%s' logged out", s->username);
    s->user_id  = 0;
    s->is_admin = 0;
    s->username[0] = '\0';
    resp_ok(r, "Logged out");
}

static void h_profile(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT username AS Username, full_name AS Name, email AS Email, "
             "COALESCE(phone,'') AS Phone, COALESCE(address,'') AS Address, role AS Role, "
             "DATE_FORMAT(created_at, '%%d-%%m-%%Y') AS 'Member since' "
             "FROM users WHERE user_id = %ld", s->user_id);
    resp_ok(r, "Your profile");
    if (add_table(s, r, NULL, sql) < 0)
        db_fail(r);
}

static void h_update_profile(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    char field[32];
    const char *value = argv[2];
    snprintf(field, sizeof field, "%s", argv[1]);
    for (char *p = field; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (!strcmp(field, "full_name")) {
        if (!*value || strlen(value) > 100) { resp_err(r, "Name must be 1-100 characters"); return; }
    } else if (!strcmp(field, "email")) {
        if (!auth_valid_email(value)) { resp_err(r, "Please enter a valid e-mail address"); return; }
    } else if (!strcmp(field, "phone")) {
        if (!auth_valid_phone(value)) { resp_err(r, "Phone number may contain 7-20 digits, spaces, '+' or '-'"); return; }
    } else if (!strcmp(field, "address")) {
        if (!*value || strlen(value) > 255) { resp_err(r, "Address must be 1-255 characters"); return; }
    } else {
        resp_err(r, "Field must be one of: full_name, email, phone, address");
        return;
    }

    /* field comes from the whitelist above, value is escaped */
    if (db_exec(s->db, "UPDATE users SET %s = '%s' WHERE user_id = %ld",
                field, esc(s, value), s->user_id) < 0) {
        db_fail(r);
        return;
    }
    resp_ok(r, "Profile updated (%s)", field);
}

static void h_change_password(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    const char *oldpw = argv[1], *newpw = argv[2];
    char err[128], hash[256];

    MYSQL_RES *res = db_query(s->db, "SELECT password_hash FROM users WHERE user_id = %ld", s->user_id);
    if (!res) { db_fail(r); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    int ok = row && auth_verify_password(oldpw, row[0]);
    mysql_free_result(res);

    if (!ok) {
        resp_err(r, "Current password is incorrect");
        return;
    }
    if (auth_check_policy(newpw, err, sizeof err) < 0) {
        resp_err(r, "%s", err);
        return;
    }
    if (!strcmp(oldpw, newpw)) {
        resp_err(r, "New password must be different from the current one");
        return;
    }
    if (auth_hash_password(newpw, hash, sizeof hash) < 0 ||
        db_exec(s->db, "UPDATE users SET password_hash = '%s' WHERE user_id = %ld",
                esc(s, hash), s->user_id) < 0) {
        db_fail(r);
        return;
    }
    LOG_INFO("User '%s' changed password", s->username);
    resp_ok(r, "Password changed successfully");
}

/* ======================================================================
 *  Product catalogue (customers, guests and admins)
 * ====================================================================== */

static void h_list_products(session_t *s, int argc, char **argv, resp_t *r)
{
    const char *search = argc > 1 ? argv[1] : "";
    char filter[512] = "";

    if (strlen(search) > 50) {
        resp_err(r, "Search text is too long (max 50 characters)");
        return;
    }
    if (*search) {
        const char *e = esc_like(s, search);
        snprintf(filter, sizeof filter,
                 " AND (name LIKE '%%%s%%' OR category LIKE '%%%s%%' OR description LIKE '%%%s%%')",
                 e, e, e);
    }

    MYSQL_RES *res;
    if (s->is_admin) {
        res = db_query(s->db,
            "SELECT product_id AS ID, name AS Name, category AS Category, price AS 'Price (Rs)', "
            "stock AS Stock, reorder_level AS Reorder, "
            "CASE WHEN is_active = 0 THEN 'DELETED' WHEN stock = 0 THEN 'OUT' "
            "     WHEN stock <= reorder_level THEN 'LOW' ELSE 'OK' END AS Status "
            "FROM products WHERE 1 = 1%s ORDER BY is_active DESC, category, name", filter);
    } else {
        res = db_query(s->db,
            "SELECT product_id AS ID, name AS Name, category AS Category, price AS 'Price (Rs)', "
            "CASE WHEN stock = 0 THEN 'Out of stock' "
            "     WHEN stock <= reorder_level THEN CONCAT('Only ', stock, ' left') "
            "     ELSE 'In stock' END AS Availability "
            "FROM products WHERE is_active = 1%s ORDER BY category, name", filter);
    }
    if (!res) { db_fail(r); return; }

    unsigned long long n = mysql_num_rows(res);
    if (*search)
        resp_ok(r, "%llu product(s) matching '%s'", n, search);
    else
        resp_ok(r, "%llu product(s)", n);
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_view_product(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) {
        resp_err(r, "Invalid product id");
        return;
    }
    MYSQL_RES *res = db_query(s->db,
        "SELECT product_id AS ID, name AS Name, category AS Category, price AS 'Price (Rs)', "
        "stock AS 'In stock', COALESCE(description,'') AS Description "
        "FROM products WHERE product_id = %ld%s",
        pid, s->is_admin ? "" : " AND is_active = 1");
    if (!res) { db_fail(r); return; }
    if (mysql_num_rows(res) == 0) {
        mysql_free_result(res);
        resp_err(r, "Product %ld not found", pid);
        return;
    }
    resp_ok(r, "Product details");
    resp_result(r, res);
    mysql_free_result(res);
}

/* ======================================================================
 *  Shopping cart
 * ====================================================================== */

static void h_cart_add(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid, qty;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    if (parse_long(argv[2], 1, MAX_QTY_PER_ITEM, &qty) < 0) {
        resp_err(r, "Quantity must be between 1 and %d", MAX_QTY_PER_ITEM);
        return;
    }

    MYSQL_RES *res = db_query(s->db,
        "SELECT p.stock, p.name, COALESCE(c.quantity, 0) "
        "FROM products p LEFT JOIN cart_items c ON c.product_id = p.product_id AND c.user_id = %ld "
        "WHERE p.product_id = %ld AND p.is_active = 1", s->user_id, pid);
    if (!res) { db_fail(r); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        resp_err(r, "Product %ld not found", pid);
        return;
    }
    long stock = atol(row[0]), in_cart = atol(row[2]);
    char name[128];
    snprintf(name, sizeof name, "%s", row[1]);
    mysql_free_result(res);

    if (in_cart + qty > MAX_QTY_PER_ITEM) {
        resp_err(r, "You can buy at most %d units of one product", MAX_QTY_PER_ITEM);
        return;
    }
    if (in_cart + qty > stock) {
        resp_err(r, "Only %ld unit(s) of '%s' available (you already have %ld in your cart)",
                 stock, name, in_cart);
        return;
    }
    if (db_exec(s->db,
                "INSERT INTO cart_items (user_id, product_id, quantity) VALUES (%ld, %ld, %ld) "
                "ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)",
                s->user_id, pid, qty) < 0) {
        db_fail(r);
        return;
    }
    resp_ok(r, "Added %ld x '%s' to your cart", qty, name);
}

static void h_cart_update(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid, qty;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    if (parse_long(argv[2], 0, MAX_QTY_PER_ITEM, &qty) < 0) {
        resp_err(r, "Quantity must be between 0 and %d (0 removes the item)", MAX_QTY_PER_ITEM);
        return;
    }

    MYSQL_RES *res = db_query(s->db,
        "SELECT p.stock, p.name FROM cart_items c JOIN products p ON p.product_id = c.product_id "
        "WHERE c.user_id = %ld AND c.product_id = %ld", s->user_id, pid);
    if (!res) { db_fail(r); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        resp_err(r, "Product %ld is not in your cart", pid);
        return;
    }
    long stock = atol(row[0]);
    char name[128];
    snprintf(name, sizeof name, "%s", row[1]);
    mysql_free_result(res);

    if (qty == 0) {
        if (db_exec(s->db, "DELETE FROM cart_items WHERE user_id = %ld AND product_id = %ld",
                    s->user_id, pid) < 0) { db_fail(r); return; }
        resp_ok(r, "Removed '%s' from your cart", name);
        return;
    }
    if (qty > stock) {
        resp_err(r, "Only %ld unit(s) of '%s' available", stock, name);
        return;
    }
    if (db_exec(s->db, "UPDATE cart_items SET quantity = %ld WHERE user_id = %ld AND product_id = %ld",
                qty, s->user_id, pid) < 0) { db_fail(r); return; }
    resp_ok(r, "Quantity of '%s' set to %ld", name, qty);
}

static void h_cart_remove(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    if (db_exec(s->db, "DELETE FROM cart_items WHERE user_id = %ld AND product_id = %ld",
                s->user_id, pid) < 0) { db_fail(r); return; }
    if (mysql_affected_rows(s->db) == 0)
        resp_err(r, "Product %ld is not in your cart", pid);
    else
        resp_ok(r, "Item removed from your cart");
}

static void h_cart_clear(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    if (db_exec(s->db, "DELETE FROM cart_items WHERE user_id = %ld", s->user_id) < 0) {
        db_fail(r);
        return;
    }
    resp_ok(r, "Cart cleared");
}

static void h_cart_view(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    MYSQL_RES *res = db_query(s->db,
        "SELECT p.product_id AS ID, p.name AS Product, p.price AS 'Price (Rs)', c.quantity AS Qty, "
        "p.price * c.quantity AS 'Subtotal (Rs)', "
        "CASE WHEN p.is_active = 0 THEN 'No longer sold' "
        "     WHEN p.stock >= c.quantity THEN 'Available' "
        "     ELSE CONCAT('Only ', p.stock, ' left') END AS Availability "
        "FROM cart_items c JOIN products p ON p.product_id = c.product_id "
        "WHERE c.user_id = %ld ORDER BY c.added_at", s->user_id);
    if (!res) { db_fail(r); return; }

    unsigned long long n = mysql_num_rows(res);
    if (n == 0) {
        mysql_free_result(res);
        resp_ok(r, "Your cart is empty");
        return;
    }
    long long total = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        long long sub;
        if (parse_money(row[4], &sub) == 0)
            total += sub;
    }
    mysql_data_seek(res, 0);

    char tbuf[32];
    fmt_money(total, tbuf, sizeof tbuf);
    resp_ok(r, "Cart: %llu item(s), total Rs %s", n, tbuf);
    resp_result(r, res);
    mysql_free_result(res);
}

/* ======================================================================
 *  Checkout - real-time inventory update + payment in one transaction
 * ====================================================================== */

typedef struct {
    long      pid;
    long      qty;
    long      stock;
    long long unit;     /* paise */
    char      name[128];
} line_item_t;

static void h_checkout(session_t *s, int argc, char **argv, resp_t *r)
{
    payment_request_t preq;
    payment_result_t  pres;
    char err[160], money[32], unit_buf[32], sub_buf[32], addr[256];
    line_item_t *items = NULL;
    size_t n = 0;
    long long total = 0;
    long order_id = 0;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;

    memset(&pres, 0, sizeof pres);
    if (payment_parse(argc - 1, argv + 1, &preq, err, sizeof err) < 0) {
        resp_err(r, "%s", err);
        return;
    }

    if (db_begin(s->db) < 0)
        goto db_error;

    /*
     * Lock the cart rows and the product rows (SELECT ... FOR UPDATE).
     * Concurrent checkouts for the same product wait here, so stock can
     * never be oversold. Rows are locked in product_id order to avoid
     * deadlocks between two customers.
     */
    res = db_query(s->db,
        "SELECT c.product_id, c.quantity, p.price, p.stock, p.is_active, p.name "
        "FROM cart_items c JOIN products p ON p.product_id = c.product_id "
        "WHERE c.user_id = %ld ORDER BY c.product_id FOR UPDATE", s->user_id);
    if (!res)
        goto db_error;

    n = (size_t)mysql_num_rows(res);
    if (n == 0) {
        mysql_free_result(res);
        db_rollback(s->db);
        resp_err(r, "Your cart is empty");
        goto out;
    }
    items = calloc(n, sizeof *items);
    if (!items)
        goto db_error;

    for (size_t i = 0; i < n && (row = mysql_fetch_row(res)); i++) {
        line_item_t *it = &items[i];
        it->pid   = atol(row[0]);
        it->qty   = atol(row[1]);
        it->stock = atol(row[3]);
        parse_money(row[2], &it->unit);
        snprintf(it->name, sizeof it->name, "%s", row[5]);

        if (atoi(row[4]) == 0) {
            snprintf(err, sizeof err, "'%s' is no longer sold - please remove it from your cart", it->name);
            goto reject;
        }
        if (it->stock < it->qty) {
            snprintf(err, sizeof err, "Only %ld unit(s) of '%s' left in stock - please update your cart",
                     it->stock, it->name);
            goto reject;
        }
        total += it->unit * it->qty;
    }
    mysql_free_result(res);
    res = NULL;

    /* shipping address */
    res = db_query(s->db, "SELECT COALESCE(address, '') FROM users WHERE user_id = %ld", s->user_id);
    if (!res)
        goto db_error;
    row = mysql_fetch_row(res);
    snprintf(addr, sizeof addr, "%s", row && row[0] ? row[0] : "");
    mysql_free_result(res);
    res = NULL;
    if (!addr[0]) {
        snprintf(err, sizeof err, "Please add a shipping address to your profile before checkout");
        goto reject;
    }

    fmt_money(total, money, sizeof money);
    if (db_exec(s->db,
                "INSERT INTO orders (user_id, status, total_amount, payment_status, shipping_address) "
                "VALUES (%ld, 'PLACED', %s, 'PENDING', '%s')",
                s->user_id, money, esc(s, addr)) < 0)
        goto db_error;
    order_id = (long)mysql_insert_id(s->db);

    for (size_t i = 0; i < n; i++) {
        line_item_t *it = &items[i];
        fmt_money(it->unit, unit_buf, sizeof unit_buf);
        fmt_money(it->unit * it->qty, sub_buf, sizeof sub_buf);

        if (db_exec(s->db,
                    "INSERT INTO order_items (order_id, product_id, quantity, unit_price, subtotal) "
                    "VALUES (%ld, %ld, %ld, %s, %s)",
                    order_id, it->pid, it->qty, unit_buf, sub_buf) < 0)
            goto db_error;

        /* real-time inventory update */
        if (db_exec(s->db,
                    "UPDATE products SET stock = stock - %ld WHERE product_id = %ld AND stock >= %ld",
                    it->qty, it->pid, it->qty) < 0)
            goto db_error;
        if (mysql_affected_rows(s->db) != 1) {
            snprintf(err, sizeof err, "'%s' just went out of stock", it->name);
            goto reject;
        }
        if (db_exec(s->db,
                    "INSERT INTO inventory_log (product_id, change_qty, reason, ref_order_id, changed_by, stock_after) "
                    "SELECT product_id, -%ld, 'SALE', %ld, %ld, stock FROM products WHERE product_id = %ld",
                    it->qty, order_id, s->user_id, it->pid) < 0)
            goto db_error;
    }

    /* payment through the gateway child process */
    if (payment_process(&preq, total, &pres) < 0 || !pres.approved) {
        db_rollback(s->db);
        LOG_WARN("Payment failed for user '%s' (Rs %s via %s): %s",
                 s->username, money, payment_method_name(preq.method), pres.message);
        resp_err(r, "Payment failed: %s. The order was not placed and your cart is unchanged.",
                 pres.message[0] ? pres.message : "gateway error");
        goto out;
    }

    if (db_exec(s->db,
                "INSERT INTO payments (order_id, method, masked_detail, amount, status, reference, paid_at) "
                "VALUES (%ld, '%s', '%s', %s, '%s', '%s', %s)",
                order_id, payment_method_name(preq.method), esc(s, preq.masked), money,
                pres.pending ? "PENDING" : "SUCCESS", esc(s, pres.reference),
                pres.pending ? "NULL" : "NOW()") < 0 ||
        db_exec(s->db, "UPDATE orders SET payment_status = '%s' WHERE order_id = %ld",
                pres.pending ? "PENDING" : "PAID", order_id) < 0 ||
        db_exec(s->db,
                "INSERT INTO order_status_history (order_id, status, changed_by, note) "
                "VALUES (%ld, 'PLACED', %ld, 'Order placed by customer')", order_id, s->user_id) < 0 ||
        db_exec(s->db, "DELETE FROM cart_items WHERE user_id = %ld", s->user_id) < 0)
        goto db_error;

    if (db_commit(s->db) < 0) {
        db_rollback(s->db);
        LOG_ERROR("COMMIT failed for order by '%s'; payment %s must be reversed", s->username, pres.reference);
        resp_err(r, "Could not save the order. Any payment taken (ref %s) will be reversed.", pres.reference);
        goto out;
    }

    stats_order_placed();
    LOG_INFO("Order #%ld placed by '%s': Rs %s via %s (ref %s)",
             order_id, s->username, money, payment_method_name(preq.method), pres.reference);

    /* low-stock alerts for the admin (server log) */
    for (size_t i = 0; i < n; i++) {
        long left = items[i].stock - items[i].qty;
        long long reorder = db_scalar(s->db, 0, "SELECT reorder_level FROM products WHERE product_id = %ld",
                                      items[i].pid);
        if (left <= reorder)
            LOG_WARN("LOW STOCK: '%s' (id %ld) has %ld unit(s) left (reorder level %lld)",
                     items[i].name, items[i].pid, left, reorder);
    }

    resp_ok(r, "Order #%ld placed! Total Rs %s. Payment: %s (ref %s)",
            order_id, money, pres.message, pres.reference);
    resp_heading(r, "Order summary");
    resp_rowv(r, 4, "Product", "Qty", "Unit price (Rs)", "Subtotal (Rs)");
    for (size_t i = 0; i < n; i++) {
        char q[16];
        snprintf(q, sizeof q, "%ld", items[i].qty);
        fmt_money(items[i].unit, unit_buf, sizeof unit_buf);
        fmt_money(items[i].unit * items[i].qty, sub_buf, sizeof sub_buf);
        resp_rowv(r, 4, items[i].name, q, unit_buf, sub_buf);
    }
    goto out;

reject:
    if (res)
        mysql_free_result(res);
    db_rollback(s->db);
    resp_err(r, "%s", err);
    goto out;

db_error:
    {
        unsigned int e = mysql_errno(s->db);
        if (res)
            mysql_free_result(res);
        db_rollback(s->db);
        if (e == ER_LOCK_DEADLOCK || e == ER_LOCK_WAIT_TIMEOUT)
            resp_err(r, "The store is busy right now - please try checkout again");
        else
            resp_err(r, "Database error during checkout - nothing was charged");
    }

out:
    free(items);
    payment_wipe(&preq);
}

/* ======================================================================
 *  Orders and tracking
 * ====================================================================== */

static void h_my_orders(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    char sql[1024];
    snprintf(sql, sizeof sql,
             "SELECT o.order_id AS 'Order', DATE_FORMAT(o.order_date, " DT ") AS Placed, "
             "(SELECT SUM(quantity) FROM order_items WHERE order_id = o.order_id) AS Items, "
             "o.total_amount AS 'Total (Rs)', o.status AS Status, o.payment_status AS Payment "
             "FROM orders o WHERE o.user_id = %ld ORDER BY o.order_date DESC", s->user_id);
    resp_ok(r, "Your orders");
    int n = add_table(s, r, NULL, sql);
    if (n < 0)
        db_fail(r);
    else if (n == 0)
        resp_ok(r, "You have not placed any orders yet");
}

static void h_order_details(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long oid;
    char sql[1024], owner[64] = "";
    if (parse_long(argv[1], 1, INT_MAX, &oid) < 0) { resp_err(r, "Invalid order id"); return; }

    if (!s->is_admin)
        snprintf(owner, sizeof owner, " AND o.user_id = %ld", s->user_id);

    long long found = db_scalar(s->db, -1, "SELECT COUNT(*) FROM orders o WHERE o.order_id = %ld%s", oid, owner);
    if (found < 0) { db_fail(r); return; }
    if (found == 0) { resp_err(r, "Order #%ld not found", oid); return; }

    resp_ok(r, "Order #%ld", oid);

    snprintf(sql, sizeof sql,
             "SELECT o.order_id AS 'Order', DATE_FORMAT(o.order_date, " DT ") AS Placed, "
             "u.username AS Customer, o.status AS Status, o.payment_status AS Payment, "
             "o.total_amount AS 'Total (Rs)', o.shipping_address AS 'Ship to' "
             "FROM orders o JOIN users u ON u.user_id = o.user_id WHERE o.order_id = %ld", oid);
    if (add_table(s, r, "Order", sql) < 0) goto fail;

    snprintf(sql, sizeof sql,
             "SELECT p.product_id AS ID, p.name AS Product, oi.quantity AS Qty, "
             "oi.unit_price AS 'Unit price (Rs)', oi.subtotal AS 'Subtotal (Rs)' "
             "FROM order_items oi JOIN products p ON p.product_id = oi.product_id "
             "WHERE oi.order_id = %ld ORDER BY oi.order_item_id", oid);
    if (add_table(s, r, "Items", sql) < 0) goto fail;

    snprintf(sql, sizeof sql,
             "SELECT method AS Method, masked_detail AS Detail, amount AS 'Amount (Rs)', status AS Status, "
             "reference AS Reference, COALESCE(DATE_FORMAT(paid_at, " DT "), '-') AS 'Paid at' "
             "FROM payments WHERE order_id = %ld", oid);
    if (add_table(s, r, "Payment", sql) < 0) goto fail;

    snprintf(sql, sizeof sql,
             "SELECT DATE_FORMAT(h.changed_at, " DT ") AS 'When', h.status AS Status, "
             "COALESCE(h.note, '') AS Note, COALESCE(u.username, 'system') AS 'By' "
             "FROM order_status_history h LEFT JOIN users u ON u.user_id = h.changed_by "
             "WHERE h.order_id = %ld ORDER BY h.changed_at, h.history_id", oid);
    if (add_table(s, r, "Tracking history", sql) < 0) goto fail;
    return;

fail:
    db_fail(r);
}

/*
 * Cancel an order and put the items back into stock (one transaction).
 * Returns 0 on success, -1 with err filled in.
 */
static int cancel_order(session_t *s, long oid, const char *note, char *err, size_t errsz, char *refund, size_t refsz)
{
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;

    if (db_begin(s->db) < 0)
        goto db_error;

    res = db_query(s->db, "SELECT user_id, status, payment_status FROM orders WHERE order_id = %ld FOR UPDATE", oid);
    if (!res)
        goto db_error;
    row = mysql_fetch_row(res);
    if (!row || (!s->is_admin && atol(row[0]) != s->user_id)) {
        mysql_free_result(res);
        db_rollback(s->db);
        snprintf(err, errsz, "Order #%ld not found", oid);
        return -1;
    }
    char status[24], pay[16];
    snprintf(status, sizeof status, "%s", row[1]);
    snprintf(pay, sizeof pay, "%s", row[2]);
    mysql_free_result(res);
    res = NULL;

    if (strcmp(status, "PLACED") != 0 && strcmp(status, "CONFIRMED") != 0) {
        db_rollback(s->db);
        snprintf(err, errsz, "Order #%ld is %s and can no longer be cancelled", oid, status);
        return -1;
    }

    /* return every item to stock */
    res = db_query(s->db, "SELECT product_id, quantity FROM order_items WHERE order_id = %ld ORDER BY product_id", oid);
    if (!res)
        goto db_error;
    while ((row = mysql_fetch_row(res))) {
        long pid = atol(row[0]), qty = atol(row[1]);
        if (db_exec(s->db, "UPDATE products SET stock = stock + %ld WHERE product_id = %ld", qty, pid) < 0 ||
            db_exec(s->db,
                    "INSERT INTO inventory_log (product_id, change_qty, reason, ref_order_id, changed_by, stock_after) "
                    "SELECT product_id, %ld, 'CANCEL', %ld, %ld, stock FROM products WHERE product_id = %ld",
                    qty, oid, s->user_id, pid) < 0)
            goto db_error;
    }
    mysql_free_result(res);
    res = NULL;

    const char *new_pay = strcmp(pay, "PAID") == 0 ? "REFUNDED" : "VOID";
    if (db_exec(s->db, "UPDATE orders SET status = 'CANCELLED', payment_status = '%s' WHERE order_id = %ld",
                new_pay, oid) < 0 ||
        db_exec(s->db, "UPDATE payments SET status = '%s' WHERE order_id = %ld", new_pay, oid) < 0 ||
        db_exec(s->db,
                "INSERT INTO order_status_history (order_id, status, changed_by, note) "
                "VALUES (%ld, 'CANCELLED', %ld, '%s')", oid, s->user_id, esc(s, note)) < 0)
        goto db_error;

    if (db_commit(s->db) < 0)
        goto db_error;

    snprintf(refund, refsz, "%s", strcmp(new_pay, "REFUNDED") == 0
             ? "Payment will be refunded to the original method."
             : "No payment was collected.");
    LOG_INFO("Order #%ld cancelled by '%s' (%s)", oid, s->username, note);
    return 0;

db_error:
    if (res)
        mysql_free_result(res);
    db_rollback(s->db);
    snprintf(err, errsz, "Database error - please try again");
    return -1;
}

static void h_cancel_order(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long oid;
    char err[160], refund[96];
    if (parse_long(argv[1], 1, INT_MAX, &oid) < 0) { resp_err(r, "Invalid order id"); return; }
    if (cancel_order(s, oid, "Cancelled by customer", err, sizeof err, refund, sizeof refund) < 0)
        resp_err(r, "%s", err);
    else
        resp_ok(r, "Order #%ld cancelled and items returned to stock. %s", oid, refund);
}

/* ======================================================================
 *  Admin: inventory management
 * ====================================================================== */

static void h_add_product(session_t *s, int argc, char **argv, resp_t *r)
{
    const char *name = argv[1], *cat = argv[2], *desc = argc > 6 ? argv[6] : "";
    long long price;
    long stock, reorder;
    char pbuf[32];

    if (!*name || strlen(name) > 100) { resp_err(r, "Name must be 1-100 characters"); return; }
    if (!*cat || strlen(cat) > 50)    { resp_err(r, "Category must be 1-50 characters"); return; }
    if (parse_money(argv[3], &price) < 0) { resp_err(r, "Price must be a number like 499 or 499.50"); return; }
    if (parse_long(argv[4], 0, MAX_STOCK, &stock) < 0)   { resp_err(r, "Stock must be 0-%ld", MAX_STOCK); return; }
    if (parse_long(argv[5], 0, MAX_STOCK, &reorder) < 0) { resp_err(r, "Reorder level must be 0-%ld", MAX_STOCK); return; }
    if (strlen(desc) > 255) { resp_err(r, "Description must be at most 255 characters"); return; }
    fmt_money(price, pbuf, sizeof pbuf);

    if (db_begin(s->db) < 0) { db_fail(r); return; }
    if (db_exec(s->db,
                "INSERT INTO products (name, category, description, price, stock, reorder_level) "
                "VALUES ('%s', '%s', '%s', %s, %ld, %ld)",
                esc(s, name), esc(s, cat), esc(s, desc), pbuf, stock, reorder) < 0) {
        unsigned int e = mysql_errno(s->db);
        db_rollback(s->db);
        if (e == ER_DUP_ENTRY)
            resp_err(r, "A product named '%s' already exists", name);
        else
            db_fail(r);
        return;
    }
    long pid = (long)mysql_insert_id(s->db);
    if (db_exec(s->db,
                "INSERT INTO inventory_log (product_id, change_qty, reason, changed_by, stock_after, note) "
                "VALUES (%ld, %ld, 'INITIAL', %ld, %ld, 'Product created')", pid, stock, s->user_id, stock) < 0 ||
        db_commit(s->db) < 0) {
        db_rollback(s->db);
        db_fail(r);
        return;
    }
    LOG_INFO("Admin '%s' added product #%ld '%s' (stock %ld, Rs %s)", s->username, pid, name, stock, pbuf);
    resp_ok(r, "Product #%ld '%s' added with %ld unit(s) in stock", pid, name, stock);
}

static void h_update_product(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid, num = -1;
    long long price;
    char field[32], value_sql[600];
    const char *value = argv[3];

    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    snprintf(field, sizeof field, "%s", argv[2]);
    for (char *p = field; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (!strcmp(field, "name") || !strcmp(field, "category") || !strcmp(field, "description")) {
        size_t max = !strcmp(field, "name") ? 100 : !strcmp(field, "category") ? 50 : 255;
        if ((strcmp(field, "description") && !*value) || strlen(value) > max) {
            resp_err(r, "%s must be %s%zu characters", field, strcmp(field, "description") ? "1-" : "0-", max);
            return;
        }
        snprintf(value_sql, sizeof value_sql, "'%s'", esc(s, value));
    } else if (!strcmp(field, "price")) {
        if (parse_money(value, &price) < 0) { resp_err(r, "Price must be a number like 499 or 499.50"); return; }
        fmt_money(price, value_sql, sizeof value_sql);
    } else if (!strcmp(field, "reorder_level")) {
        if (parse_long(value, 0, MAX_STOCK, &num) < 0) { resp_err(r, "Reorder level must be 0-%ld", MAX_STOCK); return; }
        snprintf(value_sql, sizeof value_sql, "%ld", num);
    } else if (!strcmp(field, "is_active")) {
        if (parse_long(value, 0, 1, &num) < 0) { resp_err(r, "is_active must be 0 or 1"); return; }
        snprintf(value_sql, sizeof value_sql, "%ld", num);
    } else {
        resp_err(r, "Field must be one of: name, category, description, price, reorder_level, is_active "
                    "(use RESTOCK / ADJUST_STOCK for stock)");
        return;
    }

    long long exists = db_scalar(s->db, -1, "SELECT COUNT(*) FROM products WHERE product_id = %ld", pid);
    if (exists < 0) { db_fail(r); return; }
    if (exists == 0) { resp_err(r, "Product %ld not found", pid); return; }

    if (db_exec(s->db, "UPDATE products SET %s = %s WHERE product_id = %ld", field, value_sql, pid) < 0) {
        if (mysql_errno(s->db) == ER_DUP_ENTRY)
            resp_err(r, "Another product already has that name");
        else
            db_fail(r);
        return;
    }
    if (!strcmp(field, "is_active") && num == 0)
        db_exec(s->db, "DELETE FROM cart_items WHERE product_id = %ld", pid);

    LOG_INFO("Admin '%s' updated product #%ld: %s", s->username, pid, field);
    resp_ok(r, "Product #%ld updated (%s)", pid, field);
}

/* Change stock by delta or set it to an absolute value, with an audit log entry. */
static void stock_change(session_t *s, resp_t *r, long pid, long amount, int absolute,
                         const char *reason, const char *note)
{
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (db_begin(s->db) < 0) { db_fail(r); return; }
    res = db_query(s->db, "SELECT stock, name FROM products WHERE product_id = %ld FOR UPDATE", pid);
    if (!res) { db_rollback(s->db); db_fail(r); return; }
    row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        db_rollback(s->db);
        resp_err(r, "Product %ld not found", pid);
        return;
    }
    long old = atol(row[0]);
    char name[128];
    snprintf(name, sizeof name, "%s", row[1]);
    mysql_free_result(res);

    long new_stock = absolute ? amount : old + amount;
    long delta = new_stock - old;
    if (new_stock < 0 || new_stock > MAX_STOCK) {
        db_rollback(s->db);
        resp_err(r, "Resulting stock must be between 0 and %ld", MAX_STOCK);
        return;
    }
    if (db_exec(s->db, "UPDATE products SET stock = %ld WHERE product_id = %ld", new_stock, pid) < 0 ||
        db_exec(s->db,
                "INSERT INTO inventory_log (product_id, change_qty, reason, changed_by, stock_after, note) "
                "VALUES (%ld, %ld, '%s', %ld, %ld, '%s')",
                pid, delta, reason, s->user_id, new_stock, esc(s, note)) < 0 ||
        db_commit(s->db) < 0) {
        db_rollback(s->db);
        db_fail(r);
        return;
    }
    LOG_INFO("Admin '%s' %s product #%ld '%s': %ld -> %ld", s->username, reason, pid, name, old, new_stock);
    resp_ok(r, "'%s' stock changed from %ld to %ld (%+ld)", name, old, new_stock, delta);
}

static void h_restock(session_t *s, int argc, char **argv, resp_t *r)
{
    long pid, qty;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    if (parse_long(argv[2], 1, MAX_STOCK, &qty) < 0) { resp_err(r, "Quantity must be 1-%ld", MAX_STOCK); return; }
    stock_change(s, r, pid, qty, 0, "RESTOCK", argc > 3 && *argv[3] ? argv[3] : "Stock received");
}

static void h_adjust_stock(session_t *s, int argc, char **argv, resp_t *r)
{
    long pid, qty;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
    if (parse_long(argv[2], 0, MAX_STOCK, &qty) < 0) { resp_err(r, "New stock must be 0-%ld", MAX_STOCK); return; }
    stock_change(s, r, pid, qty, 1, "ADJUST", argc > 3 && *argv[3] ? argv[3] : "Manual stock correction");
}

static void h_delete_product(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long pid;
    if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }

    long long active = db_scalar(s->db, -1, "SELECT is_active FROM products WHERE product_id = %ld", pid);
    if (active < 0) { resp_err(r, "Product %ld not found", pid); return; }

    long long sold = db_scalar(s->db, -1, "SELECT COUNT(*) FROM order_items WHERE product_id = %ld", pid);
    if (sold < 0) { db_fail(r); return; }

    if (db_begin(s->db) < 0) { db_fail(r); return; }
    if (sold == 0) {
        /* never sold: remove completely */
        if (db_exec(s->db, "DELETE FROM cart_items WHERE product_id = %ld", pid) < 0 ||
            db_exec(s->db, "DELETE FROM inventory_log WHERE product_id = %ld", pid) < 0 ||
            db_exec(s->db, "DELETE FROM products WHERE product_id = %ld", pid) < 0 ||
            db_commit(s->db) < 0) {
            db_rollback(s->db);
            db_fail(r);
            return;
        }
        LOG_INFO("Admin '%s' deleted product #%ld", s->username, pid);
        resp_ok(r, "Product #%ld deleted", pid);
        return;
    }

    if (!active) {
        db_rollback(s->db);
        resp_err(r, "Product #%ld is already deleted", pid);
        return;
    }
    /* has order history: soft delete so old orders stay valid */
    if (db_exec(s->db, "UPDATE products SET is_active = 0 WHERE product_id = %ld", pid) < 0 ||
        db_exec(s->db, "DELETE FROM cart_items WHERE product_id = %ld", pid) < 0 ||
        db_commit(s->db) < 0) {
        db_rollback(s->db);
        db_fail(r);
        return;
    }
    LOG_INFO("Admin '%s' deactivated product #%ld", s->username, pid);
    resp_ok(r, "Product #%ld removed from the store (kept in the database because it appears in past orders)", pid);
}

/* ======================================================================
 *  Admin: orders
 * ====================================================================== */

static const char *ORDER_STATUSES[] = {
    "PLACED", "CONFIRMED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"
};

static int valid_status(const char *st)
{
    for (size_t i = 0; i < sizeof ORDER_STATUSES / sizeof *ORDER_STATUSES; i++)
        if (!strcmp(st, ORDER_STATUSES[i]))
            return 1;
    return 0;
}

static int transition_ok(const char *from, const char *to)
{
    static const char *allowed[][2] = {
        { "PLACED",           "CONFIRMED"        },
        { "CONFIRMED",        "SHIPPED"          },
        { "SHIPPED",          "OUT_FOR_DELIVERY" },
        { "SHIPPED",          "DELIVERED"        },
        { "OUT_FOR_DELIVERY", "DELIVERED"        },
    };
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++)
        if (!strcmp(from, allowed[i][0]) && !strcmp(to, allowed[i][1]))
            return 1;
    return 0;
}

static void h_list_orders(session_t *s, int argc, char **argv, resp_t *r)
{
    char status[32] = "", where[64] = "";
    if (argc > 1 && *argv[1]) {
        snprintf(status, sizeof status, "%s", argv[1]);
        str_upper(status);
        if (!valid_status(status)) {
            resp_err(r, "Unknown status. Use PLACED, CONFIRMED, SHIPPED, OUT_FOR_DELIVERY, DELIVERED or CANCELLED");
            return;
        }
        snprintf(where, sizeof where, "WHERE o.status = '%s'", status);
    }

    MYSQL_RES *res = db_query(s->db,
        "SELECT o.order_id AS 'Order', DATE_FORMAT(o.order_date, " DT ") AS Placed, u.username AS Customer, "
        "(SELECT SUM(quantity) FROM order_items WHERE order_id = o.order_id) AS Items, "
        "o.total_amount AS 'Total (Rs)', o.status AS Status, o.payment_status AS Payment "
        "FROM orders o JOIN users u ON u.user_id = o.user_id %s "
        "ORDER BY o.order_date DESC LIMIT 200", where);
    if (!res) { db_fail(r); return; }
    resp_ok(r, "%llu order(s)%s%s", (unsigned long long)mysql_num_rows(res), *status ? " with status " : "", status);
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_update_order_status(session_t *s, int argc, char **argv, resp_t *r)
{
    long oid;
    char to[32], note[160];
    if (parse_long(argv[1], 1, INT_MAX, &oid) < 0) { resp_err(r, "Invalid order id"); return; }
    snprintf(to, sizeof to, "%s", argv[2]);
    str_upper(to);
    if (!valid_status(to) || !strcmp(to, "PLACED")) {
        resp_err(r, "New status must be CONFIRMED, SHIPPED, OUT_FOR_DELIVERY, DELIVERED or CANCELLED");
        return;
    }
    snprintf(note, sizeof note, "%s", argc > 3 && *argv[3] ? argv[3] : "Updated by admin");

    if (!strcmp(to, "CANCELLED")) {
        char err[160], refund[96];
        if (cancel_order(s, oid, note, err, sizeof err, refund, sizeof refund) < 0)
            resp_err(r, "%s", err);
        else
            resp_ok(r, "Order #%ld cancelled, items restocked. %s", oid, refund);
        return;
    }

    if (db_begin(s->db) < 0) { db_fail(r); return; }
    MYSQL_RES *res = db_query(s->db, "SELECT status, payment_status FROM orders WHERE order_id = %ld FOR UPDATE", oid);
    if (!res) { db_rollback(s->db); db_fail(r); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        db_rollback(s->db);
        resp_err(r, "Order #%ld not found", oid);
        return;
    }
    char from[32], pay[16];
    snprintf(from, sizeof from, "%s", row[0]);
    snprintf(pay, sizeof pay, "%s", row[1]);
    mysql_free_result(res);

    if (!transition_ok(from, to)) {
        db_rollback(s->db);
        resp_err(r, "Cannot change order #%ld from %s to %s", oid, from, to);
        return;
    }

    int cod_collected = !strcmp(to, "DELIVERED") && !strcmp(pay, "PENDING");
    if (db_exec(s->db, "UPDATE orders SET status = '%s'%s WHERE order_id = %ld",
                to, cod_collected ? ", payment_status = 'PAID'" : "", oid) < 0 ||
        (cod_collected &&
         db_exec(s->db, "UPDATE payments SET status = 'SUCCESS', paid_at = NOW() WHERE order_id = %ld", oid) < 0) ||
        db_exec(s->db,
                "INSERT INTO order_status_history (order_id, status, changed_by, note) VALUES (%ld, '%s', %ld, '%s')",
                oid, to, s->user_id, esc(s, note)) < 0 ||
        db_commit(s->db) < 0) {
        db_rollback(s->db);
        db_fail(r);
        return;
    }
    LOG_INFO("Admin '%s' moved order #%ld %s -> %s", s->username, oid, from, to);
    resp_ok(r, "Order #%ld: %s -> %s%s", oid, from, to, cod_collected ? " (cash collected, marked PAID)" : "");
}

/* ======================================================================
 *  Admin: customers
 * ====================================================================== */

static void h_list_customers(session_t *s, int argc, char **argv, resp_t *r)
{
    char filter[512] = "";
    if (argc > 1 && *argv[1]) {
        if (strlen(argv[1]) > 50) { resp_err(r, "Search text is too long"); return; }
        const char *e = esc_like(s, argv[1]);
        snprintf(filter, sizeof filter,
                 " AND (u.username LIKE '%%%s%%' OR u.full_name LIKE '%%%s%%' OR u.email LIKE '%%%s%%')", e, e, e);
    }
    MYSQL_RES *res = db_query(s->db,
        "SELECT u.user_id AS ID, u.username AS Username, u.full_name AS Name, u.email AS Email, "
        "COALESCE(u.phone, '') AS Phone, IF(u.is_active, 'Active', 'Disabled') AS Status, "
        "COUNT(o.order_id) AS Orders, "
        "COALESCE(SUM(CASE WHEN o.status <> 'CANCELLED' THEN o.total_amount END), 0) AS 'Spent (Rs)', "
        "DATE_FORMAT(u.created_at, " DAY ") AS Joined "
        "FROM users u LEFT JOIN orders o ON o.user_id = u.user_id "
        "WHERE u.role = 'CUSTOMER'%s GROUP BY u.user_id ORDER BY u.user_id", filter);
    if (!res) { db_fail(r); return; }
    resp_ok(r, "%llu customer(s)", (unsigned long long)mysql_num_rows(res));
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_set_user_active(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc;
    long uid, active;
    if (parse_long(argv[1], 1, INT_MAX, &uid) < 0) { resp_err(r, "Invalid user id"); return; }
    if (parse_long(argv[2], 0, 1, &active) < 0) { resp_err(r, "Value must be 1 (enable) or 0 (disable)"); return; }
    if (db_exec(s->db, "UPDATE users SET is_active = %ld, failed_logins = 0, locked_until = NULL "
                       "WHERE user_id = %ld AND role = 'CUSTOMER'", active, uid) < 0) { db_fail(r); return; }
    long long found = db_scalar(s->db, 0, "SELECT COUNT(*) FROM users WHERE user_id = %ld AND role = 'CUSTOMER'", uid);
    if (!found) { resp_err(r, "Customer %ld not found", uid); return; }
    LOG_INFO("Admin '%s' %s customer #%ld", s->username, active ? "enabled" : "disabled", uid);
    resp_ok(r, "Customer #%ld %s", uid, active ? "enabled (and unlocked)" : "disabled");
}

/* ======================================================================
 *  Admin: reports and analysis
 * ====================================================================== */

static void h_report_summary(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    static const char *sql =
        "SELECT 'Total revenue (Rs)' AS Metric, CAST(COALESCE(SUM(total_amount), 0) AS CHAR) AS Value "
        "  FROM orders WHERE status <> 'CANCELLED' "
        "UNION ALL SELECT 'Revenue today (Rs)', CAST(COALESCE(SUM(total_amount), 0) AS CHAR) "
        "  FROM orders WHERE status <> 'CANCELLED' AND DATE(order_date) = CURDATE() "
        "UNION ALL SELECT 'Revenue last 30 days (Rs)', CAST(COALESCE(SUM(total_amount), 0) AS CHAR) "
        "  FROM orders WHERE status <> 'CANCELLED' AND order_date >= CURDATE() - INTERVAL 30 DAY "
        "UNION ALL SELECT 'Average order value (Rs)', CAST(COALESCE(ROUND(AVG(total_amount), 2), 0) AS CHAR) "
        "  FROM orders WHERE status <> 'CANCELLED' "
        "UNION ALL SELECT 'Orders - total', CAST(COUNT(*) AS CHAR) FROM orders "
        "UNION ALL SELECT 'Orders - awaiting delivery', CAST(COUNT(*) AS CHAR) FROM orders "
        "  WHERE status IN ('PLACED','CONFIRMED','SHIPPED','OUT_FOR_DELIVERY') "
        "UNION ALL SELECT 'Orders - delivered', CAST(COUNT(*) AS CHAR) FROM orders WHERE status = 'DELIVERED' "
        "UNION ALL SELECT 'Orders - cancelled', CAST(COUNT(*) AS CHAR) FROM orders WHERE status = 'CANCELLED' "
        "UNION ALL SELECT 'Units sold', CAST(COALESCE(SUM(oi.quantity), 0) AS CHAR) "
        "  FROM order_items oi JOIN orders o ON o.order_id = oi.order_id WHERE o.status <> 'CANCELLED' "
        "UNION ALL SELECT 'Registered customers', CAST(COUNT(*) AS CHAR) FROM users WHERE role = 'CUSTOMER' "
        "UNION ALL SELECT 'Products on sale', CAST(COUNT(*) AS CHAR) FROM products WHERE is_active = 1 "
        "UNION ALL SELECT 'Products low / out of stock', CAST(COUNT(*) AS CHAR) FROM products "
        "  WHERE is_active = 1 AND stock <= reorder_level "
        "UNION ALL SELECT 'Inventory value (Rs)', CAST(COALESCE(SUM(price * stock), 0) AS CHAR) "
        "  FROM products WHERE is_active = 1";
    resp_ok(r, "Business summary");
    if (add_table(s, r, NULL, sql) < 0)
        db_fail(r);
}

static void h_report_sales(session_t *s, int argc, char **argv, resp_t *r)
{
    long days = 7;
    if (argc > 1 && *argv[1] && parse_long(argv[1], 1, 365, &days) < 0) {
        resp_err(r, "Days must be between 1 and 365");
        return;
    }
    MYSQL_RES *res = db_query(s->db,
        "SELECT COUNT(*), COALESCE(SUM(orders), 0), COALESCE(SUM(units), 0), COALESCE(SUM(revenue), 0) "
        "FROM v_daily_sales WHERE sale_date > CURDATE() - INTERVAL %ld DAY", days);
    if (!res) { db_fail(r); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    resp_ok(r, "Sales for the last %ld day(s): %s order(s), %s unit(s), revenue Rs %s",
            days, row ? row[1] : "0", row ? row[2] : "0", row ? row[3] : "0");
    mysql_free_result(res);

    res = db_query(s->db,
        "SELECT DATE_FORMAT(sale_date, " DAY ") AS Date, DAYNAME(sale_date) AS Day, orders AS Orders, "
        "units AS Units, revenue AS 'Revenue (Rs)' "
        "FROM v_daily_sales WHERE sale_date > CURDATE() - INTERVAL %ld DAY ORDER BY sale_date", days);
    if (!res) { db_fail(r); return; }
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_report_top(session_t *s, int argc, char **argv, resp_t *r)
{
    long n = 10;
    if (argc > 1 && *argv[1] && parse_long(argv[1], 1, 100, &n) < 0) {
        resp_err(r, "Count must be between 1 and 100");
        return;
    }
    MYSQL_RES *res = db_query(s->db,
        "SELECT product_id AS ID, name AS Product, category AS Category, units_sold AS 'Units sold', "
        "revenue AS 'Revenue (Rs)' FROM v_product_sales WHERE units_sold > 0 "
        "ORDER BY units_sold DESC, revenue DESC LIMIT %ld", n);
    if (!res) { db_fail(r); return; }
    resp_ok(r, "Top %ld best-selling products", n);
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_report_category(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    static const char *sql =
        "SELECT category AS Category, COUNT(*) AS Products, SUM(units_sold) AS 'Units sold', "
        "SUM(revenue) AS 'Revenue (Rs)' FROM v_product_sales GROUP BY category ORDER BY SUM(revenue) DESC";
    resp_ok(r, "Sales by category");
    if (add_table(s, r, NULL, sql) < 0)
        db_fail(r);
}

static void h_report_inventory(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    long long low = db_scalar(s->db, -1,
        "SELECT COUNT(*) FROM products WHERE is_active = 1 AND stock <= reorder_level");
    MYSQL_RES *res = db_query(s->db,
        "SELECT product_id AS ID, name AS Product, category AS Category, stock AS Stock, "
        "reorder_level AS Reorder, price AS 'Price (Rs)', price * stock AS 'Stock value (Rs)', "
        "CASE WHEN stock = 0 THEN 'OUT OF STOCK' WHEN stock <= reorder_level THEN 'REORDER' ELSE 'OK' END AS Status "
        "FROM products WHERE is_active = 1 "
        "ORDER BY (stock <= reorder_level) DESC, stock ASC, name");
    if (!res) { db_fail(r); return; }
    resp_ok(r, "Inventory status: %lld product(s) need restocking", low);
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_report_stock_log(session_t *s, int argc, char **argv, resp_t *r)
{
    long pid = 0;
    char where[64] = "";
    if (argc > 1 && *argv[1]) {
        if (parse_long(argv[1], 1, INT_MAX, &pid) < 0) { resp_err(r, "Invalid product id"); return; }
        snprintf(where, sizeof where, "WHERE l.product_id = %ld", pid);
    }
    MYSQL_RES *res = db_query(s->db,
        "SELECT DATE_FORMAT(l.created_at, " DT ") AS 'When', p.name AS Product, l.reason AS Reason, "
        "l.change_qty AS 'Change', l.stock_after AS 'Stock after', COALESCE(l.ref_order_id, '') AS 'Order', "
        "COALESCE(u.username, 'system') AS 'By', COALESCE(l.note, '') AS Note "
        "FROM inventory_log l JOIN products p ON p.product_id = l.product_id "
        "LEFT JOIN users u ON u.user_id = l.changed_by %s "
        "ORDER BY l.log_id DESC LIMIT 50", where);
    if (!res) { db_fail(r); return; }
    resp_ok(r, "Latest stock movements%s", pid ? " for the selected product" : "");
    resp_result(r, res);
    mysql_free_result(res);
}

static void h_server_stats(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)s; (void)argc; (void)argv;
    server_stats_t st;
    stats_snapshot(&st);

    long up = (long)(time(NULL) - st.started);
    char b[10][48];
    snprintf(b[0], sizeof b[0], "%ld", st.pid);
    snprintf(b[1], sizeof b[1], "%ldd %02ldh %02ldm %02lds", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
    snprintf(b[2], sizeof b[2], "%d / %d", st.active_clients, st.max_clients);
    snprintf(b[3], sizeof b[3], "%lu", st.total_connections);
    snprintf(b[4], sizeof b[4], "%lu", st.rejected_busy);
    snprintf(b[5], sizeof b[5], "%lu", st.total_requests);
    snprintf(b[6], sizeof b[6], "%lu", st.orders_placed);
    snprintf(b[7], sizeof b[7], "%lu", st.failed_logins);
    snprintf(b[8], sizeof b[8], "%s", mysql_get_client_info());

    resp_ok(r, "Server statistics");
    resp_rowv(r, 2, "Metric", "Value");
    resp_rowv(r, 2, "Server process id", b[0]);
    resp_rowv(r, 2, "Uptime", b[1]);
    resp_rowv(r, 2, "Connected clients (active / max)", b[2]);
    resp_rowv(r, 2, "Connections since start", b[3]);
    resp_rowv(r, 2, "Connections rejected (server full)", b[4]);
    resp_rowv(r, 2, "Requests served", b[5]);
    resp_rowv(r, 2, "Orders placed since start", b[6]);
    resp_rowv(r, 2, "Failed login attempts", b[7]);
    resp_rowv(r, 2, "MySQL client library", b[8]);
}

/* ======================================================================
 *  Dispatch table
 * ====================================================================== */

static void h_help(session_t *s, int argc, char **argv, resp_t *r);

static const command_t COMMANDS[] = {
    { "HELP",            0, R_ANY,      h_help,            "HELP" },
    { "PING",            0, R_ANY,      h_ping,            "PING" },
    { "QUIT",            0, R_ANY,      h_quit,            "QUIT" },
    { "REGISTER",        4, R_ANY,      h_register,        "REGISTER|username|password|full_name|email[|phone[|address]]" },
    { "LOGIN",           2, R_ANY,      h_login,           "LOGIN|username|password" },
    { "LIST_PRODUCTS",   0, R_ANY,      h_list_products,   "LIST_PRODUCTS[|search]" },
    { "VIEW_PRODUCT",    1, R_ANY,      h_view_product,    "VIEW_PRODUCT|product_id" },

    { "LOGOUT",          0, R_LOGIN,    h_logout,          "LOGOUT" },
    { "PROFILE",         0, R_LOGIN,    h_profile,         "PROFILE" },
    { "UPDATE_PROFILE",  2, R_LOGIN,    h_update_profile,  "UPDATE_PROFILE|full_name/email/phone/address|value" },
    { "CHANGE_PASSWORD", 2, R_LOGIN,    h_change_password, "CHANGE_PASSWORD|old_password|new_password" },
    { "ORDER_DETAILS",   1, R_LOGIN,    h_order_details,   "ORDER_DETAILS|order_id" },

    { "CART_ADD",        2, R_CUSTOMER, h_cart_add,        "CART_ADD|product_id|quantity" },
    { "CART_UPDATE",     2, R_CUSTOMER, h_cart_update,     "CART_UPDATE|product_id|quantity (0 = remove)" },
    { "CART_REMOVE",     1, R_CUSTOMER, h_cart_remove,     "CART_REMOVE|product_id" },
    { "CART_CLEAR",      0, R_CUSTOMER, h_cart_clear,      "CART_CLEAR" },
    { "CART_VIEW",       0, R_CUSTOMER, h_cart_view,       "CART_VIEW" },
    { "CHECKOUT",        1, R_CUSTOMER, h_checkout,        "CHECKOUT|CARD|number|MM/YY|cvv  or  CHECKOUT|UPI|id@bank  or  CHECKOUT|COD" },
    { "MY_ORDERS",       0, R_CUSTOMER, h_my_orders,       "MY_ORDERS" },
    { "CANCEL_ORDER",    1, R_CUSTOMER, h_cancel_order,    "CANCEL_ORDER|order_id" },

    { "ADD_PRODUCT",     5, R_ADMIN,    h_add_product,     "ADD_PRODUCT|name|category|price|stock|reorder_level[|description]" },
    { "UPDATE_PRODUCT",  3, R_ADMIN,    h_update_product,  "UPDATE_PRODUCT|product_id|field|value" },
    { "RESTOCK",         2, R_ADMIN,    h_restock,         "RESTOCK|product_id|quantity[|note]" },
    { "ADJUST_STOCK",    2, R_ADMIN,    h_adjust_stock,    "ADJUST_STOCK|product_id|new_stock[|note]" },
    { "DELETE_PRODUCT",  1, R_ADMIN,    h_delete_product,  "DELETE_PRODUCT|product_id" },
    { "LIST_ORDERS",     0, R_ADMIN,    h_list_orders,     "LIST_ORDERS[|status]" },
    { "UPDATE_ORDER_STATUS", 2, R_ADMIN, h_update_order_status, "UPDATE_ORDER_STATUS|order_id|status[|note]" },
    { "LIST_CUSTOMERS",  0, R_ADMIN,    h_list_customers,  "LIST_CUSTOMERS[|search]" },
    { "SET_USER_ACTIVE", 2, R_ADMIN,    h_set_user_active, "SET_USER_ACTIVE|user_id|1/0" },
    { "REPORT_SUMMARY",  0, R_ADMIN,    h_report_summary,  "REPORT_SUMMARY" },
    { "REPORT_SALES",    0, R_ADMIN,    h_report_sales,    "REPORT_SALES[|days]" },
    { "REPORT_TOP",      0, R_ADMIN,    h_report_top,      "REPORT_TOP[|count]" },
    { "REPORT_CATEGORY", 0, R_ADMIN,    h_report_category, "REPORT_CATEGORY" },
    { "REPORT_INVENTORY",0, R_ADMIN,    h_report_inventory,"REPORT_INVENTORY" },
    { "REPORT_STOCK_LOG",0, R_ADMIN,    h_report_stock_log,"REPORT_STOCK_LOG[|product_id]" },
    { "SERVER_STATS",    0, R_ADMIN,    h_server_stats,    "SERVER_STATS" },
};

#define NCOMMANDS (sizeof COMMANDS / sizeof *COMMANDS)

/* NULL if allowed, otherwise the reason it is not */
static const char *role_denied(const session_t *s, int role)
{
    switch (role) {
    case R_LOGIN:
        return s->user_id ? NULL : "Please log in first";
    case R_CUSTOMER:
        if (!s->user_id) return "Please log in first";
        return s->is_admin ? "This action is only for customer accounts" : NULL;
    case R_ADMIN:
        if (!s->user_id) return "Please log in first";
        return s->is_admin ? NULL : "Permission denied: administrators only";
    default:
        return NULL;
    }
}

static void h_help(session_t *s, int argc, char **argv, resp_t *r)
{
    (void)argc; (void)argv;
    resp_ok(r, "Commands available to you (fields separated by '|')");
    resp_rowv(r, 2, "Command", "Usage");
    for (size_t i = 0; i < NCOMMANDS; i++)
        if (!role_denied(s, COMMANDS[i].role))
            resp_rowv(r, 2, COMMANDS[i].name, COMMANDS[i].usage);
}

void handle_request(session_t *s, char *line, resp_t *r)
{
    char *argv[ORS_MAX_ARGS];
    int argc = 0;

    /* split on '|' (empty fields are kept) */
    char *p = line;
    for (;;) {
        if (argc == ORS_MAX_ARGS) {
            resp_err(r, "Too many fields in request");
            return;
        }
        argv[argc++] = p;
        char *bar = strchr(p, ORS_SEP);
        if (!bar)
            break;
        *bar = '\0';
        p = bar + 1;
    }

    while (*argv[0] == ' ')
        argv[0]++;
    if (argv[0][0] == '\0') {
        resp_err(r, "Empty request - type HELP for the list of commands");
        return;
    }
    str_upper(argv[0]);

    const command_t *cmd = NULL;
    for (size_t i = 0; i < NCOMMANDS; i++)
        if (!strcmp(argv[0], COMMANDS[i].name)) {
            cmd = &COMMANDS[i];
            break;
        }

    stats_request();

    if (!cmd) {
        resp_err(r, "Unknown command '%.40s' - type HELP", argv[0]);
        return;
    }
    const char *denied = role_denied(s, cmd->role);
    if (denied) {
        resp_err(r, "%s", denied);
        return;
    }
    if (argc - 1 < cmd->min_args) {
        resp_err(r, "Missing fields. Usage: %s", cmd->usage);
        return;
    }

    /* never log arguments: they may contain passwords or card numbers */
    LOG_INFO("%s [%s] %s", s->peer, s->user_id ? s->username : "guest", cmd->name);

    cmd->fn(s, argc, argv, r);

    for (int i = 0; i < s->npool; i++)
        free(s->pool[i]);
    s->npool = 0;
}
