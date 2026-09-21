// MD5 is used only for ESP ROM transfer integrity, never for authentication.
// Byte-oriented implementation; tested against RFC vectors and node:crypto.
const shifts = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const constants = Uint32Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32));
export function hex(bytes) {
  return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
}
export function md5(bytes) {
  if (!(bytes instanceof Uint8Array)) throw new TypeError('MD5 expects Uint8Array');
  const padded = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64);
  padded.set(bytes);
  padded[bytes.length] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (bytes.length * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(bytes.length / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (let offset = 0; offset < padded.length; offset += 64) {
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      const group = Math.floor(i / 16);
      const f = group === 0 ? (b & c) | (~b & d) : group === 1 ? (d & b) | (~d & c) : group === 2 ? b ^ c ^ d : c ^ (b | ~d);
      const word = group === 0 ? i : group === 1 ? (5 * i + 1) % 16 : group === 2 ? (3 * i + 5) % 16 : (7 * i) % 16;
      const sum = (a + f + constants[i] + view.getUint32(offset + word * 4, true)) >>> 0;
      const shift = shifts[group * 4 + i % 4];
      a = d; d = c; c = b;
      b = (b + ((sum << shift) | (sum >>> (32 - shift)))) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0;
    c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  const out = new Uint8Array(16), result = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((word, i) => result.setUint32(i * 4, word, true));
  return hex(out);
}
export async function sha256(bytes) {
  return hex(new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes)));
}
