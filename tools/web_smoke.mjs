// Loads the built WebAssembly client in a headless browser and fails if it is
// not actually rendering.
//
// Why this exists: `cmake --build --preset web` succeeding proves almost
// nothing. Every web-only defect this project has shipped compiled cleanly
// under emcc and broke at runtime in the browser --
//
//   * a `sampler2DShadow` uniform with no precision qualifier (GLSL ES needs
//     one; desktop GLSL does not), so every shader using it failed to compile
//   * a 0x0 canvas at startup, so PostFx::create failed and the client exited
//     before drawing a frame
//   * dynamically indexing a uniform array in the skinning shader, which is
//     fine on desktop GL and a ~1000x cliff in WebGL
//   * a `web` preset with no toolchain file, which quietly produced a native
//     binary -- so "the web build is clean" had been vacuous for weeks
//
// All four are invisible to the compiler and obvious the moment something
// watches frames go by. That is all this does.
//
// Node, not Python, only because driving the DevTools protocol needs a
// WebSocket client and Node has had one built in since v22 -- the alternative
// was a pip dependency. Same rule as the rest of tools/: no install step.
//
// Requires Node 22+ (global WebSocket) and a Chrome or Chromium on the system.
//
// Usage:
//   node tools/web_smoke.mjs [--dir build/web/game] [--chrome /path/to/chrome]
//                            [--zero-canvas] [--keep-open] [--verbose]
//
//   --zero-canvas  hold the canvas at zero height for the first few seconds,
//                  recreating the startup race deliberately (CI runs both).

import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { readFile, mkdtemp, rm, readdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, extname, resolve, normalize, sep } from 'node:path';

// --- thresholds ------------------------------------------------------------
//
// Deliberately loose. These separate "working" from "catastrophically broken",
// which is the only distinction a software rasteriser on a shared CI runner
// can make honestly. A 20% rendering regression will not trip them, and
// pretending otherwise would just make the job flaky.

// A frame of the arena quantised to 5 bits per channel measures 300-380
// populated buckets (M4 Mac 375, GitHub runner 297); a flat clear colour
// measures 1. The gap either side of the floor is enormous in both directions,
// so it sits an order of magnitude below the lowest observed value rather than
// hugging it -- the palette shifts legitimately with resolution and driver.
const MIN_DISTINCT_COLORS = 32;

// Measured healthy frame rates, both under SwiftShader:
//
//   M4 Mac, local          ~40 fps
//   GitHub ubuntu-latest   5.5-7 fps
//
// and the historical uniform-array cliff, on a developer machine, 0.67 fps.
//
// The floor is set from the SLOWEST healthy environment, not the fastest: a
// shared CI runner is 6-7x slower than a laptop and varies run to run, so a
// floor calibrated locally would fail on a noisy neighbour rather than on a
// regression. At 1 fps this catches roughly a 5x slowdown or worse on the
// runner, and anything more severe than ~10x never reaches the fingerprint at
// all, so the startup check fires first. It is a backstop for the middle band,
// not a benchmark -- raising it buys flakiness, not sensitivity.
const MIN_FPS = 1;
const STARTUP_TIMEOUT_MS = 90_000;  // generous: a cold runner compiling wasm
// Long enough that even a barely-passing client contributes a usable number of
// frames to the average rather than three.
const MEASURE_MS = 8_000;

const args = process.argv.slice(2);
const flag = (name, fallback = null) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const has = (name) => args.includes(name);

const VERBOSE = has('--verbose');
const KEEP_OPEN = has('--keep-open');
const ZERO_CANVAS = has('--zero-canvas');
const ZERO_CANVAS_MS = 3_000;
const ROOT = resolve(flag('--dir', 'build/web/game'));
const PAGE = 'fps_client.html';

const log = (...a) => console.log('[web-smoke]', ...a);
const vlog = (...a) => VERBOSE && console.log('[web-smoke]', ...a);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Fail on the requirement rather than on a confusing `WebSocket is not
// defined` twenty lines into launching a browser.
if (typeof WebSocket === 'undefined') {
    console.error(
        `[web-smoke] needs Node 22 or newer for the built-in WebSocket (this is ${process.version})`);
    process.exit(1);
}

