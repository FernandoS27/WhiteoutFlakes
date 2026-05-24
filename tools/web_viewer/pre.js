// pre.js — prepended into the generated wf-core.js by Emscripten (--pre-js).
//
// Web Crypto's `crypto.getRandomValues` caps requests at 65,536 bytes per
// spec, but Emscripten's libc startup asks for ~294 KB in a single call
// during runtime init (a thread/stack guard seed buffer). Without this
// patch the module throws QuotaExceededError from `initRandomFill`
// before any user code runs. Chunk the call here, once, at module-script
// evaluation time — before any of Emscripten's own JS can capture the
// original `crypto.getRandomValues` into a hot path.
(function () {
  if (typeof crypto === 'undefined' || crypto.__wfChunked) return;
  const MAX = 65536;
  const native = crypto.getRandomValues.bind(crypto);
  crypto.getRandomValues = function (buf) {
    if (!buf || buf.byteLength <= MAX) return native(buf);
    const u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    for (let off = 0; off < u8.length; off += MAX) {
      native(u8.subarray(off, Math.min(off + MAX, u8.length)));
    }
    return buf;
  };
  crypto.__wfChunked = true;
})();
