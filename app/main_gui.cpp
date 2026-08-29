#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <timeapi.h>
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

extern volatile LONG benchmark_cancel_requested;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#include "WebView2.h"
#pragma GCC diagnostic pop

#define IDT_BRIDGE_TIMER 1001

static ICoreWebView2Controller *g_controller = NULL;
static ICoreWebView2 *g_webview = NULL;

/*
 * High-Precision Process-Relative Time Base
 */
static LARGE_INTEGER g_qpc_frequency = {};
static LARGE_INTEGER g_qpc_process_start = {};

static void EnsureQpcTimerInitialized(void)
{
	if (!g_qpc_frequency.QuadPart) {
		QueryPerformanceFrequency(&g_qpc_frequency);
		QueryPerformanceCounter(&g_qpc_process_start);
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
 * CPU Timing Jitter Sampler Data Structures & Ring Buffer
 */
#define JITTER_RING_SIZE 256

static double g_jitter_ring[JITTER_RING_SIZE];
static size_t g_jitter_write_idx = 0;
static size_t g_jitter_samples_written = 0;
static CRITICAL_SECTION g_jitter_lock;
static volatile bool g_sampler_running = false;
static HANDLE g_sampler_thread = NULL;

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

static void CloseCompletedWorkerHandle(void)
{
	EnterCriticalSection(&g_worker_thread_lock);
	if (g_worker_thread && WaitForSingleObject(g_worker_thread, 0) == WAIT_OBJECT_0) {
		CloseHandle(g_worker_thread);
		g_worker_thread = NULL;
	}
	LeaveCriticalSection(&g_worker_thread_lock);
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
	LeaveCriticalSection(&g_result_lock);
	InterlockedExchange(&g_rng_busy, 0);
}

static DWORD WINAPI JitterSamplerThread(LPVOID lpParam)
{
	(void)lpParam;
	LARGE_INTEGER freq, t1, t2;
	QueryPerformanceFrequency(&freq);

	timeBeginPeriod(1);

	while (g_sampler_running) {
		QueryPerformanceCounter(&t1);
		volatile int sink = 0;
		sink++;
		QueryPerformanceCounter(&t2);

		double delta_us = (double)(t2.QuadPart - t1.QuadPart) * 1e6 / (double)freq.QuadPart;

		EnterCriticalSection(&g_jitter_lock);
		g_jitter_ring[g_jitter_write_idx] = delta_us;
		g_jitter_write_idx = (g_jitter_write_idx + 1) % JITTER_RING_SIZE;
		if (g_jitter_samples_written < JITTER_RING_SIZE)
			g_jitter_samples_written++;
		LeaveCriticalSection(&g_jitter_lock);

		Sleep(1);
	}

	timeEndPeriod(1);
	return 0;
}

size_t GetJitterSamplesSnapshot(double *out_buffer, size_t max_count)
{
	if (!out_buffer || max_count == 0)
		return 0;

	EnterCriticalSection(&g_jitter_lock);
	size_t available = g_jitter_samples_written;
	size_t count_to_copy = available < max_count ? available : max_count;

	if (count_to_copy == 0) {
		LeaveCriticalSection(&g_jitter_lock);
		return 0;
	}

	size_t start_idx;
	if (g_jitter_samples_written < JITTER_RING_SIZE) {
		start_idx = g_jitter_write_idx - count_to_copy;
	} else {
		start_idx = (g_jitter_write_idx + JITTER_RING_SIZE - count_to_copy) % JITTER_RING_SIZE;
	}

	for (size_t i = 0; i < count_to_copy; i++) {
		size_t idx = (start_idx + i) % JITTER_RING_SIZE;
		out_buffer[i] = g_jitter_ring[idx];
	}

	LeaveCriticalSection(&g_jitter_lock);
	return count_to_copy;
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

	QueryPerformanceCounter(&req_end);

	if (init_ret == 0 && byte_count > 0) {
		unsigned char *buf = (unsigned char *)malloc(byte_count);
		if (buf) {
			QueryPerformanceCounter(&ent_start);
			gen_ret = get_random_bytes(buf, byte_count);
			QueryPerformanceCounter(&ent_end);

			duration = QpcElapsedSeconds(ent_start, ent_end);

			QueryPerformanceCounter(&proc_start);
			if (gen_ret == 0) {
				size_t preview_len = byte_count > 64 ? 64 : byte_count;
				std::stringstream ss;
				for (size_t i = 0; i < preview_len; i++) {
					ss << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i];
				}
				if (byte_count > 64) ss << "...";
				hex_str = ss.str();
			}
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
	g_pending_worker_result.bytes = byte_count;
	g_pending_worker_result.iterations = 1;
	g_pending_worker_result.duration = duration;
	g_pending_worker_result.hex_data = hex_str;
	LeaveCriticalSection(&g_result_lock);

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
	InterlockedExchange(&benchmark_cancel_requested, 0);

	QueryPerformanceCounter(&req_end);
	QueryPerformanceCounter(&ent_start);

	int res = run_benchmark(bytes_per_call, iterations);

	QueryPerformanceCounter(&ent_end);
	double duration = QpcElapsedSeconds(ent_start, ent_end);

	QueryPerformanceCounter(&proc_start);
	EnterCriticalSection(&g_result_lock);
	g_pending_worker_result.valid = true;
	g_pending_worker_result.action = "benchmark";
	g_pending_worker_result.status = res;
	g_pending_worker_result.bytes = bytes_per_call;
	g_pending_worker_result.iterations = iterations;
	g_pending_worker_result.duration = duration;
	g_pending_worker_result.hex_data = "";
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
	size_t jitter_count = GetJitterSamplesSnapshot(jitter_buf, 256);

	rng_health_stats health;
	health_monitor_snapshot(&health);

	double hist_ts[300];
	uint64_t hist_bytes[300];
	double hist_dur[300];
	size_t hist_count = health_monitor_recent_history_snapshot(hist_ts, hist_bytes, hist_dur, 300);

	WorkerResultPayload worker_res = {};
	EnterCriticalSection(&g_result_lock);
	if (g_pending_worker_result.valid) {
		worker_res = g_pending_worker_result;
		g_pending_worker_result.valid = false;
	}
	LeaveCriticalSection(&g_result_lock);

	std::stringstream ss;
	ss << std::fixed << std::setprecision(6);

	ss << "{\n";
	ss << "  \"type\": \"telemetry\",\n";
	ss << "  \"busy\": " << (g_rng_busy ? "true" : "false") << ",\n";

	ss << "  \"jitter\": [";
	for (size_t i = 0; i < jitter_count; i++) {
		ss << jitter_buf[i] << (i + 1 == jitter_count ? "" : ",");
	}
	ss << "],\n";

	ss << "  \"health\": {\n";
	ss << "    \"initialized\": " << (health.initialized ? "true" : "false") << ",\n";
	ss << "    \"total_bytes_generated\": " << health.total_bytes_generated << ",\n";
	ss << "    \"generation_call_count\": " << health.generation_call_count << ",\n";
	ss << "    \"failure_count\": " << health.failure_count << ",\n";
	ss << "    \"last_error_code\": " << health.last_error_code << ",\n";
	ss << "    \"cumulative_generation_seconds\": " << health.cumulative_generation_seconds << ",\n";
	ss << "    \"average_latency_seconds\": " << health.average_latency_seconds << ",\n";
	ss << "    \"throughput_bytes_per_second\": " << health.throughput_bytes_per_second << "\n";
	ss << "  },\n";

	ss << "  \"history\": {\n";
	ss << "    \"count\": " << hist_count << ",\n";
	ss << "    \"timestamps\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << hist_ts[i] << (i + 1 == hist_count ? "" : ",");
	}
	ss << "],\n";
	ss << "    \"bytes\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << hist_bytes[i] << (i + 1 == hist_count ? "" : ",");
	}
	ss << "],\n";
	ss << "    \"durations\": [";
	for (size_t i = 0; i < hist_count; i++) {
		ss << hist_dur[i] << (i + 1 == hist_count ? "" : ",");
	}
	ss << "]\n";
	ss << "  },\n";

	if (worker_res.action.length() > 0) {
		ss << "  \"actionResult\": {\n";
		ss << "    \"action\": \"" << worker_res.action << "\",\n";
		ss << "    \"status\": " << worker_res.status << ",\n";
		ss << "    \"bytes\": " << worker_res.bytes << ",\n";
		ss << "    \"iterations\": " << worker_res.iterations << ",\n";
		ss << "    \"duration\": " << worker_res.duration << ",\n";
		ss << "    \"hex\": \"" << worker_res.hex_data << "\"\n";
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
		ss << "    \"request\": {\"start\": " << current_timeline.request.start_seconds << ", \"duration\": " << current_timeline.request.duration_seconds << "},\n";
		ss << "    \"entropy\": {\"start\": " << current_timeline.entropy.start_seconds << ", \"duration\": " << current_timeline.entropy.duration_seconds << "},\n";
		ss << "    \"processing\": {\"start\": " << current_timeline.processing.start_seconds << ", \"duration\": " << current_timeline.processing.duration_seconds << "},\n";
		ss << "    \"serialization\": {\"start\": " << current_timeline.serialization.start_seconds << ", \"duration\": " << current_timeline.serialization.duration_seconds << "}\n";
		ss << "  }\n";
	} else {
		ss << "  \"timeline\": null\n";
	}

	ss << "}";
	return ss.str();
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
		std::string json_str(len, 0);
		WideCharToMultiByte(CP_UTF8, 0, json_wide, -1, &json_str[0], len, NULL, NULL);
		CoTaskMemFree(json_wide);

		if (json_str.find("\"action\":\"generate\"") != std::string::npos ||
		    json_str.find("\"action\": \"generate\"") != std::string::npos) {
			size_t bytes = 32;
			size_t pos = json_str.find("\"bytes\":");
			if (pos != std::string::npos) {
				bytes = (size_t)atoi(json_str.c_str() + pos + 8);
			}
			if (bytes == 0) bytes = 32;

			if (InterlockedCompareExchange(&g_rng_busy, 1, 0) == 0) {
				CloseCompletedWorkerHandle();
				GenerateWorkerArgs *wargs = new GenerateWorkerArgs();
				wargs->byte_count = bytes;
				HANDLE hThread = CreateThread(NULL, 0, GenerateWorkerThread, wargs, 0, NULL);
				if (hThread) {
					EnterCriticalSection(&g_worker_thread_lock);
					g_worker_thread = hThread;
					LeaveCriticalSection(&g_worker_thread_lock);
				} else {
					delete wargs;
					PublishWorkerStartFailure("generate", bytes, 1);
				}
			}
		}
		else if (json_str.find("\"action\":\"benchmark\"") != std::string::npos ||
			 json_str.find("\"action\": \"benchmark\"") != std::string::npos) {
			size_t bytes = 1024;
			int iter = 100;
			size_t pos_b = json_str.find("\"bytesPerCall\":");
			if (pos_b != std::string::npos) {
				bytes = (size_t)atoi(json_str.c_str() + pos_b + 15);
			}
			size_t pos_i = json_str.find("\"iterations\":");
			if (pos_i != std::string::npos) {
				iter = atoi(json_str.c_str() + pos_i + 13);
			}
			if (bytes == 0) bytes = 1024;
			if (iter <= 0) iter = 100;

			if (InterlockedCompareExchange(&g_rng_busy, 1, 0) == 0) {
				CloseCompletedWorkerHandle();
				BenchmarkWorkerArgs *wargs = new BenchmarkWorkerArgs();
				wargs->bytes_per_call = bytes;
				wargs->iterations = iter;
				HANDLE hThread = CreateThread(NULL, 0, BenchmarkWorkerThread, wargs, 0, NULL);
				if (hThread) {
					EnterCriticalSection(&g_worker_thread_lock);
					g_worker_thread = hThread;
					LeaveCriticalSection(&g_worker_thread_lock);
				} else {
					delete wargs;
					PublishWorkerStartFailure("benchmark", bytes, iter);
				}
			}
		}
		else if (json_str.find("\"action\":\"cancelBenchmark\"") != std::string::npos ||
			 json_str.find("\"action\": \"cancelBenchmark\"") != std::string::npos) {
			InterlockedExchange(&benchmark_cancel_requested, 1);
		}
		else if (json_str.find("\"action\":\"clearHistory\"") != std::string::npos ||
			 json_str.find("\"action\": \"clearHistory\"") != std::string::npos) {
			health_monitor_clear_history();
		}

		return S_OK;
	}
};