// --- static file server ----------------------------------------------------

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.wasm': 'application/wasm',
    '.data': 'application/octet-stream',
    '.json': 'application/json',
};

// Serving the .wasm with the right Content-Type matters: without it Chrome
// refuses the streaming compile path and emscripten falls back with a console
// warning, which would be noise in a check that reads the console.
function startFileServer(root) {
    const server = createServer(async (req, res) => {
        const rel = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^([/\\])+/, '');
        const path = join(root, rel);
        // normalize() collapses `..`, but only comparing the result to the
        // root actually keeps the server inside it.
        if (path !== root && !path.startsWith(root + sep)) {
            res.writeHead(403).end();
            return;
        }
        try {
            const body = await readFile(path);
            res.writeHead(200, {
                'content-type': MIME[extname(path)] ?? 'application/octet-stream',
                'content-length': body.length,
            });
            res.end(body);
        } catch {
            vlog('404', rel);
            res.writeHead(404).end();
        }
    });
    return new Promise((ok) => {
        server.listen(0, '127.0.0.1', () => ok({ server, port: server.address().port }));
    });
}

// --- chrome ----------------------------------------------------------------

function findChrome() {
    const explicit = flag('--chrome') ?? process.env.CHROME_PATH;
    if (explicit) {
        if (!existsSync(explicit)) {
            throw new Error(`Chrome not found at '${explicit}'`);
        }
        return explicit;
    }
    const candidates =
        process.platform === 'darwin'
            ? [
                  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
                  '/Applications/Chromium.app/Contents/MacOS/Chromium',
              ]
            : [
                  '/usr/bin/google-chrome',
                  '/usr/bin/google-chrome-stable',
                  '/usr/bin/chromium-browser',
                  '/usr/bin/chromium',
              ];
    const found = candidates.find((p) => existsSync(p));
    if (!found) {
        throw new Error(
            `No Chrome found. Tried:\n  ${candidates.join('\n  ')}\n` +
                'Set CHROME_PATH or pass --chrome.');
    }
    return found;
}

async function launchChrome(profileDir) {
    const bin = findChrome();
    log(`chrome: ${bin}`);
    const child = spawn(
        bin,
        [
            '--headless=new',
            '--remote-debugging-port=0',
            `--user-data-dir=${profileDir}`,
            '--window-size=960,600',
            // CI runners have no GPU, so WebGL has to come from SwiftShader.
            // Recent Chrome refuses that for WebGL without the opt-in flag.
            '--enable-unsafe-swiftshader',
            '--use-gl=angle',
            '--use-angle=swiftshader',
            // requestAnimationFrame is throttled hard for anything the
            // compositor thinks is hidden, which would look exactly like the
            // performance cliff this check is here to catch.
            '--disable-background-timer-throttling',
            '--disable-backgrounding-occluded-windows',
            '--disable-renderer-backgrounding',
            // Throwaway profile, localhost only. Containers and CI images
            // routinely lack the kernel bits the sandbox needs.
            '--no-sandbox',
            '--no-first-run',
            '--disable-extensions',
        ],
        { stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout.on('data', (d) => vlog('chrome out:', String(d).trim()));
    child.stderr.on('data', (d) => vlog('chrome err:', String(d).trim()));

    // Port 0 means Chrome picks one and writes it into the profile.
    const portFile = join(profileDir, 'DevToolsActivePort');
    const deadline = Date.now() + 30_000;
    while (Date.now() < deadline) {
        if (child.exitCode !== null) {
            throw new Error(`Chrome exited with code ${child.exitCode} before it was ready`);
        }
        try {
            const [port] = (await readFile(portFile, 'utf8')).split('\n');
            if (port) {
                return { child, port: Number(port) };
            }
        } catch {
            // not written yet
        }
        await sleep(100);
    }
    throw new Error('Chrome never wrote DevToolsActivePort');
}

// --- devtools protocol -----------------------------------------------------

class Cdp {
    #ws;
    #next = 1;
    #pending = new Map();
    handlers = [];

    static async connect(url) {
        const cdp = new Cdp();
        cdp.#ws = new WebSocket(url);
        await new Promise((ok, fail) => {
            cdp.#ws.addEventListener('open', ok, { once: true });
            cdp.#ws.addEventListener('error', () => fail(new Error(`cannot connect to ${url}`)), {
                once: true,
            });
        });
        cdp.#ws.addEventListener('message', (event) => cdp.#dispatch(JSON.parse(event.data)));
        return cdp;
    }

    #dispatch(msg) {
        if (msg.id !== undefined) {
            const entry = this.#pending.get(msg.id);
            this.#pending.delete(msg.id);
            if (!entry) return;
            msg.error ? entry.fail(new Error(`${msg.error.message} (${entry.method})`))
                      : entry.ok(msg.result);
            return;
        }
        for (const handler of this.handlers) {
            handler(msg);
        }
    }

    send(method, params = {}, sessionId) {
        const id = this.#next++;
        return new Promise((ok, fail) => {
            this.#pending.set(id, { ok, fail, method });
            this.#ws.send(JSON.stringify({ id, method, params, sessionId }));
        });
    }

    close() {
        this.#ws.close();
    }
}

