#include "rng_wrapper.h"

#include "health_monitor.h"
#include "jitterentropy.h"

#include <stdbool.h>
#include <windows.h>

static struct rand_data *rng_collector;
static bool entropy_self_test_complete;
static rng_settings current_settings;

/*
 * Guards every read/write of the state above. The GUI's own g_rng_busy_lock
 * (main_gui.cpp) already keeps two worker threads from calling init_rng()/
 * get_random_bytes()/shutdown_rng() concurrently, but BuildTelemetryJson()
 * now reads the collector too, from the UI thread, on every 30ms tick --
 * without this lock that read races jent_read_entropy_safe()'s own internal
 * reassignment of rng_collector on an intermittent health-test recovery.
 * Lazily initialized like health_monitor.c's stats_lock, for the same
 * reason: the CLI path (single-threaded) pays only an uncontended
 * Enter/LeaveCriticalSection, no syscall.
 */
static CRITICAL_SECTION rng_lock;
static INIT_ONCE rng_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_rng_lock_callback(PINIT_ONCE once, PVOID param,
					    PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	InitializeCriticalSection(&rng_lock);
	return TRUE;
}

static void ensure_rng_lock(void)
{
	InitOnceExecuteOnce(&rng_lock_once, init_rng_lock_callback, NULL, NULL);
}

static double qpc_elapsed_seconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	static LARGE_INTEGER frequency;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);

	return (double)(end.QuadPart - start.QuadPart) /
	       (double)frequency.QuadPart;
}

void rng_settings_defaults(rng_settings *out)
{
	if (!out)
		return;
	/* Exactly what jent_entropy_collector_alloc(0, 0) used to hardcode. */
	out->osr = 0;
	out->force_fips = false;
	out->ntg1 = false;
	out->disable_memory_access = false;
}

int rng_configure(const rng_settings *settings)
{
	int ret = 0;

	ensure_rng_lock();
	EnterCriticalSection(&rng_lock);
	if (rng_collector) {
		ret = RNG_ERR_ALREADY_RUNNING;
	} else {
		if (settings)
			current_settings = *settings;
		else
			rng_settings_defaults(&current_settings);
		/*
		 * A changed configuration invalidates the startup self-test
		 * result: FIPS/NTG.1 mode also forces the self-test itself to
		 * run under that mode (see jent_time_entropy_init()), not just
		 * the runtime collector.
		 */
		entropy_self_test_complete = false;
	}
	LeaveCriticalSection(&rng_lock);
	return ret;
}

void rng_get_settings(rng_settings *out)
{
	if (!out)
		return;
	ensure_rng_lock();
	EnterCriticalSection(&rng_lock);
	*out = current_settings;
	LeaveCriticalSection(&rng_lock);
}

static unsigned int settings_to_flags(const rng_settings *s)
{
	unsigned int flags = 0;

	if (s->force_fips)
		flags |= JENT_FORCE_FIPS;
	if (s->ntg1)
		flags |= JENT_NTG1;
	if (s->disable_memory_access)
		flags |= JENT_DISABLE_MEMORY_ACCESS;
	return flags;
}

/*
 * Runs when jent_read_entropy_safe() hits a health failure while the
 * collector is in FIPS/NTG.1 mode (is_fips_enabled) -- see
 * jent_set_fips_failure_callback() in jitterentropy.h. Process-wide, single
 * callback, matching the library's own contract; harmless to (re)register
 * on every init since it only ever fires in FIPS/NTG.1 mode.
 */
static void fips_failure_handler(struct rand_data *ec, unsigned int health_failure)
{
	(void)ec;
	health_monitor_record_fips_failure(health_failure);
}

int init_rng(void)
{
	unsigned int flags;
	int ret = 0;

	ensure_rng_lock();
	EnterCriticalSection(&rng_lock);

	if (rng_collector)
		goto out;

	flags = settings_to_flags(&current_settings);

	if (!entropy_self_test_complete) {
		/*
		 * jent_entropy_init_ex(0, 0) is exactly jent_entropy_init(); one
		 * call covers both the default path and a configured one.
		 */
		ret = jent_entropy_init_ex(current_settings.osr, flags);
		if (ret) {
			health_monitor_set_initialized(false);
			health_monitor_record_failure(RNG_ERR_SELF_TEST_FAILED);
			ret = RNG_ERR_SELF_TEST_FAILED;
			goto out;
		}
		entropy_self_test_complete = true;
	}

	jent_set_fips_failure_callback(fips_failure_handler);

	rng_collector = jent_entropy_collector_alloc(current_settings.osr, flags);
	if (!rng_collector) {
		health_monitor_set_initialized(false);
		health_monitor_record_failure(RNG_ERR_ALLOC_FAILED);
		ret = RNG_ERR_ALLOC_FAILED;
		goto out;
	}

	health_monitor_set_initialized(true);

out:
	LeaveCriticalSection(&rng_lock);
	return ret;
}

unsigned int rng_get_library_version(void)
{
	return jent_version();
}

int rng_get_status_json(char *buf, size_t buflen)
{
	int ret;

	if (!buf || !buflen)
		return -1;

	ensure_rng_lock();
	/*
	 * Never block: this is called from the GUI's 30ms telemetry tick on the
	 * UI thread. If a worker is mid-call (holding the lock for however long
	 * this machine's collector takes), report "no status this tick" rather
	 * than stall the message loop until it finishes.
	 */
	if (!TryEnterCriticalSection(&rng_lock))
		return -1;
	ret = rng_collector ? jent_status(rng_collector, buf, buflen) : -1;
	LeaveCriticalSection(&rng_lock);
	return ret;
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

	ensure_rng_lock();
	EnterCriticalSection(&rng_lock);

	if (!rng_collector) {
		LeaveCriticalSection(&rng_lock);
		health_monitor_record_generation_attempt(false, 0, 0.0,
							  RNG_ERR_NOT_INITIALIZED);
		return RNG_ERR_NOT_INITIALIZED;
	}

	if (!len) {
		LeaveCriticalSection(&rng_lock);
		return 0;
	}

	/*
	 * Resume after a short read instead of discarding the bytes already
	 * produced. A zero or negative return ends the loop.
	 *
	 * Each sub-call's own latency is recorded as one jitter sample: this is
	 * the RNG's real per-call timing, not a signal from an unrelated
	 * background thread. Chunked callers (see GENERATE_CHUNK_BYTES in
	 * main_gui.cpp) get several samples from one logical request.
	 */
	QueryPerformanceCounter(&start);
	while (bytes_generated < len) {
		LARGE_INTEGER call_start, call_end;

		QueryPerformanceCounter(&call_start);
		ret = jent_read_entropy_safe(&rng_collector,
					     (char *)buffer + bytes_generated,
					     len - bytes_generated);
		QueryPerformanceCounter(&call_end);
		health_monitor_record_jitter_sample(
			qpc_elapsed_seconds(call_start, call_end) * 1e6);

		if (ret <= 0)
			break;
		bytes_generated += (size_t)ret;
	}
	QueryPerformanceCounter(&end);

	LeaveCriticalSection(&rng_lock);

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
	ensure_rng_lock();
	EnterCriticalSection(&rng_lock);
	jent_entropy_collector_free(rng_collector);
	rng_collector = NULL;
	LeaveCriticalSection(&rng_lock);
	health_monitor_set_initialized(false);
}

void rng_secure_zero(void *ptr, size_t len)
{
	if (ptr && len)
		SecureZeroMemory(ptr, len);
}
