/*
 * payment.c - online payment processing.
 *
 * The real store would call a bank / payment gateway over the network. Here
 * the gateway is simulated by a CHILD PROCESS:
 *
 *   server thread --fork()--> gateway process
 *        ^                          |
 *        +------- pipe() <----------+   (fixed-size reply struct)
 *
 * The parent waits for the reply with poll() and a timeout, kills the child
 * with SIGKILL if it hangs, and always reaps it with waitpid() so no zombie
 * processes are left behind. Because the server is multi-threaded, the child
 * only performs simple, async-signal-safe work (arithmetic, write, _exit).
 *
 * Test data:
 *   CARD 4111 1111 1111 1111  -> approved
 *   CARD 4000 0000 0002 0000  -> declined (any valid card ending in 0000)
 *   UPI  anything@bank        -> approved,  UPI id starting with "fail" -> declined
 *   Amount above Rs 2,00,000  -> declined (transaction limit)
 */
#include "payment.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GATEWAY_TIMEOUT_MS 5000
#define TXN_LIMIT_PAISE    (200000LL * 100)

enum { GW_APPROVED = 1, GW_DECLINED = 2, GW_LIMIT = 3 };

struct gw_reply {
    int           code;
    unsigned long txn;
};

const char *payment_method_name(pay_method_t m)
{
    switch (m) {
    case PAY_CARD: return "CARD";
    case PAY_UPI:  return "UPI";
    default:       return "COD";
    }
}

static int luhn_ok(const char *digits)
{
    int sum = 0, dbl = 0;
    for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
        int v = digits[i] - '0';
        if (dbl) {
            v *= 2;
            if (v > 9)
                v -= 9;
        }
        sum += v;
        dbl = !dbl;
    }
    return sum % 10 == 0;
}

static int expiry_ok(const char *e)
{
    if (strlen(e) != 5 || e[2] != '/' ||
        !isdigit((unsigned char)e[0]) || !isdigit((unsigned char)e[1]) ||
        !isdigit((unsigned char)e[3]) || !isdigit((unsigned char)e[4]))
        return 0;

    int mm = (e[0] - '0') * 10 + (e[1] - '0');
    int yy = (e[3] - '0') * 10 + (e[4] - '0');
    if (mm < 1 || mm > 12)
        return 0;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int cy = tm.tm_year % 100, cm = tm.tm_mon + 1;
    return yy > cy || (yy == cy && mm >= cm);
}

