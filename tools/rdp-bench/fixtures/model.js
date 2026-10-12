/* Pure deterministic workload/marker primitives. No DOM, timers, or Math.random. */
(function(root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.RdpBenchModel = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';
  function mix32(x) {
    x = (x ^ (x >>> 16)) >>> 0; x = Math.imul(x, 0x7feb352d) >>> 0;
    x = (x ^ (x >>> 15)) >>> 0; x = Math.imul(x, 0x846ca68b) >>> 0;
    return (x ^ (x >>> 16)) >>> 0;
  }
  function randomAt(seed, tick, item) {
    return mix32((seed >>> 0) ^ Math.imul(tick >>> 0, 0x9e3779b1) ^ Math.imul(item >>> 0, 0x85ebca6b)) / 4294967296;
  }
  function tickAt(elapsedMs, fps) {
    if (!Number.isFinite(elapsedMs) || elapsedMs < 0 || !Number.isFinite(fps) || fps <= 0)
      throw new Error('Invalid time or fps');
    return Math.floor(elapsedMs * fps / 1000);
  }
  function crc16(bytes) {
    let crc = 0xffff;
    for (const byte of bytes) {
      crc ^= byte << 8;
      for (let j = 0; j < 8; j++) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
    }
    return crc;
  }
  function markerBytes(runTag, phaseId, tick, probeId) {
    for (const n of [runTag, phaseId, tick, probeId])
      if (!Number.isSafeInteger(n) || n < 0) throw new Error('Marker values must be nonnegative safe integers');
    if (runTag > 65535 || phaseId > 65535 || tick > 0xffffffff || probeId > 0xffffffff)
      throw new Error('Marker value out of range');
    const b = new Uint8Array(16), v = new DataView(b.buffer);
    v.setUint16(0, 0xb17e); v.setUint16(2, runTag); v.setUint16(4, phaseId);
    v.setUint32(6, tick); v.setUint32(10, probeId); v.setUint16(14, crc16(b.subarray(0,14)));
    return b;
  }
  function decodeMarker(b) {
    if (!(b instanceof Uint8Array) || b.length !== 16) return null;
    const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (v.getUint16(0) !== 0xb17e || v.getUint16(14) !== crc16(b.subarray(0,14))) return null;
    return {runTag:v.getUint16(2), phaseId:v.getUint16(4), tick:v.getUint32(6), probeId:v.getUint32(10)};
  }
  return {mix32, randomAt, tickAt, crc16, markerBytes, decodeMarker};
});
