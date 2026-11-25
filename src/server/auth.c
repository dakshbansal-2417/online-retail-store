/*
 * auth.c - secure password storage.
 *
 * Passwords are never stored in plain text. We use the system crypt library
 * (libxcrypt) with the SHA-512 scheme "$6$" and a 16-character random salt
 * read from /dev/urandom. crypt_r() is used because plain crypt() keeps its
 * result in a static buffer and is therefore not thread-safe.
 */
#include "auth.h"

#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_random(unsigned char *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t got = 0;
    while (got < len) {
        ssize_t k = read(fd, buf + got, len - got);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        got += (size_t)k;
    }
    close(fd);
    return 0;
}

int auth_hash_password(const char *pw, char *out, size_t outsz)
{
    static const char alphabet[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    char setting[32];

    if (read_random(rnd, sizeof rnd) < 0)
        return -1;

    memcpy(setting, "$6$", 3);
    for (size_t i = 0; i < sizeof rnd; i++)
        setting[3 + i] = alphabet[rnd[i] & 63];
    setting[19] = '$';
    setting[20] = '\0';

    struct crypt_data *cd = calloc(1, sizeof *cd);
    if (!cd)
        return -1;

    int rc = -1;
    const char *h = crypt_r(pw, setting, cd);
    if (h && h[0] == '$' && strlen(h) < outsz) {
        strcpy(out, h);
        rc = 0;
    }
    free(cd);
    return rc;
}

int auth_verify_password(const char *pw, const char *stored)
{
    struct crypt_data *cd = calloc(1, sizeof *cd);
    if (!cd)
        return 0;

    int ok = 0;
    const char *h = crypt_r(pw, stored, cd);
    if (h && h[0] == '$') {
        size_t a = strlen(h), b = strlen(stored);
        if (a == b) {
            unsigned char diff = 0;
            for (size_t i = 0; i < a; i++)
                diff |= (unsigned char)(h[i] ^ stored[i]);
            ok = (diff == 0);
        }
    }
    free(cd);
    return ok;
}

int auth_check_policy(const char *pw, char *err, size_t errsz)
{
    size_t n = strlen(pw);
    int alpha = 0, digit = 0;

    if (n < 8 || n > 64) {
        snprintf(err, errsz, "Password must be 8-64 characters long");
        return -1;
    }
    for (const char *p = pw; *p; p++) {
        if (isalpha((unsigned char)*p)) alpha = 1;
        if (isdigit((unsigned char)*p)) digit = 1;
    }
    if (!alpha || !digit) {
        snprintf(err, errsz, "Password must contain at least one letter and one digit");
        return -1;
    }
    return 0;
}

int auth_valid_username(const char *u)
{
    size_t n = strlen(u);
    if (n < 3 || n > 30)
        return 0;
    for (const char *p = u; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.')
            return 0;
    return 1;
}

int auth_valid_email(const char *e)
{
    size_t n = strlen(e);
    const char *at = strchr(e, '@');
    if (n < 5 || n > 100 || !at || at == e || strchr(at + 1, '@'))
        return 0;
    const char *dot = strrchr(at, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0')
        return 0;
    for (const char *p = e; *p; p++)
        if (isspace((unsigned char)*p))
            return 0;
    return 1;
}

int auth_valid_phone(const char *p)
{
    size_t n = strlen(p), digits = 0;
    if (n == 0)
        return 1;               /* phone is optional */
    if (n > 20)
        return 0;
    for (const char *c = p; *c; c++) {
        if (isdigit((unsigned char)*c))
            digits++;
        else if (*c != '+' && *c != '-' && *c != ' ')
            return 0;
    }
    return digits >= 7;
}
