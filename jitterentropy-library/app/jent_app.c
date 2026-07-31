#include "jent_app.h"

#include <string.h>

int jent_app_rng_open(jent_app_rng *rng, unsigned int osr, unsigned int flags)
{
	if (!rng)
		return -1;

	rng->collector = NULL;

	if (jent_entropy_init_ex(osr, flags))
		return -1;

	rng->collector = jent_entropy_collector_alloc(osr, flags);
	if (!rng->collector)
		return EMEM;

	return 0;
}

ssize_t jent_app_rng_read(jent_app_rng *rng, char *data, size_t len)
{
	if (!rng || !rng->collector)
		return -1;

	return jent_read_entropy_safe(&rng->collector, data, len);
}

void jent_app_rng_close(jent_app_rng *rng)
{
	if (!rng)
		return;

	jent_entropy_collector_free(rng->collector);
	rng->collector = NULL;
}