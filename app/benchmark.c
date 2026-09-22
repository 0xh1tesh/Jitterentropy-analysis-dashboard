#include "benchmark.h"

#include "rng_wrapper.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

/*
 * Private GUI/benchmark coordination.  This deliberately is not part of
 * benchmark.h: only main_gui.c sets it before and during a GUI benchmark.
 */
volatile LONG benchmark_cancel_requested;

static double qpc_elapsed_seconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	static LARGE_INTEGER frequency;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);

	return (double)(end.QuadPart - start.QuadPart) /
	       (double)frequency.QuadPart;
}

int run_benchmark(size_t bytes_per_call, int iterations,
		  benchmark_result *out)
{
	benchmark_result res = {0};
	unsigned char *buffer;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	double average_latency_seconds;
	double mib_per_second;
	int ret;
	int i;
	bool cancelled = false;

	if (out)
		*out = res;

	if (!bytes_per_call || iterations <= 0)
		return RNG_ERR_INVALID_ARGUMENT;

	if (bytes_per_call > SIZE_MAX / (size_t)iterations)
		return RNG_ERR_INVALID_ARGUMENT;

	buffer = malloc(bytes_per_call);
	if (!buffer)
		return RNG_ERR_ALLOC_FAILED;

	/* One collector for the whole run; init cost is reported separately. */
	shutdown_rng();

	QueryPerformanceCounter(&start);
	ret = init_rng();
	QueryPerformanceCounter(&end);
	res.init_seconds = qpc_elapsed_seconds(start, end);

	if (ret) {
		fprintf(stderr, "benchmark init_rng failed: %d\n", ret);
		goto cleanup;
	}

	QueryPerformanceCounter(&start);
	for (i = 0; i < iterations; i++) {
		if (InterlockedCompareExchange(&benchmark_cancel_requested, 0,
					       0) != 0) {
			cancelled = true;
			break;
		}

		ret = get_random_bytes(buffer, bytes_per_call);
		if (ret) {
			fprintf(stderr, "benchmark generation failed at iteration %d: %d\n",
				i + 1, ret);
			break;
		}
		res.completed_iterations++;
	}
	QueryPerformanceCounter(&end);

	res.generation_seconds = qpc_elapsed_seconds(start, end);
	res.total_bytes = bytes_per_call * (size_t)res.completed_iterations;

	if (ret)
		goto cleanup;

	average_latency_seconds = res.completed_iterations > 0 ?
		res.generation_seconds / (double)res.completed_iterations : 0.0;
	if (res.generation_seconds > 0.0) {
		mib_per_second = ((double)res.total_bytes / (1024.0 * 1024.0)) /
				 res.generation_seconds;
	} else {
		mib_per_second = 0.0;
	}

	printf("\nBenchmark summary\n");
	printf("+----------------------------+----------------------+\n");
	printf("| metric                     | value                |\n");
	printf("+----------------------------+----------------------+\n");
	printf("| init + collector time      | %18.9f s |\n", res.init_seconds);
	printf("| bytes per generation call  | %20zu |\n", bytes_per_call);
	printf("| iterations completed       | %20d |\n", res.completed_iterations);
	printf("| total generated            | %20zu |\n", res.total_bytes);
	printf("| total generation time      | %18.9f s |\n", res.generation_seconds);
	printf("| average latency            | %18.9f s |\n",
	       average_latency_seconds);
	printf("| throughput                 | %16.3f MiB/s |\n", mib_per_second);
	printf("+----------------------------+----------------------+\n");

	if (cancelled) {
		printf("Benchmark cancelled cooperatively.\n");
		ret = BENCHMARK_RESULT_CANCELLED;
	} else {
		ret = 0;
	}

cleanup:
	/*
	 * Every path, including cancellation, releases the collector before the
	 * worker releases the GUI's rng_busy_lock.  Do not use TerminateThread:
	 * it could kill that worker while the lock is held and deadlock the GUI.
	 */
	shutdown_rng();
	rng_secure_zero(buffer, bytes_per_call);
	free(buffer);
	if (out)
		*out = res;
	return ret;
}
