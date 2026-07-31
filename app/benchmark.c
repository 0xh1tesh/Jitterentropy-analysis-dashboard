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

/* A distinct successful-control-flow result: work stopped cooperatively. */
#define BENCHMARK_RESULT_CANCELLED (-2001)

static double qpc_elapsed_seconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	static LARGE_INTEGER frequency;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);

	return (double)(end.QuadPart - start.QuadPart) /
	       (double)frequency.QuadPart;
}

int run_benchmark(size_t bytes_per_call, int iterations)
{
	unsigned char *buffer;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	double init_seconds;
	double generation_seconds;
	double average_latency_seconds;
	double mb_per_second;
	size_t total_bytes;
	int ret;
	int i;
	int completed_iterations = 0;
	bool cancelled = false;

	if (!bytes_per_call || iterations <= 0)
		return RNG_ERR_INVALID_ARGUMENT;

	if (bytes_per_call > SIZE_MAX / (size_t)iterations)
		return RNG_ERR_INVALID_ARGUMENT;

	buffer = malloc(bytes_per_call);
	if (!buffer)
		return RNG_ERR_ALLOC_FAILED;

	shutdown_rng();

	QueryPerformanceCounter(&start);
	ret = init_rng();
	QueryPerformanceCounter(&end);
	init_seconds = qpc_elapsed_seconds(start, end);

	if (ret) {
		fprintf(stderr, "benchmark init_rng failed: %d\n", ret);
		goto cleanup;
	}

	shutdown_rng();

	ret = init_rng();
	if (ret) {
		fprintf(stderr, "benchmark setup init_rng failed: %d\n", ret);
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
			QueryPerformanceCounter(&end);
			fprintf(stderr, "benchmark generation failed at iteration %d: %d\n",
				i + 1, ret);
			goto cleanup;
		}
		completed_iterations++;
	}
	QueryPerformanceCounter(&end);

	generation_seconds = qpc_elapsed_seconds(start, end);
	total_bytes = bytes_per_call * (size_t)completed_iterations;
	average_latency_seconds = completed_iterations > 0 ?
		generation_seconds / (double)completed_iterations : 0.0;
	if (generation_seconds > 0.0) {
		mb_per_second = ((double)total_bytes / (1024.0 * 1024.0)) /
				generation_seconds;
	} else {
		mb_per_second = 0.0;
	}

	printf("\nBenchmark summary\n");
	printf("+----------------------------+----------------------+\n");
	printf("| metric                     | value                |\n");
	printf("+----------------------------+----------------------+\n");
	printf("| init + collector time      | %18.9f s |\n", init_seconds);
	printf("| bytes per generation call  | %20zu |\n", bytes_per_call);
	printf("| iterations completed       | %20d |\n", completed_iterations);
	printf("| total generated            | %20zu |\n", total_bytes);
	printf("| total generation time      | %18.9f s |\n", generation_seconds);
	printf("| average latency            | %18.9f s |\n",
	       average_latency_seconds);
	printf("| throughput                 | %17.3f MB/s |\n", mb_per_second);
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
	free(buffer);
	return ret;
}
