/*
 * server.c - ORS multi-threaded TCP server.
 *
 * Linux systems programming concepts used here:
 *   - BSD sockets: socket/bind/listen/accept4, SO_REUSEADDR, SO_RCVTIMEO
 *   - POSIX threads: one detached worker thread per connected client
 *   - pthread mutex + condition variable: shared statistics / client registry,
 *     waiting for all clients to finish during shutdown
 *   - POSIX counting semaphore: admission control (at most max_clients)
 *   - Signals: SIGINT/SIGTERM (graceful shutdown), SIGHUP (reopen log),
 *     SIGUSR1 (dump statistics), SIGPIPE ignored. Signals are blocked in all
 *     threads and handled synchronously by one dedicated thread via sigwait().
 *   - Self-pipe trick: the signal thread wakes the poll()-based accept loop
 *   - fcntl() record lock on the PID file: only one server instance can run
 *   - Daemonisation: fork, setsid, fork, redirect stdio to /dev/null
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mysql.h>

#include "auth.h"
#include "config.h"
#include "db.h"
#include "handlers.h"
#include "logger.h"
#include "net.h"
#include "protocol.h"
#include "resp.h"
#include "server.h"

typedef struct {
    int                fd;
    struct sockaddr_in addr;
} client_arg_t;

static ors_config_t g_cfg;
static atomic_int   g_running = 1;
static int          g_wake[2] = { -1, -1 };     /* self-pipe: signal thread -> main */
static sem_t        g_slots;                     /* free client slots               */
static int          g_pid_fd = -1;

/* --- shared state, protected by g_lock --- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_all_gone = PTHREAD_COND_INITIALIZER;
static server_stats_t  g_stats;
static int            *g_client_fds;             /* registry used at shutdown */

/* ----------------------------------------------------------------------
 *  Statistics (called from handler threads)
 * ---------------------------------------------------------------------- */

void stats_snapshot(server_stats_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}

void stats_request(void)
{
    pthread_mutex_lock(&g_lock);
    g_stats.total_requests++;
    pthread_mutex_unlock(&g_lock);
}

void stats_order_placed(void)
{
    pthread_mutex_lock(&g_lock);
    g_stats.orders_placed++;
    pthread_mutex_unlock(&g_lock);
}

void stats_failed_login(void)
{
    pthread_mutex_lock(&g_lock);
    g_stats.failed_logins++;
    pthread_mutex_unlock(&g_lock);
}

static void log_stats(void)
{
    server_stats_t st;
    stats_snapshot(&st);
    LOG_INFO("STATS: clients %d/%d, connections %lu, rejected %lu, requests %lu, orders %lu, failed logins %lu",
             st.active_clients, st.max_clients, st.total_connections, st.rejected_busy,
             st.total_requests, st.orders_placed, st.failed_logins);
}

/* ----------------------------------------------------------------------
 *  Client registry
 * ---------------------------------------------------------------------- */

static int registry_add(int fd)
{
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_cfg.max_clients; i++) {
        if (g_client_fds[i] == -1) {
            g_client_fds[i] = fd;
            g_stats.active_clients++;
            g_stats.total_connections++;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static void registry_remove(int fd)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_cfg.max_clients; i++) {
        if (g_client_fds[i] == fd) {
            g_client_fds[i] = -1;
            g_stats.active_clients--;
            break;
        }
    }
    if (g_stats.active_clients == 0)
        pthread_cond_broadcast(&g_all_gone);
    pthread_mutex_unlock(&g_lock);
}

/* ----------------------------------------------------------------------
 *  Worker thread: serves one client for the lifetime of its connection
 * ---------------------------------------------------------------------- */