// --- the check -------------------------------------------------------------

// Reaches into the module the same way a human would in the console.
const READ_SMOKE = `(() => {
  try {
    return (typeof Module !== 'undefined' && Module.fpsSmoke) ? Module.fpsSmoke : null;
  } catch (e) { return null; }
})()`;

// Emscripten flushes stderr a line at a time, so one multi-line log message
// arrives as several console.error calls and only the first carries the level
// tag. The continuation lines are the useful part of a shader compile
// failure, so they get folded back into their parent here.
const TAGGED = /] \[(TRACE|DEBUG|INFO |WARN |ERROR)]/;

function collectErrors(consoleLines) {
    const blocks = [];
    for (let i = 0; i < consoleLines.length; ++i) {
        if (!consoleLines[i].text.includes('] [ERROR]')) continue;
        const block = [consoleLines[i].text];
        for (let j = i + 1; j < consoleLines.length && !TAGGED.test(consoleLines[j].text); ++j) {
            block.push(consoleLines[j].text);
        }
        blocks.push(block.join('\n      '));
    }
    return blocks;
}

function report(failures, consoleLines, exceptions) {
    const all = [...failures];
    if (exceptions.length > 0) {
        all.push(`uncaught exception in the page:\n    ${exceptions.join('\n    ')}`);
    }
    // Our own errors are tagged by the log formatter, so this does not trip on
    // Chrome's routine chatter or on log::warn.
    const errors = collectErrors(consoleLines);
    if (errors.length > 0) {
        all.push(`the client logged ${errors.length} error(s):\n    ` + errors.join('\n    '));
    }
    if (all.length === 0) {
        log('PASSED');
        return 0;
    }
    console.error('\n[web-smoke] FAILED');
    for (const f of all) {
        console.error(`  - ${f}`);
    }
    return 1;
}

// Confirms the build directory holds a WebAssembly client and not a native
// binary that happens to share its name.
//
// This is the cheap half of the check and it exists because of a real, silent
// failure: the `web` preset had no toolchain file, so `cmake --preset web`
// produced an ordinary native executable. Every "the web build is clean"
// claim in that period was vacuous, and nothing anywhere noticed. The preset
// now hard-errors without EMSDK, but asserting the output shape is what
// actually makes the guarantee, so it is asserted rather than assumed.
async function checkArtifacts(root) {
    const listing = existsSync(root) ? await readdir(root) : null;
    const hint = 'Build it first:  emcmake cmake --preset web && cmake --build --preset web';
    if (listing === null) {
        throw new Error(`${root} does not exist.\n${hint}`);
    }

    const required = ['fps_client.html', 'fps_client.js', 'fps_client.wasm', 'fps_client.data'];
    const missing = required.filter((f) => !listing.includes(f));
    if (missing.length > 0) {
        const native = listing.includes('fps_client') ? '\nThere IS a `fps_client` with no ' +
                'extension here, which is what a NATIVE build leaves behind -- the Emscripten ' +
                'toolchain file was probably not used.'
                                                      : '';
        throw new Error(
            `${root} is missing ${missing.join(', ')}.\nContents: ${listing.join(', ')}` +
            `${native}\n${hint}`);
    }

    // \0asm, then a little-endian u32 version.
    const head = await readFile(join(root, 'fps_client.wasm'));
    const magic = head.subarray(0, 4);
    if (!(magic[0] === 0x00 && magic[1] === 0x61 && magic[2] === 0x73 && magic[3] === 0x6d)) {
        throw new Error(
            `fps_client.wasm is not a WebAssembly module (starts with ` +
            `${[...magic].map((b) => b.toString(16).padStart(2, '0')).join(' ')}, ` +
            'expected 00 61 73 6d)');
    }
    const version = head.readUInt32LE(4);
    if (version !== 1) {
        throw new Error(`fps_client.wasm declares WebAssembly version ${version}, expected 1`);
    }
    log(`artifacts: wasm v${version}, ${(head.length / 1e6).toFixed(1)} MB`);
}

