/*
 * logger.h - thread-safe, process-safe append-only log file.
 */
#ifndef ORS_LOGGER_H
#define ORS_LOGGER_H

int  log_open(const char *path, int echo_stderr);
int  log_reopen(void);          /* used on SIGHUP for log rotation */
void log_close(void);
void log_msg(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_INFO(...)  log_msg("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR", __VA_ARGS__)

#endif /* ORS_LOGGER_H */