static void *client_thread(void *arg)
{
    client_arg_t ca = *(client_arg_t *)arg;
    free(arg);

    session_t s;
    memset(&s, 0, sizeof s);
    s.fd = ca.fd;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &ca.addr.sin_addr, ip, sizeof ip);
    snprintf(s.peer, sizeof s.peer, "%s:%u", ip, (unsigned)ntohs(ca.addr.sin_port));
    LOG_INFO("Client connected: %s", s.peer);

    resp_t r;
    resp_init(&r);

    s.db = db_connect(&g_cfg);
    if (!s.db) {
        resp_err(&r, "Store database is unavailable. Please try again later.");
        resp_send(s.fd, &r);
        resp_free(&r);
        goto done;
    }

    resp_ok(&r, "Welcome to %s v%s. Type HELP for commands.", ORS_APP_NAME, ORS_VERSION);
    resp_send(s.fd, &r);
    resp_free(&r);

    net_reader_t *rd = malloc(sizeof *rd);
    char *line = malloc(ORS_MAX_LINE);
    if (!rd || !line) {
        free(rd);
        free(line);
        goto done;
    }
    net_reader_init(rd, s.fd);

    while (atomic_load(&g_running) && !s.quit) {
        int rc = net_read_line(rd, line, ORS_MAX_LINE);
        if (rc == 0)
            break;                                   /* client closed */
        if (rc == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                LOG_INFO("Client %s idle for %d s - disconnecting", s.peer, g_cfg.idle_timeout);
            break;
        }

        resp_init(&r);
        if (rc == -2)
            resp_err(&r, "Request too long (max %d bytes)", ORS_MAX_LINE - 1);
        else
            handle_request(&s, line, &r);
        int sent = resp_send(s.fd, &r);
        resp_free(&r);
        if (sent < 0)
            break;
    }
    explicit_bzero(line, ORS_MAX_LINE);              /* may have held a password */
    free(line);
    free(rd);

done:
    if (s.db)
        mysql_close(s.db);
    mysql_thread_end();
    LOG_INFO("Client disconnected: %s (%s)", s.peer, s.username[0] ? s.username : "not logged in");

    registry_remove(s.fd);
    close(s.fd);
    sem_post(&g_slots);                              /* free the slot */
    return NULL;
}

/* ----------------------------------------------------------------------
 *  Signal thread
 * ---------------------------------------------------------------------- */

static void *signal_thread(void *arg)
{
    sigset_t *set = arg;
    int sig;

    for (;;) {
        if (sigwait(set, &sig) != 0)
            continue;
        if (sig == SIGUSR1) {
            log_stats();
            continue;
        }
        if (sig == SIGHUP) {
            log_reopen();
            LOG_INFO("SIGHUP received - log file reopened");
            continue;
        }
        LOG_INFO("Received %s - shutting down gracefully", sig == SIGINT ? "SIGINT" : "SIGTERM");
        atomic_store(&g_running, 0);
        char b = 1;
        ssize_t w = write(g_wake[1], &b, 1);
        (void)w;
        return NULL;
    }
}

/* ----------------------------------------------------------------------
 *  Setup helpers
 * ---------------------------------------------------------------------- */

static void make_parent_dirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int acquire_pid_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG_ERROR("Cannot open pid file %s: %s", path, strerror(errno));
        return -1;
    }
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        char old[32] = "";
        ssize_t k = pread(fd, old, sizeof old - 1, 0);
        if (k > 0)
            old[strcspn(old, "\n")] = '\0';
        LOG_ERROR("Another ORS server is already running (pid %s, lock on %s)", old[0] ? old : "?", path);
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
        ssize_t w = write(fd, buf, (size_t)n);
        (void)w;
    }
    g_pid_fd = fd;           /* keep it open: the lock lives as long as we do */
    return 0;
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);                    /* parent returns to the shell */
    if (setsid() < 0)                /* new session, no controlling tty */
        return -1;

    signal(SIGHUP, SIG_IGN);
    pid = fork();                    /* never re-acquire a terminal */
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    signal(SIGHUP, SIG_DFL);

    umask(027);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
    return 0;
}

