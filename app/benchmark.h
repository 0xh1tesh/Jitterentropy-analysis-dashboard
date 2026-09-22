#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>

#include "rng_wrapper.h"

/* Same value the GUI receives as an action status; see rng_wrapper.h. */
#define BENCHMARK_RESULT_CANCELLED RNG_RESULT_CANCELLED

/*
 * What actually ran, including for a cancelled or failed run. Callers must
 * report these rather than the arguments they passed in.
 */
typedef struct benchmark_result {
	int completed_iterations;
	size_t total_bytes;
	double init_seconds;
	double generation_seconds;
} benchmark_result;

/* out may be NULL. Returns 0, BENCHMARK_RESULT_CANCELLED, or an RNG_ERR_*. */
int run_benchmark(size_t bytes_per_call, int iterations,
		  benchmark_result *out);

#endif
