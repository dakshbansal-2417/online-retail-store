/*
 * client.c - interactive terminal client for the ORS server.
 *
 * Usage: ors_client [host] [port]        (default 127.0.0.1 8080)
 *
 * OS concepts: TCP client socket (getaddrinfo/connect), termios to hide
 * password/CVV input, a SIGINT handler that restores the terminal, and
 * report export to CSV with open()/write()/close().
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"

#define MAX_COLS      32
#define MAX_COL_WIDTH 40

typedef struct {
    int     ok;
    char    msg[512];
    char  **lines;
    size_t  n;
    size_t  cap;
} reply_t;

static int            g_fd = -1;
static net_reader_t   g_rd;
static int            g_color;
static struct termios g_saved_tty;
static volatile sig_atomic_t g_echo_off;

#define C_RED    (g_color ? "\033[31m" : "")
#define C_GREEN  (g_color ? "\033[32m" : "")
#define C_YELLOW (g_color ? "\033[33m" : "")
#define C_CYAN   (g_color ? "\033[36m" : "")
#define C_BOLD   (g_color ? "\033[1m"  : "")
#define C_RESET  (g_color ? "\033[0m"  : "")

/* ======================================================================
 *  Terminal handling
 * ====================================================================== */

static void restore_tty(void)
{
    if (g_echo_off) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tty);
        g_echo_off = 0;
    }
}

static void on_sigint(int sig)
{
    (void)sig;
    if (g_echo_off)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tty);   /* async-signal-safe */
    static const char msg[] = "\nInterrupted - goodbye!\n";
    ssize_t w = write(STDOUT_FILENO, msg, sizeof msg - 1);
    (void)w;
    _exit(130);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

static void quit_client(void);

static void read_stdin(char *buf, size_t size)
{
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) {
        printf("\n");
        quit_client();
    }
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    else {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;               /* discard the rest of an over-long line */
    }
    trim(buf);
}

/* Text input; '|' is the protocol separator and is not allowed. */
static void prompt_text(const char *label, char *buf, size_t size, int required)
{
    for (;;) {
        printf("%s", label);
        read_stdin(buf, size);
        if (strchr(buf, ORS_SEP)) {
            printf("%s  The '|' character is not allowed.%s\n", C_YELLOW, C_RESET);
            continue;
        }
        if (required && !buf[0]) {
            printf("%s  This field is required.%s\n", C_YELLOW, C_RESET);
            continue;
        }
        return;
    }
}

static long prompt_long(const char *label, long min, long max)
{
    char buf[64];
    for (;;) {
        printf("%s", label);
        read_stdin(buf, sizeof buf);
        char *end;
        errno = 0;
        long v = strtol(buf, &end, 10);
        if (buf[0] && !*end && !errno && v >= min && v <= max)
            return v;
        printf("%s  Please enter a number between %ld and %ld.%s\n", C_YELLOW, min, max, C_RESET);
    }
}

/* Read without echo (passwords, CVV). */
static void prompt_secret(const char *label, char *buf, size_t size)
{
    for (;;) {
        printf("%s", label);
        fflush(stdout);
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_tty) == 0) {
            struct termios t = g_saved_tty;
            t.c_lflag &= ~(tcflag_t)ECHO;
            g_echo_off = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            read_stdin(buf, size);
            restore_tty();
            printf("\n");
        } else {
            read_stdin(buf, size);
        }
        if (!buf[0] || strchr(buf, ORS_SEP)) {
            printf("%s  Value required and may not contain '|'.%s\n", C_YELLOW, C_RESET);
            continue;
        }
        return;
    }
}

static int prompt_yes(const char *label)
{
    char buf[16];
    printf("%s [y/n]: ", label);
    read_stdin(buf, sizeof buf);
    return buf[0] == 'y' || buf[0] == 'Y';
}

static void pause_enter(void)
{
    char buf[8];
    printf("\n%sPress Enter to continue...%s", C_CYAN, C_RESET);
    read_stdin(buf, sizeof buf);
}

/* ======================================================================
 *  Protocol
 * ====================================================================== */

