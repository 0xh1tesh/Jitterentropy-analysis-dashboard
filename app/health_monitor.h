#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct rng_health_stats {
	bool initialized;
	uint64_t total_bytes_generated;
	uint64_t generation_call_count;
	uint64_t failure_count;
	int last_error_code;
	double cumulative_generation_seconds;
	double average_latency_seconds;
	double throughput_bytes_per_second;
	/*
	 * JENT_RCT_FAILURE / JENT_APT_FAILURE / JENT_LAG_FAILURE / etc. bits
	 * (see jitterentropy.h) from the library's own FIPS failure callback;
	 * 0 if it has never fired. Only fires in FIPS/NTG.1 mode -- see
	 * rng_configure(). health_monitor.c stores this opaquely and does not
	 * interpret the bits, to avoid depending on jitterentropy.h here.
	 */
	unsigned int last_fips_failure_mask;
	uint64_t fips_failure_count;
} rng_health_stats;

void health_monitor_set_initialized(bool initialized);
void health_monitor_record_failure(int error_code);
void health_monitor_record_fips_failure(unsigned int failure_mask);
void health_monitor_record_generation_attempt(bool success,
					      size_t byte_count,
					      double duration_seconds,
					      int error_code);
void health_monitor_snapshot(rng_health_stats *stats);

/*
 * Process-wide QueryPerformanceCounter epoch, latched once. History
 * timestamps and the GUI's timeline both measure from this origin so they can
 * share one axis. Either output pointer may be NULL.
 */
void health_monitor_get_epoch(int64_t *qpc_start, int64_t *qpc_frequency);
void health_monitor_print_report(FILE *stream);
void health_monitor_clear_history(void);

/*
 * Additive snapshot function: Copies up to max_count most-recent samples from the
 * per-call history ring buffer in chronological order (oldest-to-newest).
 * Returns the actual number of samples copied (0 if no samples yet).
 */
size_t health_monitor_recent_history_snapshot(double *out_timestamps,
					       uint64_t *out_bytes,
					       double *out_durations,
					       size_t max_count);

/*
 * Per-call entropy-collection latency, in microseconds. One sample per
 * jent_read_entropy_safe() invocation inside get_random_bytes() -- this is
 * the RNG's own real timing, not a synthetic signal from an unrelated
 * background thread. A chunked Generate call (see GENERATE_CHUNK_BYTES in
 * main_gui.cpp) produces several samples, giving the oscilloscope view a
 * waveform even for one user action.
 */
void health_monitor_record_jitter_sample(double microseconds);

/* Same snapshot contract as health_monitor_recent_history_snapshot(). */
size_t health_monitor_jitter_samples_snapshot(double *out_samples,
					      size_t max_count);

#endif
