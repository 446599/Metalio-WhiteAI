import { md5 } from '../hashes.mjs';
import { parseCsv, DEFAULT_CSV, TABLE_OFFSET, FLASH_SIZE, SECTOR } from '../core.mjs';
export const layout = parseCsv(DEFAULT_CSV);
export function tableBytes(parts = layout) {
  const bytes = new Uint8Array(SECTOR).fill(255), view = new DataView(bytes.buffer);
  parts.forEach((p, i) => {
    const off = i * 32; bytes.fill(0, off, off + 32);
    view.setUint16(off, 0x50aa, true); bytes[off + 2] = p.type; bytes[off + 3] = p.subtype;
    view.setUint32(off + 4, p.offset, true); view.setUint32(off + 8, p.size, true);
    bytes.set(new TextEncoder().encode(p.name), off + 12); view.setUint32(off + 28, p.flags, true);
  });
  const end = parts.length * 32; bytes[end] = 0xeb; bytes[end + 1] = 0xeb;
  bytes.set(Uint8Array.from(md5(bytes.subarray(0, end)).match(/../g), s => parseInt(s, 16)), end + 16);
  return bytes;
}
export function image(length = 128) {
  const bytes = new Uint8Array(length), view = new DataView(bytes.buffer);
  bytes[0] = 0xe9; bytes[1] = 1; bytes[12] = 9;
  view.setUint32(24, 0x3fc88000, true); view.setUint32(28, Math.max(0, length - 48), true);
  let checksum = 0xef;
  for (let i = 32; i < length - 16; i++) { bytes[i] = i % 251; checksum ^= bytes[i]; }
  bytes[length - 1] = checksum;
  return bytes;
}
export function device() { return { chip: 'ESP32-S3', flashSize: FLASH_SIZE, security: { flags: 0, flashCryptCnt: 0 }, partitions: layout, tableBytes: tableBytes() }; }
export function row(name = 'ota_0', data = image()) { return { ...layout.find(p => p.name === name), fileName: `${name}.bin`, data }; }
export function mockSdk(options = {}) {
  const calls = { writes: [], reads: [], closes: 0, resets: [], options: null };
  const memory = new Uint8Array(FLASH_SIZE).fill(255); memory.set(options.table || tableBytes(), TABLE_OFFSET);
  const sdk = {
    Transport: class { constructor(port) { this.port = port; } async disconnect() { calls.closes++; } },
    ESPLoader: class {
      constructor(opts) { calls.options = opts; this.chip = { CHIP_NAME: options.chip || 'ESP32-S3' }; }
      async main(mode) { calls.resets.push(mode); if (options.connectError) throw new Error('connect failed'); }
      async getSecurityInfo() { if (options.securityError) throw new Error('security failed'); return options.security || { flags: 0, flashCryptCnt: 0 }; }
      async detectFlashSize() { return Object.hasOwn(options, 'flashSize') ? options.flashSize : '16MB'; }
      async readFlash(address, size) {
        calls.reads.push([address, size]);
        if (options.onRead) await options.onRead(address, size, calls.reads.length);
        if (options.readError) throw new Error('read failed');
        return memory.slice(address, address + size - (options.shortRead ? 1 : 0));
      }
      async writeFlash(opts) {
        calls.writes.push(opts);
        if (options.onWrite) await options.onWrite();
        if (options.writeError) throw new Error('write failed');
        for (const [i, p] of opts.fileArray.entries()) {
          memory.set(p.data, p.address); opts.reportProgress?.(i, 1, 2); opts.reportProgress?.(i, 2, 2);
          if (opts.calculateMD5Hash(p.data) !== md5(p.data)) throw new Error('callback hash failed');
        }
      }
      async flashMd5sum(address, size) { return options.corrupt ? '00000000000000000000000000000000' : md5(memory.subarray(address, address + size)); }
      async after(mode) { calls.resets.push(mode); }
    },
  };
  return { sdk, memory, calls };
}