static void reply_free(reply_t *r)
{
    for (size_t i = 0; i < r->n; i++)
        free(r->lines[i]);
    free(r->lines);
    memset(r, 0, sizeof *r);
}

static int reply_push(reply_t *r, const char *line)
{
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 32;
        char **l = realloc(r->lines, cap * sizeof *l);
        if (!l)
            return -1;
        r->lines = l;
        r->cap = cap;
    }
    r->lines[r->n] = strdup(line);
    if (!r->lines[r->n])
        return -1;
    r->n++;
    return 0;
}

static int read_reply(reply_t *r)
{
    static char line[ORS_MAX_LINE];
    memset(r, 0, sizeof *r);

    if (net_read_line(&g_rd, line, sizeof line) <= 0)
        return -1;
    const char *m = line;
    if (!strncmp(line, "OK", 2)) {
        r->ok = 1;
        m = line[2] ? line + 3 : "";
    } else if (!strncmp(line, "ERR", 3)) {
        m = line[3] ? line + 4 : "";
    }
    snprintf(r->msg, sizeof r->msg, "%.*s", (int)sizeof r->msg - 1, m);

    for (;;) {
        if (net_read_line(&g_rd, line, sizeof line) <= 0)
            return -1;
        if (!strcmp(line, ORS_END_LINE))
            return 0;
        if (reply_push(r, line) < 0)
            return -1;
    }
}

/* Send one request and wait for the reply. Exits if the server is gone. */
static void request(reply_t *r, const char *fmt, ...)
{
    char buf[ORS_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof buf - 1) {
        memset(r, 0, sizeof *r);
        snprintf(r->msg, sizeof r->msg, "Input too long");
        return;
    }
    buf[n++] = '\n';

    if (net_send_all(g_fd, buf, (size_t)n) < 0 || read_reply(r) < 0) {
        explicit_bzero(buf, sizeof buf);
        fprintf(stderr, "%s\nConnection to the server was lost.%s\n", C_RED, C_RESET);
        exit(1);
    }
    explicit_bzero(buf, sizeof buf);
}

/* ======================================================================
 *  Output
 * ====================================================================== */

static size_t split_fields(char *line, char **out, size_t max)
{
    size_t n = 0;
    char *p = line;
    for (;;) {
        if (n < max)
            out[n++] = p;
        char *bar = strchr(p, ORS_SEP);
        if (!bar)
            break;
        *bar = '\0';
        p = bar + 1;
    }
    return n;
}

