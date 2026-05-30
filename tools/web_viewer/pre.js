// --pre-js into wf-core.js. Web Crypto caps getRandomValues at 64 KiB
// but Emscripten libc init asks for ~294 KiB; chunk before any hot path
// captures the original reference.
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