static int create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 64) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* Make sure the schema exists and at least one admin account is present. */
static int bootstrap(void)
{
    MYSQL *db = db_connect(&g_cfg);
    if (!db) {
        LOG_ERROR("Cannot reach MySQL - check the db_* settings and that mysqld is running");
        return -1;
    }

    int rc = 0;
    long long admins = db_scalar(db, -1, "SELECT COUNT(*) FROM users WHERE role = 'ADMIN'");
    if (admins < 0) {
        LOG_ERROR("Database schema missing - load sql/schema.sql first");
        rc = -1;
    } else if (admins == 0) {
        char hash[256];
        if (!g_cfg.admin_password[0]) {
            LOG_ERROR("No admin account exists and admin_password is not set in the config file");
            rc = -1;
        } else if (auth_hash_password(g_cfg.admin_password, hash, sizeof hash) < 0) {
            LOG_ERROR("Could not hash the bootstrap admin password");
            rc = -1;
        } else {
            char *u = db_escape(db, g_cfg.admin_user);
            char *h = db_escape(db, hash);
            if (!u || !h ||
                db_exec(db, "INSERT INTO users (username, password_hash, role, full_name, email) "
                            "VALUES ('%s', '%s', 'ADMIN', 'Store Administrator', 'admin@store.local')", u, h) < 0) {
                rc = -1;
            } else {
                LOG_WARN("Created initial admin account '%s' - change its password after first login",
                         g_cfg.admin_user);
            }
            free(u);
            free(h);
        }
    }
    mysql_close(db);
    explicit_bzero(g_cfg.admin_password, sizeof g_cfg.admin_password);
    return rc;
}

