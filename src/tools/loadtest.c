/*
 * loadtest.c - concurrency test for real-time inventory.
 *
 * N customer PROCESSES (fork) log in, put the same product in their carts and
 * then all press "checkout" at the same instant. The server must sell exactly
 * as many units as are in stock - never more.
 *
 * OS concepts: fork(), pipes used as a start barrier (every child blocks in
 * read() until the parent closes the write end), waitpid() and exit codes.
 *
 * Usage:
 *   ors_loadtest -i PRODUCT_ID [-n CLIENTS] [-q QTY] [-H HOST] [-p PORT]
 *                [-s STOCK -a ADMIN_USER -w ADMIN_PASSWORD]
 *
 * With -s the tool first logs in as admin and sets the product's stock to
 * STOCK, so the expected number of successful orders is known in advance.
 */
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"

typedef struct {
    int          fd;
    net_reader_t rd;
} conn_t;

static const char *g_host = "127.0.0.1";
static const char *g_port = "8080";

static int conn_open(conn_t *c)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, g_port, &hints, &res) != 0)
        return -1;
    c->fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->fd < 0)
            continue;
        if (connect(c->fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(c->fd);
        c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0)
        return -1;
    net_reader_init(&c->rd, c->fd);
    return 0;
}

/* Read one reply. Returns 1 for OK, 0 for ERR, -1 on connection failure.
 * The status message is copied to msg. Body lines are skipped. */
static int conn_reply(conn_t *c, char *msg, size_t msgsz)
{
    char line[ORS_MAX_LINE];
    if (net_read_line(&c->rd, line, sizeof line) <= 0)
        return -1;
    int ok = strncmp(line, "OK", 2) == 0;
    const char *m = line;
    if (ok)
        m = line[2] ? line + 3 : "";
    else if (!strncmp(line, "ERR", 3))
        m = line[3] ? line + 4 : "";
    snprintf(msg, msgsz, "%.*s", (int)msgsz - 1, m);
    for (;;) {
        if (net_read_line(&c->rd, line, sizeof line) <= 0)
            return -1;
        if (!strcmp(line, ORS_END_LINE))
            return ok;
    }
}

static int conn_cmd(conn_t *c, char *msg, size_t msgsz, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    if (net_send_all(c->fd, buf, (size_t)n) < 0)
        return -1;
    return conn_reply(c, msg, msgsz);
}

/* Exit codes of a child process */
enum { EX_ORDERED = 0, EX_REJECTED = 1, EX_SETUP = 2, EX_CONN = 3 };

static int child_main(int id, long pid, long qty, int ready_fd, int start_fd)
{
    conn_t c;
    char msg[600];
    int setup_ok = 0;

    if (conn_open(&c) == 0 && conn_reply(&c, msg, sizeof msg) == 1) {
        /* registration fails harmlessly if the user already exists */
        conn_cmd(&c, msg, sizeof msg, "REGISTER|ltuser%d|Load@1234|Load Tester %d|ltuser%d@example.com||Test Lane %d",
                 id, id, id, id);
        if (conn_cmd(&c, msg, sizeof msg, "LOGIN|ltuser%d|Load@1234", id) == 1 &&
            conn_cmd(&c, msg, sizeof msg, "UPDATE_PROFILE|address|%d Test Lane, Test City", id) == 1 &&
            conn_cmd(&c, msg, sizeof msg, "CART_CLEAR") == 1 &&
            conn_cmd(&c, msg, sizeof msg, "CART_ADD|%ld|%ld", pid, qty) == 1)
            setup_ok = 1;
        else
            fprintf(stderr, "[client %2d] setup failed: %s\n", id, msg);
    } else {
        fprintf(stderr, "[client %2d] cannot connect to the server\n", id);
        c.fd = -1;
    }

    /* tell the parent we are ready, then wait for the start signal */
    char b = 'r';
    ssize_t w = write(ready_fd, &b, 1);
    (void)w;
    close(ready_fd);
    while (read(start_fd, &b, 1) < 0 && errno == EINTR)
        ;                                   /* returns 0 (EOF) when parent says GO */
    close(start_fd);

    if (!setup_ok) {
        if (c.fd >= 0)
            close(c.fd);
        return c.fd >= 0 ? EX_SETUP : EX_CONN;
    }

    int rc = conn_cmd(&c, msg, sizeof msg, "CHECKOUT|COD");
    printf("[client %2d] %s: %s\n", id, rc == 1 ? "ORDER PLACED" : "rejected", msg);
    conn_cmd(&c, msg, sizeof msg, "QUIT");
    close(c.fd);
    return rc == 1 ? EX_ORDERED : rc == 0 ? EX_REJECTED : EX_CONN;
}

