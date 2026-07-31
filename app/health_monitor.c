#include "health_monitor.h"

#include <windows.h>

/*
 * Thread-safety: a process-lifetime CRITICAL_SECTION guards every read/write
 * of current_stats and history_ring. Lazily initialized via InitOnceExecuteOnce
 * so that neither health_monitor.h nor main.c need any architectural changes — the
 * CLI path pays only the cost of an uncontended Enter/LeaveCriticalSection (no
 * syscall). The lock is never destroyed; Windows reclaims it at exit.
 */
static CRITICAL_SECTION stats_lock;
static INIT_ONCE lock_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock_callback(PINIT_ONCE once, PVOID param,
					PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	InitializeCriticalSection(&stats_lock);
	return TRUE;
}

static void ensure_lock(void)
{
	InitOnceExecuteOnce(&lock_init_once, init_lock_callback, NULL, NULL);
}

static rng_health_stats current_stats;

/*
 * HISTORY_RING_SIZE is set to 300 samples. At a typical generation/polling rate of
 * 1-2 calls/sec, 300 entries retains ~2.5 to 5 minutes of continuous call history.
 * During high-frequency benchmark iterations (e.g. 1000 calls), 300 entries captures
 * a rolling window of the most recent 300 iterations without excessive memory footprint
 * (~5.7 KB for the ring array).
 */
#define HISTORY_RING_SIZE 300

typedef struct history_sample {
	double timestamp_seconds;   /* seconds since process start (QPC-derived) */
	uint64_t byte_count;
	double duration_seconds;
} history_sample;

static history_sample history_ring[HISTORY_RING_SIZE];
static size_t history_write_index = 0;
static size_t history_samples_written = 0;

static double get_process_elapsed_seconds(void)
{
	static LARGE_INTEGER frequency;
	static LARGE_INTEGER start_time;

	if (!frequency.QuadPart) {
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start_time);
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - start_time.QuadPart) / (double)frequency.QuadPart;
}

static void health_monitor_update_derived_values(void)
{
	if (current_stats.generation_call_count) {
		current_stats.average_latency_seconds =
			current_stats.cumulative_generation_seconds /
			(double)current_stats.generation_call_count;
	} else {
		current_stats.average_latency_seconds = 0.0;
	}

	if (current_stats.cumulative_generation_seconds > 0.0) {
		current_stats.throughput_bytes_per_second =
			(double)current_stats.total_bytes_generated /
			current_stats.cumulative_generation_seconds;
	} else {
		current_stats.throughput_bytes_per_second = 0.0;
	}
}

void health_monitor_set_initialized(bool initialized)
{
	ensure_lock();
	EnterCriticalSection(&stats_lock);
	current_stats.initialized = initialized;
	LeaveCriticalSection(&stats_lock);
}

void health_monitor_record_failure(int error_code)
{
	ensure_lock();
	EnterCriticalSection(&stats_lock);
	current_stats.failure_count++;
	current_stats.last_error_code = error_code;
	LeaveCriticalSection(&stats_lock);
}

void health_monitor_record_generation_attempt(bool success,
					      size_t byte_count,
					      double duration_seconds,
					      int error_code)
{
	ensure_lock();
	EnterCriticalSection(&stats_lock);

	current_stats.generation_call_count++;
	current_stats.cumulative_generation_seconds += duration_seconds;

	if (success) {
		current_stats.total_bytes_generated += (uint64_t)byte_count;
		current_stats.last_error_code = 0;
	} else {
		/*
		 * Inline the failure recording here instead of calling
		 * health_monitor_record_failure() to avoid recursive locking
		 * (CRITICAL_SECTION is reentrant on Windows, but avoiding
		 * the redundant ensure_lock + Enter/Leave is cleaner).
		 */
		current_stats.failure_count++;
		current_stats.last_error_code = error_code;
	}

	/*
	 * Record sample into per-call history ring buffer under stats_lock.
	 * Failed attempts are recorded with byte_count = 0 so the Performance
	 * view accurately reflects generation drops/errors as zero-throughput.
	 */
	history_sample sample;
	sample.timestamp_seconds = get_process_elapsed_seconds();
	sample.byte_count = success ? (uint64_t)byte_count : 0;
	sample.duration_seconds = duration_seconds;

	history_ring[history_write_index] = sample;
	history_write_index = (history_write_index + 1) % HISTORY_RING_SIZE;
	if (history_samples_written < HISTORY_RING_SIZE)
		history_samples_written++;

	health_monitor_update_derived_values();
	LeaveCriticalSection(&stats_lock);
}

void health_monitor_snapshot(rng_health_stats *stats)
{
	if (!stats)
		return;

	ensure_lock();
	EnterCriticalSection(&stats_lock);
	health_monitor_update_derived_values();
	*stats = current_stats;
	LeaveCriticalSection(&stats_lock);
}

size_t health_monitor_recent_history_snapshot(double *out_timestamps,
					       uint64_t *out_bytes,
					       double *out_durations,
					       size_t max_count)
{
	size_t available;
	size_t count_to_copy;
	size_t start_idx;
	size_t i;

	if (!out_timestamps || !out_bytes || !out_durations || !max_count)
		return 0;

	ensure_lock();
	EnterCriticalSection(&stats_lock);

	available = history_samples_written;
	count_to_copy = available < max_count ? available : max_count;

	if (count_to_copy == 0) {
		LeaveCriticalSection(&stats_lock);
		return 0;
	}

	/*
	 * Determine starting index in the ring buffer for oldest-to-newest
	 * chronological order.
	 */
	if (history_samples_written < HISTORY_RING_SIZE) {
		start_idx = history_write_index - count_to_copy;
	} else {
		start_idx = (history_write_index + HISTORY_RING_SIZE - count_to_copy) % HISTORY_RING_SIZE;
	}

	for (i = 0; i < count_to_copy; i++) {
		size_t idx = (start_idx + i) % HISTORY_RING_SIZE;
		out_timestamps[i] = history_ring[idx].timestamp_seconds;
		out_bytes[i] = history_ring[idx].byte_count;
		out_durations[i] = history_ring[idx].duration_seconds;
	}

	LeaveCriticalSection(&stats_lock);
	return count_to_copy;
}

void health_monitor_print_report(FILE *stream)
{
	rng_health_stats stats;

	if (!stream)
		stream = stdout;

	health_monitor_snapshot(&stats);

	fprintf(stream, "RNG health status\n");
	fprintf(stream, "  initialized: %s\n", stats.initialized ? "yes" : "no");
	fprintf(stream, "  total bytes generated: %llu\n",
		(unsigned long long)stats.total_bytes_generated);
	fprintf(stream, "  generation calls: %llu\n",
		(unsigned long long)stats.generation_call_count);
	fprintf(stream, "  failures: %llu\n",
		(unsigned long long)stats.failure_count);
	fprintf(stream, "  last error code: %d\n", stats.last_error_code);
	fprintf(stream, "  cumulative generation time: %.9f s\n",
		stats.cumulative_generation_seconds);
	fprintf(stream, "  average latency: %.9f s/call\n",
		stats.average_latency_seconds);
	fprintf(stream, "  throughput: %.2f bytes/s\n",
		stats.throughput_bytes_per_second);
}
