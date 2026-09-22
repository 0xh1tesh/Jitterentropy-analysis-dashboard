#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <string>
#include <sstream>
#include <iomanip>

extern "C" {
#include "rng_wrapper.h"
#include "health_monitor.h"
#include "benchmark.h"
#include "entropy_stats.h"

extern volatile LONG benchmark_cancel_requested;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#include "WebView2.h"
#pragma GCC diagnostic pop

#include "bridge_json.h"

#define IDT_BRIDGE_TIMER 1001

/* Server-side limits. The page advertises the same numbers but is not trusted. */
#define GUI_MAX_GENERATE_BYTES     65536
#define GUI_MAX_BENCH_BYTES        65536
#define GUI_MAX_BENCH_ITERATIONS   100000
/* Generate polls for Cancel between chunks of this size. */
#define GENERATE_CHUNK_BYTES       4096
#define WORKER_SHUTDOWN_TIMEOUT_MS 10000

/*
 * Live SP800-90B-style diagnostic on the jitter-sample ring (see
 * entropy_stats.h): a smaller-than-standard APT window because the ring
 * only ever holds JITTER_RING_SIZE (256, health_monitor.c) of the most
 * recent samples, and alpha/H chosen for a continuous dashboard reading,
 * not a certified test. See rng_get_status_json() for the library's own,
 * authoritative health-test state.
 */
#define JITTER_HEALTH_ALPHA      (1.0 / 1048576.0) /* 2^-20 */
#define JITTER_HEALTH_H_BITS     1.0
#define JITTER_HEALTH_APT_WINDOW 128

static void WipeString(std::string &str)
{
	if (!str.empty())
		rng_secure_zero(&str[0], str.size());
	str.clear();
}

static ICoreWebView2Controller *g_controller = NULL;
static ICoreWebView2 *g_webview = NULL;
static EventRegistrationToken g_message_token = {};

/*
 * High-Precision Process-Relative Time Base
 */
static LARGE_INTEGER g_qpc_frequency = {};
static LARGE_INTEGER g_qpc_process_start = {};

static void EnsureQpcTimerInitialized(void)
{
	if (!g_qpc_frequency.QuadPart) {
		/* Same origin as the health monitor's history timestamps. */
		int64_t start = 0, frequency = 0;
		health_monitor_get_epoch(&start, &frequency);
		g_qpc_frequency.QuadPart = frequency;
		g_qpc_process_start.QuadPart = start;
	}
}

static double QpcToProcessSeconds(LARGE_INTEGER qpc)
{
	EnsureQpcTimerInitialized();
	return (double)(qpc.QuadPart - g_qpc_process_start.QuadPart) / (double)g_qpc_frequency.QuadPart;
}

static double QpcElapsedSeconds(LARGE_INTEGER start, LARGE_INTEGER end)
{
	EnsureQpcTimerInitialized();
	return (double)(end.QuadPart - start.QuadPart) / (double)g_qpc_frequency.QuadPart;
}

/*
 * Shared RNG Worker Concurrency Control & State
 */
static CRITICAL_SECTION g_rng_busy_lock;
static volatile LONG g_rng_busy = 0;
static CRITICAL_SECTION g_worker_thread_lock;
static HANDLE g_worker_thread = NULL;

struct WorkerResultPayload {
	bool valid;
	std::string action;
	int status;
	size_t bytes;
	int iterations;
	double duration;
	std::string hex_data;
	/* Only set by GenerateWorkerThread, on whatever bytes were produced
	 * (including a partial buffer from a cancelled or failed run). */
	bool has_entropy_stats;
	entropy_report stats;
};

static WorkerResultPayload g_pending_worker_result = {};
static CRITICAL_SECTION g_result_lock;

/*
 * Timeline View Pipeline Stage Instrumentation
 */
struct StageTiming {
	double start_seconds;
	double duration_seconds;
};

struct TimelineMetrics {
	bool valid;
	StageTiming request;
	StageTiming entropy;
	StageTiming processing;
	StageTiming serialization;
};

static TimelineMetrics g_last_timeline = {};
static CRITICAL_SECTION g_timeline_lock;

/*
 * Wait for the previous worker to finish exiting, then close its handle. A
 * worker clears g_rng_busy just before it returns, so it can still be alive
 * when the next job is admitted; a zero-timeout poll then misses it and the
 * handle is overwritten (leaked) when the slot is reused.
 */
static bool ReapWorkerHandle(void)
{
	bool ok = true;

	EnterCriticalSection(&g_worker_thread_lock);
	if (g_worker_thread) {
		if (WaitForSingleObject(g_worker_thread, 2000) == WAIT_OBJECT_0) {
			CloseHandle(g_worker_thread);
			g_worker_thread = NULL;
		} else {
			ok = false;
		}
	}
	LeaveCriticalSection(&g_worker_thread_lock);
	return ok;
}

/*
 * Tell the page a request was refused. An unconsumed real result is never
 * overwritten: losing a generated sample is worse than dropping a rejection.
 */
static void PublishRejection(const char *action, int status)
{
	EnterCriticalSection(&g_result_lock);
	if (!g_pending_worker_result.valid) {
		g_pending_worker_result.valid = true;
		g_pending_worker_result.action = action;
		g_pending_worker_result.status = status;
		g_pending_worker_result.bytes = 0;
		g_pending_worker_result.iterations = 0;
		g_pending_worker_result.duration = 0.0;
		g_pending_worker_result.hex_data.clear();
		g_pending_worker_result.has_entropy_stats = false;
	}
	LeaveCriticalSection(&g_result_lock);
}

/* Claims the single RNG worker slot, or reports RNG_ERR_BUSY. */
static bool TryAdmitJob(const char *action)
{
	if (InterlockedCompareExchange(&g_rng_busy, 1, 0) != 0) {
		PublishRejection(action, RNG_ERR_BUSY);
		return false;
	}
	if (!ReapWorkerHandle()) {
		InterlockedExchange(&g_rng_busy, 0);
		PublishRejection(action, RNG_ERR_BUSY);
		return false;
	}
	/*
	 * Reset the cancel flag here, before the worker exists, and never inside
	 * the worker: a Cancel (or window close) arriving after admission must not
	 * be erased by the job it was meant to stop.
	 */
	InterlockedExchange(&benchmark_cancel_requested, 0);
	return true;
}

static void PublishWorkerStartFailure(const char *action, size_t bytes,
				      int iterations)
{
	EnterCriticalSection(&g_result_lock);
	g_pending_worker_result.valid = true;
	g_pending_worker_result.action = action;
	g_pending_worker_result.status = RNG_ERR_ALLOC_FAILED;
	g_pending_worker_result.bytes = bytes;
	g_pending_worker_result.iterations = iterations;
	g_pending_worker_result.duration = 0.0;
	g_pending_worker_result.hex_data.clear();
	g_pending_worker_result.has_entropy_stats = false;
	LeaveCriticalSection(&g_result_lock);
	InterlockedExchange(&g_rng_busy, 0);
}

struct GenerateWorkerArgs {
	size_t byte_count;
};

static DWORD WINAPI GenerateWorkerThread(LPVOID lpParam)
{
	GenerateWorkerArgs *args = (GenerateWorkerArgs *)lpParam;
	size_t byte_count = args->byte_count;
	delete args;

	LARGE_INTEGER req_start, req_end, ent_start, ent_end, proc_start, proc_end;
	QueryPerformanceCounter(&req_start);

	EnterCriticalSection(&g_rng_busy_lock);

	int init_ret = init_rng();
	int gen_ret = 0;
	std::string hex_str = "";
	double duration = 0.0;
	size_t produced = 0;
	entropy_report stats;
	bool has_stats = false;

	QueryPerformanceCounter(&req_end);

	if (init_ret == 0 && byte_count > 0) {
		unsigned char *buf = (unsigned char *)malloc(byte_count);
		if (buf) {
			size_t done = 0;

			QueryPerformanceCounter(&ent_start);
			while (done < byte_count) {
				if (InterlockedCompareExchange(&benchmark_cancel_requested, 0, 0) != 0) {
					gen_ret = RNG_RESULT_CANCELLED;
					break;
				}
				size_t n = byte_count - done;
				if (n > GENERATE_CHUNK_BYTES)
					n = GENERATE_CHUNK_BYTES;
				gen_ret = get_random_bytes(buf + done, n);
				if (gen_ret)
					break;
				done += n;
			}
			QueryPerformanceCounter(&ent_end);
			produced = done;

			duration = QpcElapsedSeconds(ent_start, ent_end);

			QueryPerformanceCounter(&proc_start);
			if (gen_ret == 0) {
				static const char kHex[] = "0123456789abcdef";
				hex_str.resize(byte_count * 2);
				for (size_t i = 0; i < byte_count; i++) {
					hex_str[2 * i] = kHex[buf[i] >> 4];
					hex_str[2 * i + 1] = kHex[buf[i] & 0x0f];
				}
			}
			/* Whatever was actually produced, including a partial buffer
			 * from a cancelled or failed run -- before it is wiped below. */
			if (produced > 0) {
				entropy_stats_report(buf, produced, &stats);
				has_stats = true;
			}
			rng_secure_zero(buf, byte_count);
			free(buf);
			QueryPerformanceCounter(&proc_end);
		} else {
			gen_ret = RNG_ERR_ALLOC_FAILED;
			ent_start = ent_end = proc_start = proc_end = req_end;
		}
	} else {
		gen_ret = init_ret;
		ent_start = ent_end = proc_start = proc_end = req_end;
	}

	EnterCriticalSection(&g_result_lock);
	g_pending_worker_result.valid = true;
	g_pending_worker_result.action = "generate";
	g_pending_worker_result.status = gen_ret;
	g_pending_worker_result.bytes = produced;
	g_pending_worker_result.iterations = 1;
	g_pending_worker_result.duration = duration;
	g_pending_worker_result.hex_data = hex_str;
	g_pending_worker_result.has_entropy_stats = has_stats;
	if (has_stats)
		g_pending_worker_result.stats = stats;
	LeaveCriticalSection(&g_result_lock);
	WipeString(hex_str);

	/* Safely record timeline stage timings under lock */
	EnterCriticalSection(&g_timeline_lock);
	g_last_timeline.valid = true;
	g_last_timeline.request.start_seconds = QpcToProcessSeconds(req_start);
	g_last_timeline.request.duration_seconds = QpcElapsedSeconds(req_start, req_end);
	g_last_timeline.entropy.start_seconds = QpcToProcessSeconds(ent_start);
	g_last_timeline.entropy.duration_seconds = QpcElapsedSeconds(ent_start, ent_end);
	g_last_timeline.processing.start_seconds = QpcToProcessSeconds(proc_start);
	g_last_timeline.processing.duration_seconds = QpcElapsedSeconds(proc_start, proc_end);
	LeaveCriticalSection(&g_timeline_lock);

	LeaveCriticalSection(&g_rng_busy_lock);
	InterlockedExchange(&g_rng_busy, 0);
	return 0;
}

struct BenchmarkWorkerArgs {
	size_t bytes_per_call;
	int iterations;
};

static DWORD WINAPI BenchmarkWorkerThread(LPVOID lpParam)
{
	BenchmarkWorkerArgs *args = (BenchmarkWorkerArgs *)lpParam;
	size_t bytes_per_call = args->bytes_per_call;
	int iterations = args->iterations;
	delete args;

	LARGE_INTEGER req_start, req_end, ent_start, ent_end, proc_start, proc_end;
	QueryPerformanceCounter(&req_start);

	EnterCriticalSection(&g_rng_busy_lock);

	QueryPerformanceCounter(&req_end);
	QueryPerformanceCounter(&ent_start);

	benchmark_result bres;
	int res = run_benchmark(bytes_per_call, iterations, &bres);

	QueryPerformanceCounter(&ent_end);
	/* What the run measured itself, not wall time around its setup. */
	double duration = bres.generation_seconds;

	QueryPerformanceCounter(&proc_start);
	EnterCriticalSection(&g_result_lock);
	g_pending_worker_result.valid = true;
	g_pending_worker_result.action = "benchmark";
	g_pending_worker_result.status = res;
	g_pending_worker_result.bytes = bytes_per_call;
	g_pending_worker_result.iterations = bres.completed_iterations;
	g_pending_worker_result.duration = duration;
	g_pending_worker_result.hex_data = "";
	g_pending_worker_result.has_entropy_stats = false;
	LeaveCriticalSection(&g_result_lock);
	QueryPerformanceCounter(&proc_end);

	EnterCriticalSection(&g_timeline_lock);
	g_last_timeline.valid = true;
	g_last_timeline.request.start_seconds = QpcToProcessSeconds(req_start);
	g_last_timeline.request.duration_seconds = QpcElapsedSeconds(req_start, req_end);
	g_last_timeline.entropy.start_seconds = QpcToProcessSeconds(ent_start);
	g_last_timeline.entropy.duration_seconds = QpcElapsedSeconds(ent_start, ent_end);
	g_last_timeline.processing.start_seconds = QpcToProcessSeconds(proc_start);
	g_last_timeline.processing.duration_seconds = QpcElapsedSeconds(proc_start, proc_end);
	LeaveCriticalSection(&g_timeline_lock);

	LeaveCriticalSection(&g_rng_busy_lock);
	InterlockedExchange(&g_rng_busy, 0);
	return 0;
}

static std::string BuildTelemetryJson()
{
	LARGE_INTEGER ser_start, ser_end;
	QueryPerformanceCounter(&ser_start);

	double jitter_buf[256];
	size_t jitter_count = health_monitor_jitter_samples_snapshot(jitter_buf, 256);
	entropy_rct_result jitter_rct = entropy_stats_rct(jitter_buf, jitter_count,
							  JITTER_HEALTH_ALPHA, JITTER_HEALTH_H_BITS);
	entropy_apt_result jitter_apt = entropy_stats_apt(jitter_buf, jitter_count,
							  JITTER_HEALTH_APT_WINDOW,
							  JITTER_HEALTH_ALPHA, JITTER_HEALTH_H_BITS);

	rng_health_stats health;
	health_monitor_snapshot(&health);

	unsigned int library_version = rng_get_library_version();
	char status_json[4096];
	bool have_status = rng_get_status_json(status_json, sizeof(status_json)) == 0;

	double hist_ts[300];
	uint64_t hist_bytes[300];
	double hist_dur[300];
	size_t hist_count = health_monitor_recent_history_snapshot(hist_ts, hist_bytes, hist_dur, 300);

	WorkerResultPayload worker_res = {};
	EnterCriticalSection(&g_result_lock);
	if (g_pending_worker_result.valid) {
		worker_res = g_pending_worker_result;
		g_pending_worker_result.valid = false;
		WipeString(g_pending_worker_result.hex_data);
	}
	LeaveCriticalSection(&g_result_lock);

	std::stringstream ss;
	ss << std::fixed << std::setprecision(6);

	ss << "{\n";
	ss << "  \"type\": \"telemetry\",\n";
	ss << "  \"busy\": " << (g_rng_busy ? "true" : "false") << ",\n";

	ss << "  \"jitter\": [";
	for (size_t i = 0; i < jitter_count; i++) {
		ss << bridge::Finite(jitter_buf[i]) << (i + 1 == jitter_count ? "" : ",");
	}
	ss << "],\n";

	/*
	 * Diagnostic overlay on the jitter samples above (real per-call RNG
	 * latency, not a synthetic signal -- see health_monitor_record_jitter_sample()
	 * in rng_wrapper.c). Not the library's own internal health-test state;
	 * that is "library.status.healthFailure" below, straight from jent_status().
	 */
	ss << "  \"jitterHealth\": {\n";
	ss << "    \"sampleCount\": " << jitter_count << ",\n";
	ss << "    \"rct\": {\"cutoff\": " << jitter_rct.cutoff
	   << ", \"maxRun\": " << jitter_rct.max_run
	   << ", \"passed\": " << (jitter_rct.passed ? "true" : "false") << "},\n";
	ss << "    \"apt\": {\"windowSize\": " << jitter_apt.window_size
	   << ", \"cutoff\": " << jitter_apt.cutoff
	   << ", \"windowsTested\": " << jitter_apt.windows_tested
	   << ", \"windowsFailed\": " << jitter_apt.windows_failed
	   << ", \"passed\": " << (jitter_apt.passed ? "true" : "false") << "}\n";
	ss << "  },\n";

	/*
	 * The library's own authoritative state: jent_version() and the raw
	 * jent_status() JSON object, spliced in verbatim (it is already valid,
	 * self-contained JSON -- see rng_get_status_json()).
	 */
	ss << "  \"library\": {\n";
	ss << "    \"version\": \"" << (library_version / 1000000) << "."
	   << ((library_version / 10000) % 100) << "." << ((library_version / 100) % 100) << "\",\n";
	ss << "    \"status\": " << (have_status ? status_json : "null") << "\n";
	ss << "  },\n";

	ss << "  \"health\": {\n";
	ss << "    \"initialized\": " << (health.initialized ? "true" : "false") << ",\n";
	ss << "    \"total_bytes_generated\": " << health.total_bytes_generated << ",\n";
	ss << "    \"generation_call_count\": " << health.generation_call_count << ",\n";
	ss << "    \"failure_count\": " << health.failure_count << ",\n";
	ss << "    \"last_error_code\": " << health.last_error_code << ",\n";
	ss << "    \"cumulative_generation_seconds\": " << bridge::Finite(health.cumulative_generation_seconds) << ",\n";
	ss << "    \"average_latency_seconds\": " << bridge::Finite(health.average_latency_seconds) << ",\n";
	ss << "    \"throughput_bytes_per_second\": " << bridge::Finite(health.throughput_bytes_per_second) << "\n";
	ss << "  },\n";

	ss << "  \"history\": {\n";
	ss << "    \"count\": " << hist_count << ",\n";
	ss << "    \"timestamps\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << bridge::Finite(hist_ts[i]) << (i + 1 == hist_count ? "" : ",");
	}
	ss << "],\n";
	ss << "    \"bytes\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << hist_bytes[i] << (i + 1 == hist_count ? "" : ",");
	}
	ss << "],\n";
	ss << "    \"durations\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << bridge::Finite(hist_dur[i]) << (i + 1 == hist_count ? "" : ",");
	}
	ss << "]\n";
	ss << "  },\n";

	if (worker_res.action.length() > 0) {
		ss << "  \"actionResult\": {\n";
		ss << "    \"action\": \"" << bridge::JsonEscape(worker_res.action) << "\",\n";
		ss << "    \"status\": " << worker_res.status << ",\n";
		ss << "    \"bytes\": " << worker_res.bytes << ",\n";
		ss << "    \"iterations\": " << worker_res.iterations << ",\n";
		ss << "    \"duration\": " << bridge::Finite(worker_res.duration) << ",\n";
		ss << "    \"hex\": \"" << bridge::JsonEscape(worker_res.hex_data) << "\",\n";
		if (worker_res.has_entropy_stats) {
			const entropy_report &r = worker_res.stats;

			ss << "    \"entropyStats\": {\n";
			ss << "      \"byteCount\": " << r.byte_count << ",\n";
			ss << "      \"shannonBitsPerByte\": " << bridge::Finite(r.shannon_bits_per_byte) << ",\n";
			ss << "      \"minEntropyBitsPerByte\": " << bridge::Finite(r.min_entropy_bits_per_byte) << ",\n";
			ss << "      \"chiSquare\": {\"statistic\": " << bridge::Finite(r.chi_square.statistic)
			   << ", \"pValue\": " << bridge::Finite(r.chi_square.p_value) << "},\n";
			ss << "      \"monobit\": {\"pValue\": " << bridge::Finite(r.monobit.p_value)
			   << ", \"ones\": " << r.monobit.ones << ", \"bitTotal\": " << r.monobit.bit_total << "},\n";
			ss << "      \"serialCorrelation\": " << bridge::Finite(r.serial_correlation) << ",\n";
			ss << "      \"runs\": {\"applicable\": " << (r.runs.applicable ? "true" : "false")
			   << ", \"pValue\": " << bridge::Finite(r.runs.p_value)
			   << ", \"observedRuns\": " << r.runs.observed_runs << "},\n";
			ss << "      \"histogram\": [";
			for (int i = 0; i < ENTROPY_STATS_BINS; i++) {
				ss << r.histogram[i] << (i + 1 == ENTROPY_STATS_BINS ? "" : ",");
			}
			ss << "]\n";
			ss << "    }\n";
		} else {
			ss << "    \"entropyStats\": null\n";
		}
		ss << "  },\n";
	} else {
		ss << "  \"actionResult\": null,\n";
	}

	QueryPerformanceCounter(&ser_end);

	EnterCriticalSection(&g_timeline_lock);
	if (g_last_timeline.valid) {
		g_last_timeline.serialization.start_seconds = QpcToProcessSeconds(ser_start);
		g_last_timeline.serialization.duration_seconds = QpcElapsedSeconds(ser_start, ser_end);
	}
	TimelineMetrics current_timeline = g_last_timeline;
	LeaveCriticalSection(&g_timeline_lock);

	if (current_timeline.valid) {
		ss << "  \"timeline\": {\n";
		ss << "    \"request\": {\"start\": " << bridge::Finite(current_timeline.request.start_seconds) << ", \"duration\": " << bridge::Finite(current_timeline.request.duration_seconds) << "},\n";
		ss << "    \"entropy\": {\"start\": " << bridge::Finite(current_timeline.entropy.start_seconds) << ", \"duration\": " << bridge::Finite(current_timeline.entropy.duration_seconds) << "},\n";
		ss << "    \"processing\": {\"start\": " << bridge::Finite(current_timeline.processing.start_seconds) << ", \"duration\": " << bridge::Finite(current_timeline.processing.duration_seconds) << "},\n";
		ss << "    \"serialization\": {\"start\": " << bridge::Finite(current_timeline.serialization.start_seconds) << ", \"duration\": " << bridge::Finite(current_timeline.serialization.duration_seconds) << "}\n";
		ss << "  }\n";
	} else {
		ss << "  \"timeline\": null\n";
	}

	ss << "}";
	std::string out = ss.str();
	WipeString(worker_res.hex_data);
	return out;
}

class WebMessageReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler {
private:
	LONG m_refCount;

public:
	WebMessageReceivedHandler() : m_refCount(1) {}
	virtual ~WebMessageReceivedHandler() {}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return InterlockedIncrement(&m_refCount);
	}

	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&m_refCount);
		if (count == 0) delete this;
		return count;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (!ppvObject) return E_POINTER;
		if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2WebMessageReceivedEventHandler)) {
			*ppvObject = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) override {
		(void)sender;
		LPWSTR json_wide = NULL;
		if (FAILED(args->get_WebMessageAsJson(&json_wide)) || !json_wide) {
			return S_OK;
		}

		int len = WideCharToMultiByte(CP_UTF8, 0, json_wide, -1, NULL, 0, NULL, NULL);
		std::string json_str;
		if (len > 1) {
			json_str.resize(len);
			WideCharToMultiByte(CP_UTF8, 0, json_wide, -1, &json_str[0], len, NULL, NULL);
			json_str.resize(len - 1); /* drop the terminating NUL */
		}
		CoTaskMemFree(json_wide);

		bridge::Object msg;
		std::string action;
		if (!bridge::ParseFlatObject(json_str, msg) ||
		    bridge::GetString(msg, "action", action) != bridge::kOk) {
			printf("Ignoring malformed bridge message\n");
			return S_OK;
		}

		if (action == "generate") {
			long long bytes = 32;
			if (bridge::GetInt(msg, "bytes", 1, GUI_MAX_GENERATE_BYTES, bytes) == bridge::kInvalid) {
				PublishRejection("generate", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (!TryAdmitJob("generate"))
				return S_OK;

			GenerateWorkerArgs *wargs = new GenerateWorkerArgs();
			wargs->byte_count = (size_t)bytes;
			HANDLE hThread = CreateThread(NULL, 0, GenerateWorkerThread, wargs, 0, NULL);
			if (hThread) {
				EnterCriticalSection(&g_worker_thread_lock);
				g_worker_thread = hThread;
				LeaveCriticalSection(&g_worker_thread_lock);
			} else {
				delete wargs;
				PublishWorkerStartFailure("generate", (size_t)bytes, 1);
			}
		}
		else if (action == "benchmark") {
			long long bytes = 1024;
			long long iter = 100;
			if (bridge::GetInt(msg, "bytesPerCall", 1, GUI_MAX_BENCH_BYTES, bytes) == bridge::kInvalid ||
			    bridge::GetInt(msg, "iterations", 1, GUI_MAX_BENCH_ITERATIONS, iter) == bridge::kInvalid) {
				PublishRejection("benchmark", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (!TryAdmitJob("benchmark"))
				return S_OK;

			BenchmarkWorkerArgs *wargs = new BenchmarkWorkerArgs();
			wargs->bytes_per_call = (size_t)bytes;
			wargs->iterations = (int)iter;
			HANDLE hThread = CreateThread(NULL, 0, BenchmarkWorkerThread, wargs, 0, NULL);
			if (hThread) {
				EnterCriticalSection(&g_worker_thread_lock);
				g_worker_thread = hThread;
				LeaveCriticalSection(&g_worker_thread_lock);
			} else {
				delete wargs;
				PublishWorkerStartFailure("benchmark", (size_t)bytes, (int)iter);
			}
		}
		else if (action == "cancelBenchmark") {
			/* Cancels whichever job is running; a stale flag is reset at admission. */
			if (g_rng_busy)
				InterlockedExchange(&benchmark_cancel_requested, 1);
		}
		else if (action == "clearHistory") {
			health_monitor_clear_history();
			EnterCriticalSection(&g_timeline_lock);
			g_last_timeline.valid = false;
			LeaveCriticalSection(&g_timeline_lock);
		}
		else if (action == "configure") {
			/* Fields are optional: only the ones present are changed. */
			rng_settings settings;
			long long osr = 0;
			bool flag = false;
			bridge::GetResult res;

			if (g_rng_busy) {
				PublishRejection("configure", RNG_ERR_BUSY);
				return S_OK;
			}

			rng_get_settings(&settings);

			res = bridge::GetInt(msg, "osr", 0, 65535, osr);
			if (res == bridge::kInvalid) {
				PublishRejection("configure", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (res == bridge::kOk)
				settings.osr = (unsigned int)osr;

			res = bridge::GetBool(msg, "forceFips", flag);
			if (res == bridge::kInvalid) {
				PublishRejection("configure", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (res == bridge::kOk)
				settings.force_fips = flag;

			res = bridge::GetBool(msg, "ntg1", flag);
			if (res == bridge::kInvalid) {
				PublishRejection("configure", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (res == bridge::kOk)
				settings.ntg1 = flag;

			res = bridge::GetBool(msg, "disableMemoryAccess", flag);
			if (res == bridge::kInvalid) {
				PublishRejection("configure", RNG_ERR_INVALID_ARGUMENT);
				return S_OK;
			}
			if (res == bridge::kOk)
				settings.disable_memory_access = flag;

			/* g_rng_busy == 0 here (checked above): no worker thread can be
			 * touching the collector, so this mirrors WM_DESTROY's shutdown. */
			shutdown_rng();
			PublishRejection("configure", rng_configure(&settings));
		}
		else {
			printf("Ignoring unknown bridge action\n");
		}

		return S_OK;
	}
};

static std::wstring GuiErrorPage(const wchar_t *message)
{
	return std::wstring(L"<!DOCTYPE html><html><body><h2>") + message + L"</h2></body></html>";
}

static std::wstring LoadGuiHtmlFile()
{
	wchar_t exe_path[MAX_PATH] = {0};
	DWORD exe_len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
	if (exe_len == 0 || exe_len >= MAX_PATH) {
		printf("ERROR: Could not determine the executable path\n");
		return GuiErrorPage(L"Error: could not locate the application directory");
	}
	wchar_t *last_slash = wcsrchr(exe_path, L'\\');
	if (last_slash) *last_slash = L'\0';

	/* The packaged GUI always loads the asset copied beside this executable. */
	std::wstring asset_path = std::wstring(exe_path) + L"\\gui\\index.html";
	FILE *f = _wfopen(asset_path.c_str(), L"rb");

	if (!f) {
		printf("ERROR: Could not open frontend HTML at: %ls\n", asset_path.c_str());
		return GuiErrorPage(L"Error: index.html not found");
	}

	printf("Loading frontend HTML from: %ls\n", asset_path.c_str());

	long sz = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		sz = ftell(f);
	if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		printf("ERROR: Could not size %ls\n", asset_path.c_str());
		return GuiErrorPage(L"Error reading index.html");
	}

	std::string content((size_t)sz, '\0');
	if (fread(&content[0], 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		printf("ERROR: Failed to read file content from %ls\n", asset_path.c_str());
		return GuiErrorPage(L"Error reading index.html");
	}
	fclose(f);

	/* Explicit length (not -1): an embedded NUL must not silently truncate the page. */
	int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), (int)content.size(), NULL, 0);
	if (wlen <= 0) {
		printf("ERROR: index.html is not valid UTF-8\n");
		return GuiErrorPage(L"Error: index.html is not valid UTF-8");
	}
	std::wstring wstr((size_t)wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), (int)content.size(), &wstr[0], wlen);
	return wstr;
}

class EnvironmentCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
private:
	LONG m_refCount;
	HWND m_hWnd;

public:
	EnvironmentCreatedHandler(HWND hWnd) : m_refCount(1), m_hWnd(hWnd) {}
	virtual ~EnvironmentCreatedHandler() {}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return InterlockedIncrement(&m_refCount);
	}

	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&m_refCount);
		if (count == 0) delete this;
		return count;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (!ppvObject) return E_POINTER;
		if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
			*ppvObject = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override;
};

class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
private:
	LONG m_refCount;
	HWND m_hWnd;

public:
	ControllerCreatedHandler(HWND hWnd) : m_refCount(1), m_hWnd(hWnd) {}
	virtual ~ControllerCreatedHandler() {}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return InterlockedIncrement(&m_refCount);
	}

	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&m_refCount);
		if (count == 0) delete this;
		return count;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (!ppvObject) return E_POINTER;
		if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
			*ppvObject = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override {
		if (FAILED(result) || !controller) {
			printf("Controller creation failed: 0x%08X\n", (unsigned int)result);
			fflush(stdout);
			MessageBoxA(m_hWnd, "The embedded dashboard could not be initialized. Please verify that the Microsoft Edge WebView2 Runtime is installed.", "Jitterentropy", MB_OK | MB_ICONERROR);
			DestroyWindow(m_hWnd);
			return result;
		}

		g_controller = controller;
		g_controller->AddRef();
		if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || !g_webview) {
			printf("get_CoreWebView2 failed\n");
			MessageBoxA(m_hWnd, "The embedded dashboard could not be initialized.", "Jitterentropy", MB_OK | MB_ICONERROR);
			DestroyWindow(m_hWnd);
			return E_FAIL;
		}

		RECT bounds;
		GetClientRect(m_hWnd, &bounds);
		g_controller->put_Bounds(bounds);

		/* Register inbound JS web message handler */
		WebMessageReceivedHandler *handler = new WebMessageReceivedHandler();
		HRESULT add_hr = g_webview->add_WebMessageReceived(handler, &g_message_token);
		handler->Release(); /* WebView2 took its own reference */
		if (FAILED(add_hr)) {
			/* Without this every button in the page would be dead. */
			printf("add_WebMessageReceived failed: 0x%08X\n", (unsigned int)add_hr);
			MessageBoxA(m_hWnd, "The dashboard could not register its message handler.", "Jitterentropy", MB_OK | MB_ICONERROR);
			DestroyWindow(m_hWnd);
			return add_hr;
		}

		/* Start 30ms outbound telemetry timer */
		SetTimer(m_hWnd, IDT_BRIDGE_TIMER, 30, NULL);

		std::wstring html_content = LoadGuiHtmlFile();
		g_webview->NavigateToString(html_content.c_str());

		printf("WebView2 controller initialized successfully and frontend UI loaded!\n");
		fflush(stdout);
		return S_OK;
	}
};