static std::wstring LoadGuiHtmlFile()
{
	char exe_dir[MAX_PATH] = {0};
	GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
	char *last_slash = strrchr(exe_dir, '\\');
	if (last_slash) *last_slash = '\0';

	/* The packaged GUI always loads the asset copied beside this executable. */
	char resolved_path[MAX_PATH] = {0};
	std::string asset_path = std::string(exe_dir) + "\\gui\\index.html";
	GetFullPathNameA(asset_path.c_str(), MAX_PATH, resolved_path, NULL);
	FILE *f = fopen(resolved_path, "rb");

	if (!f) {
		printf("ERROR: Could not open frontend HTML at: %s\n", resolved_path);
		fflush(stdout);
		return L"<!DOCTYPE html><html><body><h2>Error: index.html not found</h2></body></html>";
	}

	printf("Loading frontend HTML from: %s\n", resolved_path);
	fflush(stdout);

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	std::string content(sz, 0);
	if (fread(&content[0], 1, sz, f) != (size_t)sz) {
		fclose(f);
		printf("ERROR: Failed to read file content from %s\n", resolved_path);
		fflush(stdout);
		return L"<!DOCTYPE html><html><body><h2>Error reading index.html</h2></body></html>";
	}
	fclose(f);

	int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, NULL, 0);
	if (wlen <= 0) return L"";
	std::wstring wstr(wlen, 0);
	MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, &wstr[0], wlen);
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
		g_controller->get_CoreWebView2(&g_webview);

		RECT bounds;
		GetClientRect(m_hWnd, &bounds);
		g_controller->put_Bounds(bounds);

		/* Register inbound JS web message handler */
		EventRegistrationToken token;
		g_webview->add_WebMessageReceived(new WebMessageReceivedHandler(), &token);

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
	return env->CreateCoreWebView2Controller(m_hWnd, new ControllerCreatedHandler(m_hWnd));
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
				g_webview->PostWebMessageAsJson(wide_str.c_str());
			}
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
		InterlockedExchange(&benchmark_cancel_requested, 1);

		EnterCriticalSection(&g_worker_thread_lock);
		if (g_worker_thread) {
			WaitForSingleObject(g_worker_thread, INFINITE);
			CloseHandle(g_worker_thread);
			g_worker_thread = NULL;
		}
		LeaveCriticalSection(&g_worker_thread_lock);
		shutdown_rng();

		if (g_sampler_thread) {
			g_sampler_running = false;
			WaitForSingleObject(g_sampler_thread, INFINITE);
			CloseHandle(g_sampler_thread);
			g_sampler_thread = NULL;
		}

		DeleteCriticalSection(&g_jitter_lock);
		DeleteCriticalSection(&g_rng_busy_lock);
		DeleteCriticalSection(&g_worker_thread_lock);
		DeleteCriticalSection(&g_result_lock);
		DeleteCriticalSection(&g_timeline_lock);

		if (g_webview) {
			g_webview->Release();
			g_webview = NULL;
		}
		if (g_controller) {
			g_controller->Release();
			g_controller = NULL;
		}
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

