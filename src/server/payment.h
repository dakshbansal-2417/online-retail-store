/*
 * payment.h - payment validation and the simulated payment gateway.
 */
#ifndef ORS_PAYMENT_H
#define ORS_PAYMENT_H

#include <stddef.h>

typedef enum { PAY_CARD, PAY_UPI, PAY_COD } pay_method_t;

typedef struct {
    pay_method_t method;
    char masked[80];        /* safe to store, e.g. "Card **** 1111"   */
    char card[20];          /* digits only - never stored             */
    char expiry[6];         /* MM/YY                                  */
    char cvv[5];            /* never stored                           */
    char vpa[64];           /* UPI id                                 */
} payment_request_t;

typedef struct {
    int  approved;          /* 1 if the order may be placed           */
    int  pending;           /* 1 for cash on delivery                 */
    char reference[40];     /* gateway transaction reference          */
    char message[128];
} payment_result_t;

const char *payment_method_name(pay_method_t m);

/* argv[0] = CARD|UPI|COD followed by method-specific fields. */
int payment_parse(int argc, char **argv, payment_request_t *req, char *err, size_t errsz);

/*
 * Charge amount_paise. Card/UPI payments are processed by a separate child
 * process (fork) that plays the bank gateway and answers through a pipe.
 * Returns 0 when the gateway answered (check res->approved), -1 on failure.
 */
int payment_process(const payment_request_t *req, long long amount_paise, payment_result_t *res);

/* Overwrite sensitive card data in memory. */
void payment_wipe(payment_request_t *req);

#endif /* ORS_PAYMENT_H */
