/*
 * logger.c - log file written with low-level system calls.
 *
 * - open(2) with O_APPEND so every write goes to the end of the file
 * - a pthread mutex serialises writers inside this process
 * - an fcntl(2) write lock (advisory record lock) serialises writers across
 *   processes (e.g. two tools appending to the same log)
 */
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int  log_fd   = -1;
static int  log_echo = 0;
static char log_path[256];
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static int open_log_file(const char *path)
{
    return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
}

int log_open(const char *path, int echo_stderr)
{
    snprintf(log_path, sizeof log_path, "%s", path);
    log_echo = echo_stderr;
    log_fd = open_log_file(path);
    return log_fd < 0 ? -1 : 0;
}

int log_reopen(void)
{
    pthread_mutex_lock(&log_mutex);
    int fd = open_log_file(log_path);
    if (fd >= 0) {
        if (log_fd >= 0)
            close(log_fd);
        log_fd = fd;
    }
    pthread_mutex_unlock(&log_mutex);
    return fd < 0 ? -1 : 0;
}

void log_close(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_fd >= 0)
        close(log_fd);
    log_fd = -1;
    pthread_mutex_unlock(&log_mutex);
}

void log_msg(const char *level, const char *fmt, ...)
{
    char line[2048];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    size_t n = strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S", &tm);
    int k = snprintf(line + n, sizeof line - n, " [%-5s] [tid %ld] ",
                     level, (long)syscall(SYS_gettid));
    if (k > 0)
        n += (size_t)k;

    va_list ap;
    va_start(ap, fmt);
    k = vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    if (k > 0)
        n += (size_t)k;

    if (n > sizeof line - 2)
        n = sizeof line - 2;       /* message was truncated */
    line[n++] = '\n';
    line[n]   = '\0';

    pthread_mutex_lock(&log_mutex);
    if (log_fd >= 0) {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        while (fcntl(log_fd, F_SETLKW, &fl) == -1 && errno == EINTR)
            ;
        ssize_t w = write(log_fd, line, n);
        (void)w;
        fl.l_type = F_UNLCK;
        fcntl(log_fd, F_SETLK, &fl);
    }
    if (log_echo) {
        ssize_t w = write(STDERR_FILENO, line, n);
        (void)w;
    }
    pthread_mutex_unlock(&log_mutex);
}