HRESULT STDMETHODCALLTYPE EnvironmentCreatedHandler::Invoke(HRESULT result, ICoreWebView2Environment *env) {
	if (FAILED(result) || !env) {
		printf("Environment creation failed: 0x%08X\n", (unsigned int)result);
		fflush(stdout);
		MessageBoxA(m_hWnd, "The embedded dashboard could not be initialized. Please verify that the Microsoft Edge WebView2 Runtime is installed.", "Jitterentropy", MB_OK | MB_ICONERROR);
		DestroyWindow(m_hWnd);
		return result;
	}
	printf("WebView2 environment created successfully. Creating controller...\n");
	fflush(stdout);
	ControllerCreatedHandler *handler = new ControllerCreatedHandler(m_hWnd);
	HRESULT ctrl_hr = env->CreateCoreWebView2Controller(m_hWnd, handler);
	handler->Release(); /* WebView2 holds its own reference while pending */
	return ctrl_hr;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_TIMER:
		if (wParam == IDT_BRIDGE_TIMER && g_webview) {
			std::string json_utf8 = BuildTelemetryJson();
			int wide_len = MultiByteToWideChar(CP_UTF8, 0, json_utf8.c_str(), -1, NULL, 0);
			if (wide_len > 0) {
				std::wstring wide_str(wide_len, 0);
				MultiByteToWideChar(CP_UTF8, 0, json_utf8.c_str(), -1, &wide_str[0], wide_len);
				HRESULT post_hr = g_webview->PostWebMessageAsJson(wide_str.c_str());
				if (FAILED(post_hr)) {
					static bool logged = false; /* one line, not 33 per second */
					if (!logged) {
						logged = true;
						printf("PostWebMessageAsJson failed: 0x%08X\n", (unsigned int)post_hr);
					}
				}
				SecureZeroMemory(&wide_str[0], wide_str.size() * sizeof(wchar_t));
			}
			WipeString(json_utf8);
		}
		break;

	case WM_SIZE:
		if (g_controller) {
			RECT bounds;
			GetClientRect(hWnd, &bounds);
			g_controller->put_Bounds(bounds);
		}
		break;

	case WM_DESTROY:
		KillTimer(hWnd, IDT_BRIDGE_TIMER);
		/* Don't leave a frozen-looking window up while a worker winds down. */
		ShowWindow(hWnd, SW_HIDE);
		InterlockedExchange(&benchmark_cancel_requested, 1);

		EnterCriticalSection(&g_worker_thread_lock);
		if (g_worker_thread) {
			if (WaitForSingleObject(g_worker_thread, WORKER_SHUTDOWN_TIMEOUT_MS) != WAIT_OBJECT_0) {
				/* A collector call that never returns must not hang the exit. */
				printf("Worker did not stop within %d ms; exiting anyway.\n", WORKER_SHUTDOWN_TIMEOUT_MS);
				ExitProcess(0);
			}
			CloseHandle(g_worker_thread);
			g_worker_thread = NULL;
		}
		LeaveCriticalSection(&g_worker_thread_lock);
		shutdown_rng();

		/* Tear WebView2 down before its callbacks' state goes away. */
		if (g_webview) {
			g_webview->remove_WebMessageReceived(g_message_token);
			g_webview->Release();
			g_webview = NULL;
		}
		if (g_controller) {
			g_controller->Close(); /* otherwise msedgewebview2.exe can outlive us */
			g_controller->Release();
			g_controller = NULL;
		}

		DeleteCriticalSection(&g_rng_busy_lock);
		DeleteCriticalSection(&g_worker_thread_lock);
		DeleteCriticalSection(&g_result_lock);
		DeleteCriticalSection(&g_timeline_lock);

		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

int main() {
	setvbuf(stdout, NULL, _IONBF, 0);
	SetProcessDPIAware(); /* otherwise the WebView2 surface is bitmap-scaled on high-DPI displays */

	EnsureQpcTimerInitialized();
	InitializeCriticalSection(&g_rng_busy_lock);
	InitializeCriticalSection(&g_worker_thread_lock);
	InitializeCriticalSection(&g_result_lock);
	InitializeCriticalSection(&g_timeline_lock);

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		printf("CoInitializeEx failed: 0x%08X\n", (unsigned int)hr);
		DeleteCriticalSection(&g_rng_busy_lock);
		DeleteCriticalSection(&g_worker_thread_lock);
		DeleteCriticalSection(&g_result_lock);
		DeleteCriticalSection(&g_timeline_lock);
		return 1;
	}

	HINSTANCE hInstance = GetModuleHandle(NULL);
	WNDCLASSEX wc = {};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = _T("WebView2TestWindowClass");

	if (!RegisterClassEx(&wc)) {
		printf("RegisterClassEx failed!\n");
		return 1;
	}

	HWND hWnd = CreateWindowEx(
		0,
		_T("WebView2TestWindowClass"),
		_T("Jitterentropy - WebView2 C<->JS Bridge Test"),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
		NULL, NULL, hInstance, NULL
	);

	if (!hWnd) {
		printf("CreateWindowEx failed!\n");
		return 1;
	}

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	wchar_t user_data_folder[MAX_PATH] = {0};
	wchar_t local_app_data[MAX_PATH] = {0};
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) > 0) {
		swprintf(user_data_folder, MAX_PATH, L"%s\\JitterentropyApp\\WebView2", local_app_data);
	} else if (GetEnvironmentVariableW(L"TEMP", local_app_data, MAX_PATH) > 0) {
		swprintf(user_data_folder, MAX_PATH, L"%s\\JitterentropyApp\\WebView2", local_app_data);
	}
	PCWSTR pUserDataFolder = (user_data_folder[0] != L'\0') ? user_data_folder : NULL;

	printf("Initializing WebView2 environment...\n");
	EnvironmentCreatedHandler *env_handler = new EnvironmentCreatedHandler(hWnd);
	hr = CreateCoreWebView2EnvironmentWithOptions(NULL, pUserDataFolder, NULL, env_handler);
	env_handler->Release();
	if (FAILED(hr)) {
		printf("CreateCoreWebView2EnvironmentWithOptions returned error: 0x%08X\n", (unsigned int)hr);
		MessageBoxA(hWnd, "The embedded dashboard could not be initialized. Please verify that the Microsoft Edge WebView2 Runtime is installed.", "Jitterentropy", MB_OK | MB_ICONERROR);
		DestroyWindow(hWnd);
		CoUninitialize();
		return 1;
	}

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	CoUninitialize();
	return 0;
}