static int all_digits(const char *s, size_t min, size_t max)
{
    size_t n = strlen(s);
    if (n < min || n > max)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static int valid_vpa(const char *v)
{
    const char *at = strchr(v, '@');
    if (!at || strchr(at + 1, '@') || strlen(v) >= 64)
        return 0;
    if (at - v < 2 || strlen(at + 1) < 2)
        return 0;
    for (const char *p = v; p < at; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
            return 0;
    for (const char *p = at + 1; *p; p++)
        if (!isalpha((unsigned char)*p))
            return 0;
    return 1;
}

int payment_parse(int argc, char **argv, payment_request_t *req, char *err, size_t errsz)
{
    memset(req, 0, sizeof *req);

    if (argc < 1 || argv[0][0] == '\0') {
        snprintf(err, errsz, "Payment method required (CARD, UPI or COD)");
        return -1;
    }

    if (!strcasecmp(argv[0], "CARD")) {
        if (argc < 4) {
            snprintf(err, errsz, "Card payment needs card number, expiry (MM/YY) and CVV");
            return -1;
        }
        size_t n = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (isdigit((unsigned char)*p)) {
                if (n >= sizeof req->card - 1) {
                    snprintf(err, errsz, "Invalid card number");
                    return -1;
                }
                req->card[n++] = *p;
            } else if (*p != ' ' && *p != '-') {
                snprintf(err, errsz, "Card number may contain only digits");
                return -1;
            }
        }
        req->card[n] = '\0';
        if (n < 13 || n > 19 || !luhn_ok(req->card)) {
            snprintf(err, errsz, "Invalid card number (failed checksum)");
            payment_wipe(req);
            return -1;
        }
        if (!expiry_ok(argv[2])) {
            snprintf(err, errsz, "Card expiry must be a current or future date in MM/YY format");
            payment_wipe(req);
            return -1;
        }
        if (!all_digits(argv[3], 3, 4)) {
            snprintf(err, errsz, "CVV must be 3 or 4 digits");
            payment_wipe(req);
            return -1;
        }
        req->method = PAY_CARD;
        snprintf(req->expiry, sizeof req->expiry, "%s", argv[2]);
        snprintf(req->cvv, sizeof req->cvv, "%s", argv[3]);
        snprintf(req->masked, sizeof req->masked, "Card **** %s", req->card + n - 4);
        return 0;
    }

    if (!strcasecmp(argv[0], "UPI")) {
        if (argc < 2 || !valid_vpa(argv[1])) {
            snprintf(err, errsz, "Enter a valid UPI ID, e.g. name@okbank");
            return -1;
        }
        req->method = PAY_UPI;
        snprintf(req->vpa, sizeof req->vpa, "%s", argv[1]);
        const char *at = strchr(req->vpa, '@');
        snprintf(req->masked, sizeof req->masked, "UPI %.2s****%s", req->vpa, at);
        return 0;
    }

    if (!strcasecmp(argv[0], "COD")) {
        req->method = PAY_COD;
        snprintf(req->masked, sizeof req->masked, "Cash on delivery");
        return 0;
    }

    snprintf(err, errsz, "Unknown payment method '%.20s' (use CARD, UPI or COD)", argv[0]);
    return -1;
}

/* Runs inside the gateway child process: pure computation only. */
static int gateway_decision(const payment_request_t *req, long long amount)
{
    if (amount > TXN_LIMIT_PAISE)
        return GW_LIMIT;
    if (req->method == PAY_CARD) {
        size_t n = strlen(req->card);
        if (n >= 4 && strcmp(req->card + n - 4, "0000") == 0)
            return GW_DECLINED;
    }
    if (req->method == PAY_UPI && strncasecmp(req->vpa, "fail", 4) == 0)
        return GW_DECLINED;
    return GW_APPROVED;
}

int payment_process(const payment_request_t *req, long long amount, payment_result_t *res)
{
    memset(res, 0, sizeof *res);

    if (req->method == PAY_COD) {
        res->approved = 1;
        res->pending  = 1;
        snprintf(res->reference, sizeof res->reference, "COD%010ld", (long)time(NULL) % 10000000000L);
        snprintf(res->message, sizeof res->message, "Cash on delivery - pay when the order arrives");
        return 0;
    }

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        snprintf(res->message, sizeof res->message, "Payment gateway unavailable");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(res->message, sizeof res->message, "Payment gateway unavailable");
        return -1;
    }

    if (pid == 0) {
        /* ---------------- child: simulated bank gateway ---------------- */
        close(pfd[0]);
        struct gw_reply rep;
        memset(&rep, 0, sizeof rep);
        poll(NULL, 0, 200 + (int)(getpid() % 300));      /* network latency */
        rep.code = gateway_decision(req, amount);
        rep.txn  = ((unsigned long)time(NULL) * 1000UL + (unsigned long)getpid()) % 10000000000UL;
        ssize_t w = write(pfd[1], &rep, sizeof rep);     /* atomic: < PIPE_BUF */
        (void)w;
        _exit(0);
    }

    /* ---------------- parent: wait for the gateway's answer ---------------- */
    close(pfd[1]);

    struct gw_reply rep;
    memset(&rep, 0, sizeof rep);
    struct pollfd p = { .fd = pfd[0], .events = POLLIN };
    int pr;
    do {
        pr = poll(&p, 1, GATEWAY_TIMEOUT_MS);
    } while (pr < 0 && errno == EINTR);

    ssize_t got = -1;
    if (pr > 0) {
        do {
            got = read(pfd[0], &rep, sizeof rep);
        } while (got < 0 && errno == EINTR);
    } else {
        kill(pid, SIGKILL);                 /* gateway hung: terminate it */
    }
    close(pfd[0]);

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (got != (ssize_t)sizeof rep) {
        snprintf(res->message, sizeof res->message, "Payment gateway timed out");
        return -1;
    }

    snprintf(res->reference, sizeof res->reference, "TXN%010lu", rep.txn);
    switch (rep.code) {
    case GW_APPROVED:
        res->approved = 1;
        snprintf(res->message, sizeof res->message, "Payment approved");
        break;
    case GW_LIMIT:
        snprintf(res->message, sizeof res->message,
                 "Amount exceeds the per-transaction limit of Rs 2,00,000");
        break;
    default:
        snprintf(res->message, sizeof res->message, "Payment declined by the bank");
        break;
    }
    return 0;
}

void payment_wipe(payment_request_t *req)
{
    explicit_bzero(req->card, sizeof req->card);
    explicit_bzero(req->cvv, sizeof req->cvv);
    explicit_bzero(req->expiry, sizeof req->expiry);
}
