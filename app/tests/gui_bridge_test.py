"""Manual integration test: drives jitterentropy_gui.exe through its JS<->native bridge.

Not part of ctest: it opens the real window and takes about a minute. Needs the GUI built
in ../../runtime and `pip install websockets`. Covers input rejection, Generate/benchmark
cancel, BUSY reporting, and clean shutdown while a job is running.
"""
import asyncio, ctypes, json, os, subprocess, sys, time, urllib.request
import websockets

EXE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    '..', '..', 'runtime', 'jitterentropy_gui.exe'))
PORT = 9333
results = []


def check(name, cond, detail=''):
    results.append(cond)
    print(('PASS ' if cond else 'FAIL ') + name + (('  -> ' + str(detail)) if detail != '' else ''), flush=True)


def ps(cmd):
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True).stdout.strip()


def handle_count(pid):
    try:
        return int(ps(f'(Get-Process -Id {pid}).HandleCount'))
    except ValueError:
        return -1


def webview_children():
    out = ps("(Get-CimInstance Win32_Process -Filter \"name='msedgewebview2.exe'\" | "
             "Where-Object { $_.CommandLine -like '*JitterentropyApp*' }).Count")
    return int(out or 0)


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

    async def results(self):
        return await self.ev('window.__ar')

    async def clear(self):
        await self.ev('window.__ar.length = 0; true')

    async def busy(self):
        return await self.ev('window.__busy')

    async def wait_result(self, timeout, action=None):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for r in await self.results():
                if action is None or r['action'] == action:
                    return r, time.time() - t0
            await asyncio.sleep(0.1)
        return None, time.time() - t0

    async def wait_idle(self, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not await self.busy():
                return True
            await asyncio.sleep(0.1)
        return False


async def main():
    env = dict(os.environ, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f'--remote-debugging-port={PORT}')
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    pid = proc.pid
    page = None
    for _ in range(60):
        try:
            tabs = json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json', timeout=1))
            pages = [t for t in tabs if t.get('type') == 'page']
            if pages:
                page = pages[0]
                break
        except Exception:
            pass
        time.sleep(0.5)
    if not page:
        print('FAIL could not attach to WebView2 debug port'); proc.kill(); return 1

    async with websockets.connect(page['webSocketDebuggerUrl'], max_size=None) as ws:
        c = Cdp(ws)
        await c.ev("""window.__ar = []; window.__busy = false;
            window.chrome.webview.addEventListener('message', e => {
              if (e.data && e.data.type === 'telemetry') {
                window.__busy = e.data.busy;
                if (e.data.actionResult) window.__ar.push(e.data.actionResult);
              }
            }); true""")
        await asyncio.sleep(1.0)
        check('telemetry flowing / page loaded', await c.ev('typeof window.__busy') == 'boolean')

        # ---- 1. hostile / malformed input never starts a job
        for label, msg in [
            ('bytes=500000000 (old hang)', {'action': 'generate', 'bytes': 500000000}),
            ('bytes=-5 (old SIZE_MAX)', {'action': 'generate', 'bytes': -5}),
            ('bytes=1e9 (old parsed as 1)', None),
            ('bytes=0', {'action': 'generate', 'bytes': 0}),
            ('bytes as string', {'action': 'generate', 'bytes': '512'}),
            ('bench iterations=10^9', {'action': 'benchmark', 'bytesPerCall': 1024, 'iterations': 1000000000}),
            ('bench bytesPerCall=10^9', {'action': 'benchmark', 'bytesPerCall': 1000000000, 'iterations': 5}),
        ]:
            await c.clear()
            if msg is None:  # postMessage of an object serialises 1e9 as 1000000000; send raw JSON text instead
                await c.ev("window.chrome.webview.postMessage(JSON.parse('{\"action\":\"generate\",\"bytes\":1e9}')); true")
            else:
                await c.send(msg)
            r, dt = await c.wait_result(3)
            ok = r is not None and r['status'] == -1005 and not await c.busy()
            check(f'reject: {label}', ok, f"status={r and r['status']} busy={await c.busy()} ({dt:.1f}s)")

        # nested "bytes" must not be read; malformed ignored, nothing starts
        await c.clear()
        await c.ev("window.chrome.webview.postMessage({action:'generate', opts:{bytes:999}}); true")
        await asyncio.sleep(1.0)
        check('nested object ignored, no job started', not await c.busy() and not await c.results())

        # ---- 2. Generate is cancellable (previously impossible)
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 65536})   # ~11 s at ~6 KB/s
        await asyncio.sleep(1.2)
        check('generate running (busy)', await c.busy())
        t = time.time()
        await c.send({'action': 'cancelBenchmark'})
        r, dt = await c.wait_result(6, 'generate')
        check('generate cancelled promptly', r is not None and r['status'] == -2001 and dt < 3.0,
              f"status={r and r['status']} produced={r and r['bytes']} of 65536, {time.time()-t:.2f}s after cancel")
        check('cancelled generate produced partial, not full', r is not None and 0 <= r['bytes'] < 65536)
        check('cancelled generate returns no hex', r is not None and r['hex'] == '')
        check('slot free after cancel', await c.wait_idle(5))

        # ---- 3. busy rejection is reported, not silently dropped
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 65536})
        await asyncio.sleep(0.6)
        await c.send({'action': 'generate', 'bytes': 32})
        r, dt = await c.wait_result(3, 'generate')
        check('second request while busy -> RNG_ERR_BUSY(-1007)', r is not None and r['status'] == -1007,
              f"status={r and r['status']}")
        await c.send({'action': 'cancelBenchmark'})
        await c.wait_idle(6)

        # ---- 4. benchmark reports what ACTUALLY ran when cancelled
        await c.clear()
        await c.send({'action': 'benchmark', 'bytesPerCall': 1024, 'iterations': 100000})
        await asyncio.sleep(3.0)
        await c.send({'action': 'cancelBenchmark'})
        r, dt = await c.wait_result(6, 'benchmark')
        check('benchmark cancelled', r is not None and r['status'] == -2001, f"status={r and r['status']}")
        check('benchmark reports completed iterations, not the 100000 requested',
              r is not None and 0 < r['iterations'] < 100000, f"iterations={r and r['iterations']}")
        check('benchmark duration is measured run time (~3s), not init+wall',
              r is not None and 1.0 < r['duration'] < 8.0, f"duration={r and r['duration']:.2f}s")
        await c.wait_idle(6)

        # ---- 5. stale cancel must not poison the next job
        await c.send({'action': 'cancelBenchmark'})       # idle: must be ignored
        await asyncio.sleep(0.3)
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 64})
        r, dt = await c.wait_result(6, 'generate')
        check('idle Cancel does not cancel the next Generate', r is not None and r['status'] == 0 and len(r['hex']) == 128,
              f"status={r and r['status']} hexlen={r and len(r['hex'])}")
        check('hex is 64 random bytes (not all zero)', r is not None and r['hex'] != '0' * 128)

        # ---- 6. handle leak: back-to-back jobs
        await c.wait_idle(5)
        h0 = handle_count(pid)
        for i in range(40):
            await c.clear()
            await c.send({'action': 'generate', 'bytes': 16})
            await c.wait_result(5, 'generate')
        await c.wait_idle(5)
        h1 = handle_count(pid)
        check('handle count flat over 40 back-to-back generates', h1 - h0 <= 3, f'{h0} -> {h1}')

        # ---- 7. shutdown while a long job runs (previously: infinite hang)
        n_before = webview_children()
        await c.clear()
        await c.send({'action': 'generate', 'bytes': 65536})
        await asyncio.sleep(1.0)
        check('long job running before close', await c.busy())
        hwnd = ctypes.windll.user32.FindWindowW('WebView2TestWindowClass', None)
        check('found app window', hwnd != 0)
        t = time.time()
        ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE

    try:
        proc.wait(timeout=20)
        dt = time.time() - t
        check('process exits after close during a running job', True, f'{dt:.2f}s (was: infinite hang)')
        check('exit was prompt (< worker timeout)', dt < 9.0, f'{dt:.2f}s')
    except subprocess.TimeoutExpired:
        check('process exits after close during a running job', False, 'STILL RUNNING after 20s')
        proc.kill()

    await asyncio.sleep(3)
    n_after = webview_children()
    check('no orphaned msedgewebview2 for this app after exit', n_after == 0, f'before={n_before} after={n_after}')

    out = proc.stdout.read().decode(errors='replace') if proc.stdout else ''
    print('--- app stdout (tail) ---'); print('\n'.join(out.splitlines()[-12:]))
    print(f'\n{sum(results)}/{len(results)} checks passed')
    return 0 if all(results) else 1


sys.exit(asyncio.run(main()))
