/*
 * auth.h - password hashing (SHA-512 crypt) and input validation.
 */
#ifndef ORS_AUTH_H
#define ORS_AUTH_H

#include <stddef.h>

/* Hash pw with a random salt ("$6$salt$hash"). 0 ok, -1 error. */
int auth_hash_password(const char *pw, char *out, size_t outsz);

/* 1 if pw matches the stored hash, 0 otherwise (constant-time compare). */
int auth_verify_password(const char *pw, const char *stored_hash);

/* 0 if pw satisfies the password policy, else -1 with a reason in err. */
int auth_check_policy(const char *pw, char *err, size_t errsz);

int auth_valid_username(const char *u);
int auth_valid_email(const char *e);
int auth_valid_phone(const char *p);

#endif /* ORS_AUTH_H */