static void accept_loop(int lfd)
{
    struct pollfd pfds[2] = {
        { .fd = lfd,        .events = POLLIN },
        { .fd = g_wake[0],  .events = POLLIN },
    };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (atomic_load(&g_running)) {
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR("poll: %s", strerror(errno));
            break;
        }
        if (pfds[1].revents)
            break;                              /* woken by the signal thread */
        if (!(pfds[0].revents & POLLIN))
            continue;

        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;
        int cfd = accept4(lfd, (struct sockaddr *)&addr, &alen, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
                LOG_ERROR("accept: %s", strerror(errno));
            continue;
        }

        /* admission control: take a slot from the counting semaphore */
        if (sem_trywait(&g_slots) != 0) {
            char msg[160];
            int n = snprintf(msg, sizeof msg, "ERR Server busy: %d clients already connected. Try again later.\n.\n",
                             g_cfg.max_clients);
            net_send_all(cfd, msg, (size_t)n);
            close(cfd);
            pthread_mutex_lock(&g_lock);
            g_stats.rejected_busy++;
            pthread_mutex_unlock(&g_lock);
            LOG_WARN("Rejected connection: server full (%d clients)", g_cfg.max_clients);
            continue;
        }

        struct timeval tv = { .tv_sec = g_cfg.idle_timeout, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        client_arg_t *ca = malloc(sizeof *ca);
        if (!ca || registry_add(cfd) < 0) {
            free(ca);
            close(cfd);
            sem_post(&g_slots);
            continue;
        }
        ca->fd   = cfd;
        ca->addr = addr;

        pthread_t tid;
        int err = pthread_create(&tid, &attr, client_thread, ca);
        if (err != 0) {
            LOG_ERROR("pthread_create: %s", strerror(err));
            registry_remove(cfd);
            close(cfd);
            free(ca);
            sem_post(&g_slots);
        }
    }
    pthread_attr_destroy(&attr);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c config_file] [-p port] [-d]\n"
            "  -c FILE  configuration file (default: config/server.conf)\n"
            "  -p PORT  listen port (overrides server_port)\n"
            "  -d       run in the background as a daemon\n"
            "Signals: SIGINT/SIGTERM = graceful stop, SIGHUP = reopen log, SIGUSR1 = log statistics\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *cfg_path = "config/server.conf";
    int port = 0, daemon_mode = 0, opt;

    while ((opt = getopt(argc, argv, "c:p:dh")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'd': daemon_mode = 1; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    config_defaults(&g_cfg);
    if (config_load(cfg_path, &g_cfg) < 0) {
        fprintf(stderr, "Cannot read config file '%s': %s\n"
                        "Copy config/server.conf.example to %s and edit the database password.\n",
                cfg_path, strerror(errno), cfg_path);
        return 1;
    }
    struct stat st;
    if (stat(cfg_path, &st) == 0 && (st.st_mode & S_IROTH))
        fprintf(stderr, "Warning: %s is readable by every user and contains passwords (run: chmod 600 %s)\n",
                cfg_path, cfg_path);
    if (port > 0)
        g_cfg.server_port = port;
    if (g_cfg.server_port <= 0 || g_cfg.server_port > 65535) {
        fprintf(stderr, "Invalid port %d\n", g_cfg.server_port);
        return 1;
    }
    if (g_cfg.max_clients < 1)
        g_cfg.max_clients = 1;
    if (g_cfg.idle_timeout < 10)
        g_cfg.idle_timeout = 10;

    if (daemon_mode && daemonize() < 0) {
        perror("daemonize");
        return 1;
    }

    make_parent_dirs(g_cfg.log_file);
    make_parent_dirs(g_cfg.pid_file);
    if (log_open(g_cfg.log_file, !daemon_mode) < 0) {
        fprintf(stderr, "Cannot open log file %s: %s\n", g_cfg.log_file, strerror(errno));
        return 1;
    }
    if (acquire_pid_file(g_cfg.pid_file) < 0) {
        if (!daemon_mode)
            fprintf(stderr, "Server not started (see message above).\n");
        return 1;
    }

    /* Block these signals in every thread; the signal thread collects them. */
    signal(SIGPIPE, SIG_IGN);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (mysql_library_init(0, NULL, NULL) != 0) {
        LOG_ERROR("mysql_library_init failed");
        return 1;
    }
    if (bootstrap() < 0) {
        mysql_library_end();
        unlink(g_cfg.pid_file);
        return 1;
    }

    g_client_fds = malloc((size_t)g_cfg.max_clients * sizeof *g_client_fds);
    if (!g_client_fds || sem_init(&g_slots, 0, (unsigned)g_cfg.max_clients) < 0 ||
        pipe2(g_wake, O_CLOEXEC) < 0) {
        LOG_ERROR("Startup failed: %s", strerror(errno));
        return 1;
    }
    for (int i = 0; i < g_cfg.max_clients; i++)
        g_client_fds[i] = -1;
    g_stats.max_clients = g_cfg.max_clients;
    g_stats.started     = time(NULL);
    g_stats.pid         = (long)getpid();

    int lfd = create_listener(g_cfg.server_port);
    if (lfd < 0) {
        LOG_ERROR("Cannot listen on port %d: %s", g_cfg.server_port, strerror(errno));
        unlink(g_cfg.pid_file);
        return 1;
    }

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &set) != 0) {
        LOG_ERROR("Cannot start signal thread");
        return 1;
    }

    LOG_INFO("%s v%s listening on port %d (pid %ld, max %d clients, db %s@%s/%s)",
             ORS_APP_NAME, ORS_VERSION, g_cfg.server_port, (long)getpid(), g_cfg.max_clients,
             g_cfg.db_user, g_cfg.db_host, g_cfg.db_name);

    accept_loop(lfd);

    /* ---------------- graceful shutdown ---------------- */
    close(lfd);
    if (atomic_load(&g_running))                 /* loop ended on an error */
        pthread_kill(sig_tid, SIGTERM);
    pthread_join(sig_tid, NULL);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_cfg.max_clients; i++)
        if (g_client_fds[i] != -1)
            shutdown(g_client_fds[i], SHUT_RDWR);   /* wakes threads blocked in read() */

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    while (g_stats.active_clients > 0) {
        if (pthread_cond_timedwait(&g_all_gone, &g_lock, &deadline) == ETIMEDOUT)
            break;
    }
    int remaining = g_stats.active_clients;
    pthread_mutex_unlock(&g_lock);

    log_stats();
    if (remaining > 0) {
        LOG_WARN("%d client thread(s) still busy - exiting anyway", remaining);
    } else {
        mysql_library_end();
        sem_destroy(&g_slots);
    }

    unlink(g_cfg.pid_file);
    close(g_pid_fd);
    LOG_INFO("Server stopped.");
    log_close();
    return 0;
}