static int admin_set_stock(const char *user, const char *pw, long pid, long stock)
{
    conn_t c;
    char msg[600];
    if (conn_open(&c) < 0 || conn_reply(&c, msg, sizeof msg) != 1) {
        fprintf(stderr, "Cannot connect to %s:%s\n", g_host, g_port);
        return -1;
    }
    if (conn_cmd(&c, msg, sizeof msg, "LOGIN|%s|%s", user, pw) != 1) {
        fprintf(stderr, "Admin login failed: %s\n", msg);
        close(c.fd);
        return -1;
    }
    int rc = conn_cmd(&c, msg, sizeof msg, "ADJUST_STOCK|%ld|%ld|Load test setup", pid, stock);
    printf("Setup: %s\n", msg);
    conn_cmd(&c, msg, sizeof msg, "QUIT");
    close(c.fd);
    return rc == 1 ? 0 : -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -i PRODUCT_ID [-n CLIENTS] [-q QTY] [-H HOST] [-p PORT]\n"
            "          [-s STOCK -a ADMIN_USER -w ADMIN_PASSWORD]\n"
            "Example: %s -i 3 -n 20 -s 5 -a admin -w 'Admin@123'\n"
            "         (20 customers race for 5 units: exactly 5 orders must succeed)\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    long pid = 0, qty = 1, stock = -1;
    int n = 10, opt;
    const char *admin = NULL, *admin_pw = NULL;

    while ((opt = getopt(argc, argv, "i:n:q:H:p:s:a:w:h")) != -1) {
        switch (opt) {
        case 'i': pid = atol(optarg); break;
        case 'n': n = atoi(optarg); break;
        case 'q': qty = atol(optarg); break;
        case 'H': g_host = optarg; break;
        case 'p': g_port = optarg; break;
        case 's': stock = atol(optarg); break;
        case 'a': admin = optarg; break;
        case 'w': admin_pw = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (pid <= 0 || n < 1 || n > 200 || qty < 1) {
        usage(argv[0]);
        return 1;
    }
    if (stock >= 0) {
        if (!admin || !admin_pw) {
            fprintf(stderr, "-s needs -a ADMIN_USER and -w ADMIN_PASSWORD\n");
            return 1;
        }
        if (admin_set_stock(admin, admin_pw, pid, stock) < 0)
            return 1;
    }

    int ready[2], start[2];
    if (pipe(ready) < 0 || pipe(start) < 0) {
        perror("pipe");
        return 1;
    }

    printf("Starting %d customer processes, each buying %ld unit(s) of product %ld...\n", n, qty, pid);
    fflush(stdout);

    pid_t *kids = calloc((size_t)n, sizeof *kids);
    if (!kids)
        return 1;
    for (int i = 0; i < n; i++) {
        kids[i] = fork();
        if (kids[i] < 0) {
            perror("fork");
            n = i;
            break;
        }
        if (kids[i] == 0) {
            close(ready[0]);
            close(start[1]);
            _exit(child_main(i + 1, pid, qty, ready[1], start[0]));
        }
    }
    close(ready[1]);
    close(start[0]);

    /* barrier: wait until every child has logged in and filled its cart */
    char b;
    int got = 0;
    while (got < n) {
        ssize_t k = read(ready[0], &b, 1);
        if (k <= 0)
            break;
        got++;
    }
    close(ready[0]);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    printf("All %d clients ready - GO!\n", got);
    fflush(stdout);
    close(start[1]);                     /* every child's read() returns now */

    int ordered = 0, rejected = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        int status;
        while (waitpid(kids[i], &status, 0) < 0 && errno == EINTR)
            ;
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : EX_CONN;
        if (code == EX_ORDERED)       ordered++;
        else if (code == EX_REJECTED) rejected++;
        else                          failed++;
    }
    gettimeofday(&t1, NULL);
    free(kids);

    long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
    printf("\n============ RESULT ============\n");
    printf("Clients            : %d\n", n);
    printf("Orders placed      : %d (%ld unit(s) sold)\n", ordered, ordered * qty);
    printf("Rejected (no stock): %d\n", rejected);
    printf("Setup/conn errors  : %d\n", failed);
    printf("Time for checkouts : %ld ms\n", ms);
    if (stock >= 0) {
        long expect = stock / qty < n ? stock / qty : n;
        printf("Expected orders    : %ld -> %s\n", expect,
               ordered == expect ? "PASS (no overselling)" : "CHECK RESULTS");
    }
    return 0;
}