async function main() {
    await checkArtifacts(ROOT);

    const { server, port } = await startFileServer(ROOT);
    const profileDir = await mkdtemp(join(tmpdir(), 'fps-web-smoke-'));
    let chrome;
    let cdp;
    const cleanup = async () => {
        cdp?.close();
        chrome?.child.kill();
        server.close();
        await rm(profileDir, { recursive: true, force: true }).catch(() => {});
    };

    try {
        chrome = await launchChrome(profileDir);
        const version = await (await fetch(`http://127.0.0.1:${chrome.port}/json/version`)).json();
        cdp = await Cdp.connect(version.webSocketDebuggerUrl);

        const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });

        // Everything the page says, plus everything the browser says about the
        // page. Shader compile failures surface on both: our own log::error
        // text via printErr, and Chrome's own WebGL diagnostics via Log.
        const consoleLines = [];
        const exceptions = [];
        cdp.handlers.push((msg) => {
            if (msg.sessionId !== sessionId) return;
            if (msg.method === 'Runtime.consoleAPICalled') {
                const text = msg.params.args
                    .map((a) => a.value ?? a.description ?? a.unserializableValue ?? '')
                    .join(' ');
                consoleLines.push({ level: msg.params.type, text });
                vlog(`console.${msg.params.type}:`, text);
            } else if (msg.method === 'Log.entryAdded') {
                const { level, text, source } = msg.params.entry;
                consoleLines.push({ level, text: `[${source}] ${text}` });
                vlog(`log.${level}:`, text);
            } else if (msg.method === 'Runtime.exceptionThrown') {
                const d = msg.params.exceptionDetails;
                const text = d.exception?.description ?? d.text;
                exceptions.push(text);
                vlog('exception:', text);
            }
        });
        await cdp.send('Runtime.enable', {}, sessionId);
        await cdp.send('Log.enable', {}, sessionId);
        await cdp.send('Page.enable', {}, sessionId);

        // Regression test for the startup race, which a plain headless load
        // does not reproduce: with a fixed window size Chrome has laid the
        // canvas out long before the module boots, so the client never sees
        // the 0-height canvas that broke it in a real browser.
        //
        // The starve is lifted from out here, once the client has been seen
        // running at zero height, rather than on a timer inside the page. A
        // timer would be a coin flip -- on a slow runner the module can boot
        // after it fires, and the check would quietly degrade into the
        // ordinary one while still reporting a pass.
        if (ZERO_CANVAS) {
            log('holding the canvas at zero height (startup-race regression test)');
            await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
                source: `document.addEventListener('DOMContentLoaded', () => {
                    const s = document.createElement('style');
                    s.id = 'fps-smoke-starve';
                    s.textContent =
                        '#wrap { height: 0 !important; } #canvas { height: 0 !important; }';
                    document.head.appendChild(s);
                });`,
            }, sessionId);
        }

        const url = `http://127.0.0.1:${port}/${PAGE}`;
        log(`loading ${url}`);
        await cdp.send('Page.navigate', { url }, sessionId);

        const read = async () => {
            const { result } = await cdp.send(
                'Runtime.evaluate', { expression: READ_SMOKE, returnByValue: true }, sessionId);
            return result.value ?? null;
        };

        // Step 1: the module has to come up and produce the fingerprint, which
        // is published on frame 30. A client that dies during startup -- the
        // 0x0-canvas failure -- never gets here.
        const fatal = () =>
            exceptions.length > 0 || consoleLines.some((l) => l.text.includes('] [ERROR]'));

        const waitFor = async (predicate, timeoutMs) => {
            const until = Date.now() + timeoutMs;
            while (Date.now() < until) {
                // A client that has already logged an error is not going to
                // recover; waiting out the full timeout just to say so turns
                // a clear failure into a slow one.
                if (fatal()) return null;
                const value = await read();
                if (predicate(value)) return value;
                await sleep(250);
            }
            return null;
        };

        let starved = null;
        if (ZERO_CANVAS) {
            // The client must survive booting against a 0-height canvas. It
            // cannot render anything useful yet, so all that is required here
            // is that it reached its frame loop at all.
            starved = await waitFor((v) => v && v.frames >= 1, STARTUP_TIMEOUT_MS);
            if (!starved) {
                return report(
                    ['the client did not reach its frame loop with a 0-height canvas -- ' +
                     'a degenerate drawable size must be survived, not rejected'],
                    consoleLines, exceptions);
            }
            log(`survived the zero-height boot (frame ${starved.frames}); restoring the canvas`);
            await cdp.send(
                'Runtime.evaluate', {
                    expression: `(() => {
                        document.getElementById('fps-smoke-starve')?.remove();
                        // Restoring the layout is not enough on its own: SDL
                        // re-reads the canvas size from a window resize
                        // event, and changing an element's computed style does
                        // not raise one. In a real browser the size arrives
                        // with a resize; here it has to be asked for.
                        window.dispatchEvent(new Event('resize'));
                    })()`,
                },
                sessionId);
        }

        // The fingerprint is published on the first frame at or after 30 with
        // a real drawable, so waiting for it covers both "did it start" and
        // "is it still going".
        const smoke = await waitFor((v) => v && v.distinctColors > 0, STARTUP_TIMEOUT_MS);

        const failures = [];

        if (!smoke) {
            const last = await read();
            failures.push(
                last ? `reached frame ${last.frames} but never produced a fingerprint -- ` +
                           'the frame loop stalled after starting'
                     : fatal() ? 'the client failed during startup and never rendered a frame'
                               : `no frame rendered within ${STARTUP_TIMEOUT_MS / 1000}s -- ` +
                                     'the client did not reach its frame loop');
        } else {
            log(`started: frames=${smoke.frames} elapsed=${smoke.seconds.toFixed(1)}s`);
            log(`pixels:  ${smoke.distinctColors} distinct colours, ` +
                `mean luma ${smoke.meanLuma.toFixed(3)}`);

            if (smoke.distinctColors < MIN_DISTINCT_COLORS) {
                failures.push(
                    `the frame has ${smoke.distinctColors} distinct colours ` +
                    `(need >= ${MIN_DISTINCT_COLORS}) -- the renderer produced a flat image, ` +
                    'which is what a failed shader compile looks like');
            }

            // Step 2: frame rate, measured with the client's own monotonic
            // clock so a busy CI host cannot skew it the way a JS timer would.
            log(`measuring frame rate for ${MEASURE_MS / 1000}s...`);
            const before = await read();
            await sleep(MEASURE_MS);
            const after = await read();
            const dFrames = after.frames - before.frames;
            const dSeconds = after.seconds - before.seconds;
            const fps = dSeconds > 0 ? dFrames / dSeconds : 0;
            log(`fps:     ${fps.toFixed(1)} (${dFrames} frames in ${dSeconds.toFixed(1)}s)`);
            if (fps < MIN_FPS) {
                failures.push(
                    `${fps.toFixed(2)} fps (need >= ${MIN_FPS}) -- this threshold is set low ` +
                    'enough that only a pathological regression trips it');
            }
        }

        if (KEEP_OPEN) {
            log('--keep-open: leaving the browser running, ^C to stop');
            await new Promise(() => {});
        }

        return report(failures, consoleLines, exceptions);
    } finally {
        await cleanup();
    }
}

process.exitCode = await main().catch((err) => {
    console.error(`[web-smoke] ${err.message}`);
    return 1;
});
