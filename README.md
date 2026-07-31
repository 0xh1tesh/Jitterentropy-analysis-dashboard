# Jitterentropy Analysis Dashboard

The Jitterentropy Analysis Dashboard is a Windows desktop instrumentation and analysis environment built around the upstream Jitterentropy CPU-timing Random Number Generator (RNG) library.

The core CPU timing entropy generation algorithms, hardware timing collectors, and low-level entropy harvesting mechanisms are provided by the original Jitterentropy project. This repository supplies native Windows wrappers, health monitoring, performance benchmarking, thread-safe telemetry streaming, and a Microsoft WebView2 desktop graphical user interface for real-time engineering diagnostics.

---

## Overview

The purpose of this project is to provide observability, telemetry, and diagnostic visualization for the Jitterentropy RNG on Microsoft Windows. 

The original Jitterentropy library measures fine-grained CPU execution timing deltas to harvest entropy from unpredictable microarchitectural state variations (such as instruction cache misses, branch mispredictions, memory bus contention, and pipeline stalls). This analysis dashboard exposes those internal timing variations and statistical characteristics through live instrumentation graphs without modifying the underlying entropy collection algorithms.

Key objectives:
- Harvest entropy using the vendored upstream Jitterentropy C implementation.
- Measure pipeline stage execution timing honestly using Windows high-resolution performance counters (`QueryPerformanceCounter`).
- Stream real-time telemetry from native C/C++ background worker threads to an embedded HTML5 Canvas dashboard.
- Enable interactive benchmarking and continuous health monitoring.

---

## Features

This repository adds the following instrumentation and diagnostic capabilities around the Jitterentropy library:

- **Windows Desktop Application**: Native Win32 host application with integrated Microsoft WebView2 runtime container.
- **Command-Line Interface (CLI)**: Lightweight utility (`jitterentropy_app`) for automated byte generation, health status queries, and command-line benchmarking.
- **Live Telemetry Bridge**: Single-Threaded Apartment (STA) compliant C↔JS message bridge streaming telemetry updates at ~33 Hz.
- **Oscilloscope Visualization**: High-density phosphor-style waveform displaying continuous CPU-timing jitter deltas alongside live statistical metrics (Minimum, Maximum, Median, P95 Cutoff, and Standard Deviation).
- **Performance Dashboard**: Real-time throughput (MB/s) and call latency history charts backed by a bounded native ring buffer.
- **Pipeline Timeline Analysis**: Saleae Logic Analyzer style stage execution diagram visualizing measured `Request Dispatch`, `Entropy Collection`, `Result Processing`, and `Telemetry Serialization` durations.
- **Health Monitoring**: Continuous tracking of total bytes generated, generation call count, cumulative generation duration, failure count, and last error code.
- **Asynchronous Benchmarking**: Cancellable background benchmarking thread with atomic cancellation flags.
- **Responsive Engineering UI**: Hardware-accelerated HTML5 Canvas visualization built with vanilla JavaScript, CSS, and HTML.

---

## Architecture

The system uses a layered architecture separating core entropy harvesting, native state management, thread-safe telemetry serialization, and frontend presentation:

```text
+-------------------------------------------------------------+
|             Upstream Jitterentropy Core Library             |
|       (jent_entropy_collector_alloc, get_random_bytes)      |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|                     Native Wrapper Layer                    |
|             (rng_wrapper.c / health_monitor.c)              |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|              Health Monitor & Telemetry Bridge              |
|        (300-sample history ring buffer, main_gui.cpp)       |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|             Microsoft WebView2 Runtime Container            |
|               (PostWebMessageAsJson @ ~33 Hz)               |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|             HTML5 / CSS / Vanilla JS Dashboard              |
|            (Oscilloscope, Performance, Timeline)            |
+-------------------------------------------------------------+
```