static void print_border(const size_t *w, size_t nc)
{
    putchar('+');
    for (size_t i = 0; i < nc; i++) {
        for (size_t j = 0; j < w[i] + 2; j++)
            putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static void print_cells(char **cells, size_t n, const size_t *w, size_t nc, int header)
{
    putchar('|');
    for (size_t i = 0; i < nc; i++) {
        const char *c = i < n ? cells[i] : "";
        size_t len = strlen(c);
        if (header)
            printf("%s", C_BOLD);
        if (len > w[i])
            printf(" %.*s...", (int)w[i] - 3, c);
        else
            printf(" %-*s", (int)w[i], c);
        if (header)
            printf("%s", C_RESET);
        printf(" |");
    }
    putchar('\n');
}

static void render_table(char **rows, size_t nrows)
{
    if (nrows == 0)
        return;

    char **dup = calloc(nrows, sizeof *dup);
    char *(*cells)[MAX_COLS] = calloc(nrows, sizeof *cells);
    size_t *nc = calloc(nrows, sizeof *nc);
    size_t w[MAX_COLS] = { 0 }, ncols = 0;
    if (!dup || !cells || !nc)
        goto out;

    for (size_t r = 0; r < nrows; r++) {
        dup[r] = strdup(rows[r]);
        if (!dup[r])
            goto out;
        nc[r] = split_fields(dup[r], cells[r], MAX_COLS);
        if (r == 0)
            ncols = nc[0];
        for (size_t c = 0; c < nc[r] && c < ncols; c++) {
            size_t len = strlen(cells[r][c]);
            if (len > w[c])
                w[c] = len;
        }
    }
    for (size_t c = 0; c < ncols; c++)
        if (w[c] > MAX_COL_WIDTH)
            w[c] = MAX_COL_WIDTH;

    print_border(w, ncols);
    print_cells(cells[0], nc[0], w, ncols, 1);
    print_border(w, ncols);
    for (size_t r = 1; r < nrows; r++)
        print_cells(cells[r], nc[r], w, ncols, 0);
    if (nrows > 1)
        print_border(w, ncols);
    if (nrows == 1)
        printf("  (no records)\n");
    else
        printf("  %zu row%s\n", nrows - 1, nrows == 2 ? "" : "s");

out:
    if (dup)
        for (size_t r = 0; r < nrows; r++)
            free(dup[r]);
    free(dup);
    free(cells);
    free(nc);
}

static void print_reply(const reply_t *r)
{
    printf("\n%s%s%s %s\n", r->ok ? C_GREEN : C_RED, r->ok ? "[OK]" : "[ERROR]", C_RESET, r->msg);
    if (r->n == 0)
        return;

    char **tbl = malloc(r->n * sizeof *tbl);
    if (!tbl)
        return;
    size_t tn = 0;
    for (size_t i = 0; i < r->n; i++) {
        char *l = r->lines[i];
        if (l[0] == '#') {
            render_table(tbl, tn);
            tn = 0;
            printf("\n%s== %s ==%s\n", C_BOLD, l + 1, C_RESET);
        } else {
            tbl[tn++] = l[0] == '.' ? l + 1 : l;
        }
    }
    render_table(tbl, tn);
    free(tbl);
}

static void do_request(const char *fmt, ...)
{
    char buf[ORS_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    reply_t r;
    request(&r, "%s", buf);
    print_reply(&r);
    reply_free(&r);
}

/* ---- CSV export using raw system calls ---- */

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, s, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

static int write_csv_field(int fd, const char *f)
{
    if (!strpbrk(f, ",\"")) {
        return write_all(fd, f, strlen(f));
    }
    if (write_all(fd, "\"", 1) < 0)
        return -1;
    for (; *f; f++) {
        if (*f == '"' && write_all(fd, "\"", 1) < 0)
            return -1;
        if (write_all(fd, f, 1) < 0)
            return -1;
    }
    return write_all(fd, "\"", 1);
}

static void save_csv(const reply_t *r)
{
    char path[256];
    prompt_text("File name (e.g. report.csv): ", path, sizeof path, 1);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("%sCannot create %s: %s%s\n", C_RED, path, strerror(errno), C_RESET);
        return;
    }
    int rc = 0;
    for (size_t i = 0; i < r->n && rc == 0; i++) {
        const char *l = r->lines[i];
        if (l[0] == '#') {
            rc |= write_all(fd, "\n# ", 3);
            rc |= write_all(fd, l + 1, strlen(l + 1));
            rc |= write_all(fd, "\n", 1);
            continue;
        }
        char *copy = strdup(l[0] == '.' ? l + 1 : l);
        if (!copy) {
            rc = -1;
            break;
        }
        char *cells[MAX_COLS];
        size_t nc = split_fields(copy, cells, MAX_COLS);
        for (size_t c = 0; c < nc && rc == 0; c++) {
            if (c)
                rc |= write_all(fd, ",", 1);
            rc |= write_csv_field(fd, cells[c]);
        }
        rc |= write_all(fd, "\n", 1);
        free(copy);
    }
    if (close(fd) < 0)
        rc = -1;
    if (rc == 0)
        printf("%sSaved %zu line(s) to %s%s\n", C_GREEN, r->n, path, C_RESET);
    else
        printf("%sError while writing %s: %s%s\n", C_RED, path, strerror(errno), C_RESET);
}

/* ======================================================================
 *  Shared screens
 * ====================================================================== */

static void ui_search_products(void)
{
    char term[64];
    prompt_text("Search for (name / category / description): ", term, sizeof term, 1);
    do_request("LIST_PRODUCTS|%s", term);
}

static void ui_view_product(void)
{
    long id = prompt_long("Product ID: ", 1, 2147483647L);
    do_request("VIEW_PRODUCT|%ld", id);
}

static void ui_order_details(void)
{
    long id = prompt_long("Order ID: ", 1, 2147483647L);
    do_request("ORDER_DETAILS|%ld", id);
}

static void ui_change_password(void)
{
    char oldpw[80], newpw[80], again[80];
    prompt_secret("Current password: ", oldpw, sizeof oldpw);
    prompt_secret("New password (8+ chars, letters and digits): ", newpw, sizeof newpw);
    prompt_secret("Repeat new password: ", again, sizeof again);
    if (strcmp(newpw, again) != 0)
        printf("%sPasswords do not match.%s\n", C_RED, C_RESET);
    else
        do_request("CHANGE_PASSWORD|%s|%s", oldpw, newpw);
    explicit_bzero(oldpw, sizeof oldpw);
    explicit_bzero(newpw, sizeof newpw);
    explicit_bzero(again, sizeof again);
}

static void print_header(const char *title, const char *who)
{
    printf("\n%s==============================================================%s\n", C_CYAN, C_RESET);
    printf("%s  %s%s", C_BOLD, title, C_RESET);
    if (who)
        printf("   (logged in as %s)", who);
    printf("\n%s==============================================================%s\n", C_CYAN, C_RESET);
}

/* ======================================================================
 *  Customer
 * ====================================================================== */

static void ui_checkout(void)
{
    reply_t cart;
    request(&cart, "CART_VIEW");
    print_reply(&cart);
    int empty = !cart.ok || cart.n == 0;
    reply_free(&cart);
    if (empty)
        return;

    printf("\nPayment method:\n  1. Credit / debit card\n  2. UPI\n  3. Cash on delivery\n  0. Cancel\n");
    long m = prompt_long("Choose: ", 0, 3);
    if (m == 0)
        return;

    char card[40], exp[8], cvv[8], vpa[64];
    if (m == 1) {
        prompt_text("Card number: ", card, sizeof card, 1);
        prompt_text("Expiry (MM/YY): ", exp, sizeof exp, 1);
        prompt_secret("CVV: ", cvv, sizeof cvv);
    } else if (m == 2) {
        prompt_text("UPI ID (e.g. name@okbank): ", vpa, sizeof vpa, 1);
    }

    if (!prompt_yes("Place the order now?")) {
        printf("Checkout cancelled.\n");
    } else {
        printf("Processing payment, please wait...\n");
        if (m == 1)
            do_request("CHECKOUT|CARD|%s|%s|%s", card, exp, cvv);
        else if (m == 2)
            do_request("CHECKOUT|UPI|%s", vpa);
        else
            do_request("CHECKOUT|COD");
    }
    if (m == 1) {
        explicit_bzero(card, sizeof card);
        explicit_bzero(cvv, sizeof cvv);
    }
}

static void ui_update_profile(void)
{
    static const char *fields[] = { NULL, "full_name", "email", "phone", "address" };
    printf("\nUpdate:\n  1. Full name\n  2. E-mail\n  3. Phone\n  4. Shipping address\n  0. Back\n");
    long f = prompt_long("Choose: ", 0, 4);
    if (f == 0)
        return;
    char value[256];
    prompt_text("New value: ", value, sizeof value, 1);
    do_request("UPDATE_PROFILE|%s|%s", fields[f], value);
}

static void customer_menu(const char *user)
{
    for (;;) {
        print_header("CUSTOMER MENU", user);
        printf("  Shopping                      Orders\n");
        printf("   1. Browse all products         8. My orders\n");
        printf("   2. Search products             9. Track an order\n");
        printf("   3. Product details            10. Cancel an order\n");
        printf("   4. Add to cart               Account\n");
        printf("   5. View cart                  11. My profile\n");
        printf("   6. Change quantity / remove   12. Update profile / address\n");
        printf("   7. Checkout & pay             13. Change password\n");
        printf("                                  0. Logout\n");
        long c = prompt_long("\nChoose an option: ", 0, 13);
        char buf[16];

        switch (c) {
        case 0:
            do_request("LOGOUT");
            return;
        case 1: do_request("LIST_PRODUCTS"); break;
        case 2: ui_search_products(); break;
        case 3: ui_view_product(); break;
        case 4: {
            long id = prompt_long("Product ID: ", 1, 2147483647L);
            long q  = prompt_long("Quantity: ", 1, 100);
            do_request("CART_ADD|%ld|%ld", id, q);
            break;
        }
        case 5: do_request("CART_VIEW"); break;
        case 6: {
            long id = prompt_long("Product ID in cart: ", 1, 2147483647L);
            long q  = prompt_long("New quantity (0 to remove): ", 0, 100);
            do_request("CART_UPDATE|%ld|%ld", id, q);
            break;
        }
        case 7: ui_checkout(); break;
        case 8: do_request("MY_ORDERS"); break;
        case 9: ui_order_details(); break;
        case 10: {
            long id = prompt_long("Order ID to cancel: ", 1, 2147483647L);
            snprintf(buf, sizeof buf, "%ld", id);
            if (prompt_yes("Really cancel this order?"))
                do_request("CANCEL_ORDER|%s", buf);
            break;
        }
        case 11: do_request("PROFILE"); break;
        case 12: ui_update_profile(); break;
        case 13: ui_change_password(); break;
        }
        pause_enter();
    }
}

/* ======================================================================
 *  Admin
 * ====================================================================== */

static void ui_add_product(void)
{
    char name[128], cat[64], price[32], desc[256];
    prompt_text("Product name: ", name, sizeof name, 1);
    prompt_text("Category: ", cat, sizeof cat, 1);
    prompt_text("Price (Rs): ", price, sizeof price, 1);
    long stock   = prompt_long("Opening stock: ", 0, 1000000);
    long reorder = prompt_long("Reorder level (low-stock alert at or below): ", 0, 1000000);
    prompt_text("Description (optional): ", desc, sizeof desc, 0);
    do_request("ADD_PRODUCT|%s|%s|%s|%ld|%ld|%s", name, cat, price, stock, reorder, desc);
}

static void ui_update_product(void)
{
    static const char *fields[] = { NULL, "name", "category", "description", "price", "reorder_level", "is_active" };
    long id = prompt_long("Product ID: ", 1, 2147483647L);
    printf("Field to change:\n  1. Name\n  2. Category\n  3. Description\n  4. Price\n"
           "  5. Reorder level\n  6. Active (1 = on sale, 0 = hidden)\n  0. Back\n");
    long f = prompt_long("Choose: ", 0, 6);
    if (f == 0)
        return;
    char value[256];
    prompt_text("New value: ", value, sizeof value, f != 3);
    do_request("UPDATE_PRODUCT|%ld|%s|%s", id, fields[f], value);
}

static void ui_update_order_status(void)
{
    static const char *st[] = { NULL, "CONFIRMED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED" };
    long id = prompt_long("Order ID: ", 1, 2147483647L);
    printf("New status:\n  1. CONFIRMED\n  2. SHIPPED\n  3. OUT_FOR_DELIVERY\n  4. DELIVERED\n"
           "  5. CANCELLED (restocks items)\n  0. Back\n");
    long s = prompt_long("Choose: ", 0, 5);
    if (s == 0)
        return;
    char note[128];
    prompt_text("Note for tracking history (optional): ", note, sizeof note, 0);
    do_request("UPDATE_ORDER_STATUS|%ld|%s|%s", id, st[s], note);
}

static void ui_list_orders(void)
{
    static const char *st[] = { "", "PLACED", "CONFIRMED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED" };
    printf("Show:\n  0. All orders\n  1. PLACED\n  2. CONFIRMED\n  3. SHIPPED\n  4. OUT_FOR_DELIVERY\n"
           "  5. DELIVERED\n  6. CANCELLED\n");
    long s = prompt_long("Choose: ", 0, 6);
    do_request("LIST_ORDERS|%s", st[s]);
}

static void reports_menu(void)
{
    for (;;) {
        print_header("REPORTS & ANALYSIS", NULL);
        printf("   1. Business summary (revenue, orders, customers)\n");
        printf("   2. Daily sales\n");
        printf("   3. Top-selling products\n");
        printf("   4. Sales by category\n");
        printf("   5. Inventory status / low stock\n");
        printf("   6. Stock movement log\n");
        printf("   0. Back\n");
        long c = prompt_long("\nChoose a report: ", 0, 6);
        if (c == 0)
            return;

        reply_t r;
        switch (c) {
        case 1: request(&r, "REPORT_SUMMARY"); break;
        case 2: request(&r, "REPORT_SALES|%ld", prompt_long("Number of days (1-365): ", 1, 365)); break;
        case 3: request(&r, "REPORT_TOP|%ld", prompt_long("How many products (1-100): ", 1, 100)); break;
        case 4: request(&r, "REPORT_CATEGORY"); break;
        case 5: request(&r, "REPORT_INVENTORY"); break;
        default: {
            long id = prompt_long("Product ID (0 = all products): ", 0, 2147483647L);
            if (id)
                request(&r, "REPORT_STOCK_LOG|%ld", id);
            else
                request(&r, "REPORT_STOCK_LOG");
        }
        }
        print_reply(&r);
        if (r.ok && r.n > 0 && prompt_yes("\nSave this report as a CSV file?"))
            save_csv(&r);
        reply_free(&r);
        pause_enter();
    }
}

static void admin_menu(const char *user)
{
    for (;;) {
        print_header("ADMIN MENU", user);
        printf("  Inventory                       Orders\n");
        printf("   1. View inventory                8. View orders\n");
        printf("   2. Search inventory              9. Order details\n");
        printf("   3. Add product                  10. Update order status\n");
        printf("   4. Update product              Customers\n");
        printf("   5. Restock product              11. List / search customers\n");
        printf("   6. Adjust stock count           12. Enable / disable customer\n");
        printf("   7. Delete product              System\n");
        printf("                                   13. Reports & analysis\n");
        printf("                                   14. Server statistics\n");
        printf("                                   15. Change password\n");
        printf("                                    0. Logout\n");
        long c = prompt_long("\nChoose an option: ", 0, 15);
        char text[128];

        switch (c) {
        case 0:
            do_request("LOGOUT");
            return;
        case 1: do_request("LIST_PRODUCTS"); break;
        case 2: ui_search_products(); break;
        case 3: ui_add_product(); break;
        case 4: ui_update_product(); break;
        case 5: {
            long id = prompt_long("Product ID: ", 1, 2147483647L);
            long q  = prompt_long("Units received: ", 1, 1000000);
            prompt_text("Note (optional, e.g. supplier invoice): ", text, sizeof text, 0);
            do_request("RESTOCK|%ld|%ld|%s", id, q, text);
            break;
        }
        case 6: {
            long id = prompt_long("Product ID: ", 1, 2147483647L);
            long q  = prompt_long("Actual stock count: ", 0, 1000000);
            prompt_text("Reason (e.g. damaged goods): ", text, sizeof text, 0);
            do_request("ADJUST_STOCK|%ld|%ld|%s", id, q, text);
            break;
        }
        case 7: {
            long id = prompt_long("Product ID to delete: ", 1, 2147483647L);
            if (prompt_yes("Delete this product from the store?"))
                do_request("DELETE_PRODUCT|%ld", id);
            break;
        }
        case 8: ui_list_orders(); break;
        case 9: ui_order_details(); break;
        case 10: ui_update_order_status(); break;
        case 11:
            prompt_text("Search (blank = all customers): ", text, sizeof text, 0);
            do_request("LIST_CUSTOMERS|%s", text);
            break;
        case 12: {
            long id = prompt_long("Customer ID: ", 1, 2147483647L);
            long a  = prompt_long("1 = enable, 0 = disable: ", 0, 1);
            do_request("SET_USER_ACTIVE|%ld|%ld", id, a);
            break;
        }
        case 13: reports_menu(); continue;
        case 14: do_request("SERVER_STATS"); break;
        case 15: ui_change_password(); break;
        }
        pause_enter();
    }
}

/* ======================================================================
 *  Login / registration
 * ====================================================================== */

static void ui_login(void)
{
    char user[64], pw[80];
    prompt_text("Username: ", user, sizeof user, 1);
    prompt_secret("Password: ", pw, sizeof pw);

    reply_t r;
    request(&r, "LOGIN|%s|%s", user, pw);
    explicit_bzero(pw, sizeof pw);
    print_reply(&r);

    if (r.ok && r.n >= 2) {
        char *copy = strdup(r.lines[1][0] == '.' ? r.lines[1] + 1 : r.lines[1]);
        char *f[4] = { 0 };
        if (copy && split_fields(copy, f, 4) >= 3) {
            char who[64];
            snprintf(who, sizeof who, "%s", f[1]);
            int admin = !strcmp(f[2], "ADMIN");
            free(copy);
            reply_free(&r);
            if (admin)
                admin_menu(who);
            else
                customer_menu(who);
            return;
        }
        free(copy);
    }
    reply_free(&r);
    pause_enter();
}

static void ui_register(void)
{
    char user[64], pw[80], again[80], name[128], email[128], phone[32], addr[256];
    print_header("NEW CUSTOMER REGISTRATION", NULL);
    prompt_text("Username (3-30 letters/digits/_/.): ", user, sizeof user, 1);
    prompt_secret("Password (8+ chars, letters and digits): ", pw, sizeof pw);
    prompt_secret("Repeat password: ", again, sizeof again);
    if (strcmp(pw, again) != 0) {
        printf("%sPasswords do not match.%s\n", C_RED, C_RESET);
        explicit_bzero(pw, sizeof pw);
        explicit_bzero(again, sizeof again);
        pause_enter();
        return;
    }
    prompt_text("Full name: ", name, sizeof name, 1);
    prompt_text("E-mail: ", email, sizeof email, 1);
    prompt_text("Phone (optional): ", phone, sizeof phone, 0);
    prompt_text("Shipping address: ", addr, sizeof addr, 1);
    do_request("REGISTER|%s|%s|%s|%s|%s|%s", user, pw, name, email, phone, addr);
    explicit_bzero(pw, sizeof pw);
    explicit_bzero(again, sizeof again);
    pause_enter();
}

static void main_menu(void)
{
    for (;;) {
        print_header("ONLINE RETAIL STORE", NULL);
        printf("   1. Login\n");
        printf("   2. Register as a new customer\n");
        printf("   3. Browse products (guest)\n");
        printf("   4. Search products (guest)\n");
        printf("   0. Exit\n");
        long c = prompt_long("\nChoose an option: ", 0, 4);
        switch (c) {
        case 0: return;
        case 1: ui_login(); break;
        case 2: ui_register(); break;
        case 3: do_request("LIST_PRODUCTS"); pause_enter(); break;
        case 4: ui_search_products(); pause_enter(); break;
        }
    }
}

/* ======================================================================
 *  main
 * ====================================================================== */

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void quit_client(void)
{
    restore_tty();
    if (g_fd >= 0) {
        net_send_all(g_fd, "QUIT\n", 5);
        close(g_fd);
    }
    printf("Goodbye!\n");
    exit(0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    char port[16];
    snprintf(port, sizeof port, "%d", ORS_DEFAULT_PORT);

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: %s [host] [port]   (default 127.0.0.1 %d)\n", argv[0], ORS_DEFAULT_PORT);
        return 0;
    }
    if (argc > 1)
        host = argv[1];
    if (argc > 2)
        snprintf(port, sizeof port, "%s", argv[2]);

    g_color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_fd = connect_to(host, port);
    if (g_fd < 0) {
        fprintf(stderr, "%sCould not connect to the ORS server at %s:%s.%s\n"
                        "Is ors_server running? Start it with: ./bin/ors_server\n",
                C_RED, host, port, C_RESET);
        return 1;
    }
    net_reader_init(&g_rd, g_fd);

    reply_t r;
    if (read_reply(&r) < 0) {
        fprintf(stderr, "Server closed the connection.\n");
        return 1;
    }
    print_reply(&r);
    int ok = r.ok;
    reply_free(&r);
    if (!ok)
        return 1;

    main_menu();
    quit_client();
    return 0;
}
