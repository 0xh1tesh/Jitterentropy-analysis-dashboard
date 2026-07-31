#ifndef RNG_WRAPPER_H
#define RNG_WRAPPER_H

#include <stddef.h>

#define RNG_ERR_SELF_TEST_FAILED   (-1001)
#define RNG_ERR_ALLOC_FAILED       (-1002)
#define RNG_ERR_NOT_INITIALIZED    (-1003)
#define RNG_ERR_SHORT_READ         (-1004)
#define RNG_ERR_INVALID_ARGUMENT   (-1005)
#define RNG_ERR_GENERATION_FAILED  (-1006)

int init_rng(void);
int get_random_bytes(unsigned char *buffer, size_t len);
void shutdown_rng(void);

#endif
