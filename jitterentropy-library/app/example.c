#include "jent_app.h"

#include <stdio.h>
#include <stdint.h>

int main(void)
{
	jent_app_rng rng;
	uint8_t bytes[32];
	int ret;
	size_t i;

	ret = jent_app_rng_open(&rng, 0, 0);
	if (ret) {
		fprintf(stderr, "jent_app_rng_open failed: %d\n", ret);
		return 1;
	}

	ret = (int)jent_app_rng_read(&rng, (char *)bytes, sizeof(bytes));
	if (ret < 0) {
		fprintf(stderr, "jent_app_rng_read failed: %d\n", ret);
		jent_app_rng_close(&rng);
		return 1;
	}

	for (i = 0; i < sizeof(bytes); ++i)
		printf("%02x", (unsigned int)bytes[i]);
	printf("\n");

	jent_app_rng_close(&rng);
	return 0;
}