### Data Flow
1. Background sampler threads measure CPU timing deltas and write to dedicated native ring buffers.
2. User actions (Generate, Benchmark, Cancel) sent from the JavaScript frontend travel through `window.chrome.webview.postMessage()` to native Win32 worker threads.
3. Native worker threads invoke `init_rng()` and `get_random_bytes()`, recording stage timings via `QueryPerformanceCounter()`.
4. A Win32 timer on the main UI thread periodically snapshots jitter, health, history, and timeline metrics, serializes them to JSON, and posts the payload to WebView2 via `PostWebMessageAsJson()`.

---

## Repository Structure

```text
entr_proj/
├── app/                        Application source files and GUI assets
│   ├── CMakeLists.txt          Build manifest for CLI and GUI targets
│   ├── benchmark.c / .h        Benchmarking worker thread implementation
│   ├── health_monitor.c / .h   Health statistics & history ring buffer
│   ├── main.c                  CLI entry point
│   ├── main_gui.cpp            Win32 + WebView2 application entry point
│   ├── rng_wrapper.c / .h      Jitterentropy wrapper layer
│   └── gui/                    Frontend web assets
│       └── index.html          Combined HTML, CSS, and JavaScript interface
├── deps/                       Vendored SDK dependencies
│   └── webview2/               Microsoft.Web.WebView2 SDK headers & import libraries
├── jitterentropy-library/      Vendored upstream Jitterentropy C library source
├── runtime/                    Canonical generated build output directory
└── release/                    Staged distribution package directory
```

---

## Building

### Prerequisites

- **Compiler Toolchain**: MinGW-w64 GCC/G++ 15.2+ (or compatible Win32 C/C++ toolchain).
- **Build System**: CMake 3.20 or newer.
- **Runtime Dependency**: Microsoft Edge WebView2 Runtime (installed by default on Windows 10 21H2+ and Windows 11).

### Build Steps

1. Build the vendored Jitterentropy library:
   ```powershell
   cmake -S jitterentropy-library -B jitterentropy-library/build -G "MinGW Makefiles"
   cmake --build jitterentropy-library/build --target jitterentropy
   ```

2. Build the CLI and GUI applications:
   ```powershell
   cmake -S app -B runtime -G "MinGW Makefiles"
   cmake --build runtime --target jitterentropy_app jitterentropy_gui --parallel 4
   ```

3. Launch the application:
   ```powershell
   .\runtime\jitterentropy_gui.exe
   ```

---

## Screenshots
### Oscilloscope View
<img width="1907" height="1050" alt="ossci" src="https://github.com/user-attachments/assets/74af1240-8b9d-4917-aabd-dbade41eef44" />


### Performance Dashboard
<img width="1911" height="1047" alt="Screenshot 2026-07-31 151125" src="https://github.com/user-attachments/assets/d938d937-493f-400b-9acd-6a43e0426428" />


### Timeline Analysis
<img width="1917" height="1105" alt="Screenshot 2026-07-31 151143" src="https://github.com/user-attachments/assets/5174d20e-67c1-48a2-b25e-f0eab20ce91f" />



---

## Credits

- **Core RNG Library**: The core CPU timing entropy generation algorithm, harvesting routines, and hardware timing mechanisms belong to the upstream **Jitterentropy** project created by Stephan Müller. Upstream repository: [smuellerDD/jitterentropy-library](https://github.com/smuellerDD/jitterentropy-library).
- **Windows Dashboard & Instrumentation Layer**: Developed in this repository to provide Windows Win32 host integration, WebView2 bridge bindings, health monitoring, telemetry serialization, and visualization dashboards around the upstream library.

## License

- **Original Application & Dashboard Code**: The original application code, Win32 host wrapper logic, telemetry bridge, health monitoring system, and frontend HTML5/JS dashboard located in `app/` are licensed under the [MIT License](LICENSE).
- **Bundled Jitterentropy Core Library**: The source code located in `jitterentropy-library/` is governed by the original licensing terms set by Stephan Müller (dual-licensed under the BSD 3-Clause License and GNU General Public License v2; refer to `jitterentropy-library/LICENSE`).
- **Microsoft WebView2 SDK**: The SDK headers and import libraries in `deps/webview2/` are governed by Microsoft's BSD 3-Clause License (refer to `deps/webview2/LICENSE.txt`).