int main() {
	setvbuf(stdout, NULL, _IONBF, 0);

	EnsureQpcTimerInitialized();
	InitializeCriticalSection(&g_jitter_lock);
	InitializeCriticalSection(&g_rng_busy_lock);
	InitializeCriticalSection(&g_worker_thread_lock);
	InitializeCriticalSection(&g_result_lock);
	InitializeCriticalSection(&g_timeline_lock);

	g_sampler_running = true;
	g_sampler_thread = CreateThread(NULL, 0, JitterSamplerThread, NULL, 0, NULL);
	if (!g_sampler_thread) {
		printf("Failed to create jitter sampler thread!\n");
		DeleteCriticalSection(&g_jitter_lock);
		DeleteCriticalSection(&g_rng_busy_lock);
		DeleteCriticalSection(&g_worker_thread_lock);
		DeleteCriticalSection(&g_result_lock);
		DeleteCriticalSection(&g_timeline_lock);
		return 1;
	}

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		printf("CoInitializeEx failed: 0x%08X\n", (unsigned int)hr);
		g_sampler_running = false;
		WaitForSingleObject(g_sampler_thread, INFINITE);
		CloseHandle(g_sampler_thread);
		DeleteCriticalSection(&g_jitter_lock);
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
	hr = CreateCoreWebView2EnvironmentWithOptions(NULL, pUserDataFolder, NULL, new EnvironmentCreatedHandler(hWnd));
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
