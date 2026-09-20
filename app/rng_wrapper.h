#ifndef RNG_WRAPPER_H
#define RNG_WRAPPER_H

#include <stddef.h>

#define RNG_ERR_SELF_TEST_FAILED   (-1001)
#define RNG_ERR_ALLOC_FAILED       (-1002)
#define RNG_ERR_NOT_INITIALIZED    (-1003)
#define RNG_ERR_SHORT_READ         (-1004)
#define RNG_ERR_INVALID_ARGUMENT   (-1005)
#define RNG_ERR_GENERATION_FAILED  (-1006)
#define RNG_ERR_BUSY               (-1007)

/* A job stopped cooperatively on request; not a failure of the RNG itself. */
#define RNG_RESULT_CANCELLED       (-2001)

int init_rng(void);
int get_random_bytes(unsigned char *buffer, size_t len);
void shutdown_rng(void);

/* Wipe key material; not optimised away like memset before free(). */
void rng_secure_zero(void *ptr, size_t len);

#endif
