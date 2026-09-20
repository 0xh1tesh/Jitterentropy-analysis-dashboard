#include "rng_wrapper.h"

#include "health_monitor.h"
#include "jitterentropy.h"

#include <stdbool.h>
#include <windows.h>

static struct rand_data *rng_collector;
static bool entropy_self_test_complete;

static double qpc_elapsed_seconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	static LARGE_INTEGER frequency;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);

	return (double)(end.QuadPart - start.QuadPart) /
	       (double)frequency.QuadPart;
}

int init_rng(void)
{
	int ret;

	if (rng_collector)
		return 0;

	if (!entropy_self_test_complete) {
		ret = jent_entropy_init();
		if (ret) {
			health_monitor_set_initialized(false);
			health_monitor_record_failure(RNG_ERR_SELF_TEST_FAILED);
			return RNG_ERR_SELF_TEST_FAILED;
		}
		entropy_self_test_complete = true;
	}

	/*
	 * osr=0 and flags=0 intentionally select the library defaults without
	 * FIPS or NTG.1 enforcement. Revisit these values if the application
	 * later needs compliance-mode startup/runtime health-test behavior.
	 */
	rng_collector = jent_entropy_collector_alloc(0, 0);
	if (!rng_collector) {
		health_monitor_set_initialized(false);
		health_monitor_record_failure(RNG_ERR_ALLOC_FAILED);
		return RNG_ERR_ALLOC_FAILED;
	}

	health_monitor_set_initialized(true);
	return 0;
}

int get_random_bytes(unsigned char *buffer, size_t len)
{
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	ssize_t ret = 0;
	size_t bytes_generated = 0;
	double duration_seconds;

	if (!buffer && len) {
		health_monitor_record_generation_attempt(false, 0, 0.0,
							  RNG_ERR_INVALID_ARGUMENT);
		return RNG_ERR_INVALID_ARGUMENT;
	}

	if (!rng_collector) {
		health_monitor_record_generation_attempt(false, 0, 0.0,
							  RNG_ERR_NOT_INITIALIZED);
		return RNG_ERR_NOT_INITIALIZED;
	}

	if (!len)
		return 0;

	/*
	 * Resume after a short read instead of discarding the bytes already
	 * produced. A zero or negative return ends the loop.
	 */
	QueryPerformanceCounter(&start);
	while (bytes_generated < len) {
		ret = jent_read_entropy_safe(&rng_collector,
					     (char *)buffer + bytes_generated,
					     len - bytes_generated);
		if (ret <= 0)
			break;
		bytes_generated += (size_t)ret;
	}
	QueryPerformanceCounter(&end);

	duration_seconds = qpc_elapsed_seconds(start, end);

	if (ret < 0) {
		health_monitor_record_generation_attempt(false, bytes_generated,
							  duration_seconds,
							  RNG_ERR_GENERATION_FAILED);
		return RNG_ERR_GENERATION_FAILED;
	}

	if (bytes_generated != len) {
		health_monitor_record_generation_attempt(false, bytes_generated,
							  duration_seconds,
							  RNG_ERR_SHORT_READ);
		return RNG_ERR_SHORT_READ;
	}

	health_monitor_record_generation_attempt(true, bytes_generated,
						  duration_seconds, 0);
	return 0;
}

void shutdown_rng(void)
{
	jent_entropy_collector_free(rng_collector);
	rng_collector = NULL;
	health_monitor_set_initialized(false);
}

void rng_secure_zero(void *ptr, size_t len)
{
	if (ptr && len)
		SecureZeroMemory(ptr, len);
}
