// node_compile_test.mjs — diagnose the WebAssembly.compile hang in Node.
//
// Run with:   node tools/web_viewer/node_compile_test.mjs
// (Use the Node bundled with Emscripten: C:\Projects\emsdk\node\22.16.0_64bit\bin\node.exe
//  so we're on the same V8 family Edge uses.)
//
// Reports: file size, validation result, sync compile time (via
// `new WebAssembly.Module(bytes)`), and async compile time. Uses a hard
// timeout so a hang becomes "timed out at N s" instead of an infinite stall.

import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = resolve(here, '..', '..', 'build-web', 'web', 'wf-core.wasm');

const t0 = performance.now();
const ms = () => (performance.now() - t0).toFixed(0) + ' ms';

console.log(`Node ${process.version} — V8 ${process.versions.v8}`);
console.log(`Reading ${wasmPath} …`);
const buf = await readFile(wasmPath);
console.log(`File size: ${(buf.length / 1024).toFixed(1)} KB @${ms()}`);

console.log(`WebAssembly.validate …`);
const valid = WebAssembly.validate(buf);
console.log(`validate=${valid} @${ms()}`);
if (!valid) process.exit(1);

// Synchronous compile via `new WebAssembly.Module(bytes)`. Node allows
// this without size limits (unlike browsers which require user gesture
// for sync compile over 4 KB). If THIS hangs, V8 is hung — not a browser
// async-compile heuristic issue.
console.log(`Trying SYNCHRONOUS compile via new WebAssembly.Module(...) …`);
const syncStart = performance.now();
try {
    const mod = new WebAssembly.Module(buf);
    const took = (performance.now() - syncStart).toFixed(0);
    console.log(`SYNC compile OK in ${took} ms @${ms()}; exports: ${WebAssembly.Module.exports(mod).length}`);
} catch (e) {
    console.log(`SYNC compile FAILED: ${e.message}`);
}

// Async compile with a timeout race.
console.log(`Trying ASYNC compile via WebAssembly.compile(...) with 30 s timeout …`);
const asyncStart = performance.now();
const compilePromise = WebAssembly.compile(buf);
const timeoutPromise = new Promise((_, reject) =>
    setTimeout(() => reject(new Error('compile timed out at 30 s')), 30000));
try {
    const mod = await Promise.race([compilePromise, timeoutPromise]);
    const took = (performance.now() - asyncStart).toFixed(0);
    console.log(`ASYNC compile OK in ${took} ms @${ms()}; exports: ${WebAssembly.Module.exports(mod).length}`);
} catch (e) {
    console.log(`ASYNC compile FAILED: ${e.message}`);
}
