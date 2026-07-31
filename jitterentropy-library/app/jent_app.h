#ifndef APP_JENT_APP_H
#define APP_JENT_APP_H

#include "jitterentropy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jent_app_rng {
	struct rand_data *collector;
} jent_app_rng;

int jent_app_rng_open(jent_app_rng *rng, unsigned int osr,
		       unsigned int flags);
ssize_t jent_app_rng_read(jent_app_rng *rng, char *data, size_t len);
void jent_app_rng_close(jent_app_rng *rng);

#ifdef __cplusplus
}
#endif

#endif