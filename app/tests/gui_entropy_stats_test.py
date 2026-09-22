"""Manual integration test: verifies the entropy-analysis telemetry (library
status, the jitter-sample RCT/APT overlay, per-generate entropyStats, and the
configure bridge action) over WebView2's CDP port.

Not part of ctest (it opens the real window and takes about a minute). Needs
the GUI built in ../../runtime and `pip install websockets`.
"""
import asyncio, ctypes, json, os, subprocess, time, urllib.request
import websockets

EXE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    '..', '..', 'runtime', 'jitterentropy_gui.exe'))
PORT = 9335
results = []


def check(name, cond, detail=''):
    results.append(cond)
    print(('PASS ' if cond else 'FAIL ') + name + (('  -> ' + str(detail)) if detail else ''), flush=True)


class Cdp:
    def __init__(self, ws):
        self.ws, self.n = ws, 0

    async def ev(self, expr):
        self.n += 1
        await self.ws.send(json.dumps({'id': self.n, 'method': 'Runtime.evaluate',
                                       'params': {'expression': expr, 'returnByValue': True, 'awaitPromise': True}}))
        while True:
            m = json.loads(await self.ws.recv())
            if m.get('id') == self.n:
                if 'exceptionDetails' in m.get('result', {}):
                    raise RuntimeError(m['result']['exceptionDetails'])
                return m['result']['result'].get('value')

    async def send(self, obj):
        await self.ev(f'window.chrome.webview.postMessage({json.dumps(obj)}); true')

    async def latest(self):
        return await self.ev('window.__t')

    async def results_list(self):
        return await self.ev('window.__ar')

    async def clear(self):
        await self.ev('window.__ar.length = 0; true')

    async def wait_result(self, timeout, action=None):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for r in await self.results_list():
                if action is None or r['action'] == action:
                    return r
            await asyncio.sleep(0.1)
        return None

    async def wait_idle(self, timeout=10):
        t0 = time.time()
        while time.time() - t0 < timeout:
            t = await self.latest()
            if t and not t['busy']:
                return True
            await asyncio.sleep(0.1)
        return False


async def main():
    env = dict(os.environ, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f'--remote-debugging-port={PORT}')
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pid = proc.pid
    page = None
    for _ in range(60):
        try:
            pages = [t for t in json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json', timeout=1)) if t.get('type') == 'page']
            if pages:
                page = pages[0]; break
        except Exception:
            pass
        time.sleep(0.5)
    check('attached to WebView2 debug port', page is not None)

    async with websockets.connect(page['webSocketDebuggerUrl'], max_size=None) as ws:
        c = Cdp(ws)
        await c.ev("""window.__t=null; window.__ar=[];
            window.chrome.webview.addEventListener('message', e => {
              if (e.data && e.data.type === 'telemetry') {
                window.__t = e.data;
                if (e.data.actionResult) window.__ar.push(e.data.actionResult);
              }
            }); true""")
        await asyncio.sleep(1.0)

        # ---- library.version present at idle; library.status is honestly null:
        # no collector exists until the first init_rng(), inside the first job. ----
        t = await c.latest()
        check('telemetry has library object', t is not None and 'library' in t)
        check('library.version looks like a semver string', bool(t['library']['version'].count('.') == 2))
        check('library.status is null before any RNG activity (no collector yet)',
              t['library']['status'] is None)

        # ---- jitterHealth (RCT/APT overlay), present even before any generate ----
        check('jitterHealth present', 'jitterHealth' in t)
        jh = t['jitterHealth']
        check('jitterHealth.rct has cutoff/maxRun/passed', all(k in jh['rct'] for k in ('cutoff', 'maxRun', 'passed')))
        check('jitterHealth.apt has windowSize/cutoff/windowsTested/windowsFailed/passed',
              all(k in jh['apt'] for k in ('windowSize', 'cutoff', 'windowsTested', 'windowsFailed', 'passed')))

        # ---- generate a real buffer, check jitter samples appear (real per-call latency now) ----
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 8192})  # chunked at 4096B -> >=2 jitter samples
        r = await c.wait_result(15, 'generate')
        check('generate succeeded', r is not None and r['status'] == 0, f"status={r and r['status']}")
        await asyncio.sleep(0.5)
        t = await c.latest()
        check('jitter array now has real per-call latency samples (>=2, chunked)', len(t['jitter']) >= 2,
              f"count={len(t['jitter'])}")
        check('jitter samples are positive microsecond latencies, not a synthetic waveform',
              all(x > 0 for x in t['jitter']), f"sample={t['jitter'][:3]}")

        check('library.status now present (collector exists after first generate)',
              t['library']['status'] is not None)
        st = t['library']['status']
        check('library.status has healthFailure.rct/apt/lag', all(k in st['healthFailure'] for k in ('rct', 'apt', 'lag')))
        check('library.status.configuration.osr present', 'osr' in st['configuration'])
        check('library.status.runtimeEnvironment.cpuCores present', st['runtimeEnvironment']['cpuCores'] > 0)

        # ---- entropyStats on the actionResult of a real generate ----
        check('actionResult.entropyStats present on generate', r.get('entropyStats') is not None)
        es = r['entropyStats']
        check('entropyStats.byteCount == requested bytes', es['byteCount'] == 8192, f"got {es['byteCount']}")
        check('entropyStats.shannonBitsPerByte in a plausible range for real random data',
              7.0 < es['shannonBitsPerByte'] <= 8.0, f"got {es['shannonBitsPerByte']}")
        check('entropyStats.minEntropyBitsPerByte <= shannon (never exceeds it)',
              es['minEntropyBitsPerByte'] <= es['shannonBitsPerByte'])
        check('entropyStats.chiSquare has statistic and pValue', 'statistic' in es['chiSquare'] and 'pValue' in es['chiSquare'])
        check('entropyStats.monobit.pValue in [0,1]', 0.0 <= es['monobit']['pValue'] <= 1.0)
        check('entropyStats.histogram has 256 bins summing to byteCount',
              len(es['histogram']) == 256 and sum(es['histogram']) == es['byteCount'])

        # ---- entropyStats is null for a benchmark action (byte-level stats don't apply) ----
        await c.clear()
        await c.send({'action': 'benchmark', 'bytesPerCall': 512, 'iterations': 5})
        rb = await c.wait_result(15, 'benchmark')
        check('benchmark succeeded', rb is not None and rb['status'] == 0, f"status={rb and rb['status']}")
        check('entropyStats is null for a benchmark action', rb.get('entropyStats') is None)

        # ---- configure: change osr, verify it takes effect on the next generate ----
        await c.clear()
        await c.send({'action': 'configure', 'osr': 5})
        rc = await c.wait_result(5, 'configure')
        check('configure accepted', rc is not None and rc['status'] == 0, f"status={rc and rc['status']}")

        await c.clear()
        await c.send({'action': 'generate', 'bytes': 16})
        rg = await c.wait_result(10, 'generate')
        check('generate after configure succeeded', rg is not None and rg['status'] == 0)
        await asyncio.sleep(0.5)
        t = await c.latest()
        applied_osr = t['library']['status']['configuration']['osr']
        check('configure(osr=5) actually took effect on the next collector', applied_osr == 5,
              f"configuration.osr={applied_osr}")

        # ---- configure rejected while busy ----
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 65536})
        await asyncio.sleep(0.5)
        await c.send({'action': 'configure', 'osr': 7})
        rcfg = await c.wait_result(3, 'configure')
        check('configure rejected while busy (RNG_ERR_BUSY = -1007)', rcfg is not None and rcfg['status'] == -1007,
              f"status={rcfg and rcfg['status']}")
        await c.send({'action': 'cancelBenchmark'})
        await c.wait_idle(6)

        # ---- configure rejects a malformed field ----
        await c.clear()
        await c.send({'action': 'configure', 'osr': -1})
        rbad = await c.wait_result(3, 'configure')
        check('configure rejects out-of-range osr', rbad is not None and rbad['status'] == -1005,
              f"status={rbad and rbad['status']}")

        hwnd = ctypes.windll.user32.FindWindowW('WebView2TestWindowClass', None)
        ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)

    proc.wait(timeout=15)
    print(f'\n{sum(results)}/{len(results)} checks passed')
    return 0 if all(results) else 1


import sys
sys.exit(asyncio.run(main()))
