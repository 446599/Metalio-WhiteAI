import { assertDevice, buildPlan, confirmPlan, parseTable, TABLE_OFFSET, SECTOR, number, samePartition, SYSTEM_TARGETS } from './core.mjs';
import { md5, sha256 } from './hashes.mjs';

const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
/** Owns the serial port; all serial operations are mutually exclusive. */
export class FlashSession {
  constructor(loadSdk, log = () => {}) {
    this.loadSdk = loadSdk; this.log = log;
    this.device = null; this.transport = null; this.loader = null;
    this.port = null; this.busy = false; this.lost = false;
  }
  async exclusive(work) {
    if (this.busy) throw new Error('设备忙，请等待当前操作完成');
    this.busy = true;
    try { return await work(); } finally { this.busy = false; }
  }
  async close() {
    const transport = this.transport;
    this.device = null; this.transport = null; this.loader = null; this.port = null;
    if (transport) await transport.disconnect();
  }
  markLost(port) {
    if (port === this.port) { this.lost = true; this.device = null; }
  }
  async connect(port, baudrate = 115200, reset = 'default_reset') {
    return this.exclusive(async () => {
      if (this.transport) throw new Error('请先断开现有设备');
      if (![115200, 460800, 921600].includes(baudrate) || !['default_reset', 'no_reset'].includes(reset)) throw new Error('连接参数无效');
      this.port = port; this.lost = false;
      try {
        const { ESPLoader, Transport } = await this.loadSdk();
        if (this.lost) throw new Error('设备已断开');
        this.transport = new Transport(port, false);
        this.loader = new ESPLoader({ transport: this.transport, baudrate, romBaudrate: 115200,
          debugLogging: false, terminal: { clean() {}, write: s => this.log(s), writeLine: s => this.log(s) } });
        await this.loader.main(reset);
        const security = await this.loader.getSecurityInfo();
        const detected = await this.loader.detectFlashSize();
        const match = /^(\d+)(MB|KB)$/.exec(detected || '');
        const flashSize = match ? Number(match[1]) * (match[2] === 'MB' ? 1048576 : 1024) : 0;
        const device = { chip: this.loader.chip?.CHIP_NAME, flashSize, security, partitions: null, tableBytes: null, tableError: '' };
        assertDevice(device);
        // A failed read is NOT equivalent to a blank table. Do not enable writes.
        const bytes = await this.loader.readFlash(TABLE_OFFSET, SECTOR);
        if (!(bytes instanceof Uint8Array) || bytes.length !== SECTOR) throw new Error('设备分区表读取不完整');
        device.tableBytes = bytes.slice();
        try { device.partitions = parseTable(bytes, flashSize); }
        catch (error) { device.tableError = error.message; }
        if (this.lost) throw new Error('设备已断开');
        this.device = device;
        return device;
      } catch (error) {
        try { await this.close(); } catch { /* Preserve the original connection error. */ }
        throw error;
      }
    });
  }
  async disconnect() { return this.exclusive(() => this.close()); }
  async backup(target, progress = () => {}) {
    return this.exclusive(async () => {
      const device = this.device; assertDevice(device);
      const candidates = [...SYSTEM_TARGETS, ...(device.partitions || [])];
      if (!candidates.some(p => samePartition(p, target))) throw new Error('备份范围与设备实际分区表不一致');
      if (target.offset + target.size > device.flashSize || target.size > 16 * 1048576) throw new Error('备份范围过大');
      const data = new Uint8Array(target.size);
      for (let offset = 0; offset < target.size; offset += 16384) {
        const size = Math.min(16384, target.size - offset);
        const chunk = await this.loader.readFlash(target.offset + offset, size);
        if (this.lost || !(chunk instanceof Uint8Array) || chunk.length !== size) throw new Error('备份中断，未输出不完整文件');
        data.set(chunk, offset); progress(offset + size, target.size);
      }
      return data;
    });
  }
  /** Re-read table immediately before first write to reject stale preflight. */
  async flash(rows, layout, acknowledged, riskText, progress = () => {}) {
    return this.exclusive(async () => {
      const device = this.device; assertDevice(device);
      const plan = buildPlan(rows, layout, device); confirmPlan(plan, acknowledged, riskText);
      const current = await this.loader.readFlash(TABLE_OFFSET, SECTOR);
      if (this.lost || !(current instanceof Uint8Array) || !equal(current, device.tableBytes)) throw new Error('设备分区表在确认后改变，请重新连接并核对');
      // Data first, boot metadata last. No reset until all writes verify.
      const priority = p => p.offset === TABLE_OFFSET ? 2 : p.type === 1 && p.subtype === 0 ? 3 : p.offset === 0 ? 1 : 0;
      const files = [...plan.files].sort((a, b) => priority(a) - priority(b) || a.offset - b.offset);
      const loader = this.loader;
      try {
        await loader.writeFlash({ fileArray: files.map(p => ({ address: p.offset, data: p.data })),
          flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep', eraseAll: false, compress: true,
          calculateMD5Hash: md5,
          reportProgress: (i, written, total) => {
            const before = files.slice(0, i).reduce((sum, p) => sum + p.data.length, 0);
            // The SDK reports compressed bytes. Weight each file by original size.
            const fraction = total > 0 ? Math.min(1, written / total) : 0;
            progress(Math.min(99, (before + files[i].data.length * fraction) / plan.total * 100), files[i].name);
          } });
        if (this.lost) throw new Error('设备断开，不能确认完整写入');
        // Verify exact unpadded bytes again; SDK also verifies its padded image.
        for (const p of files) {
          const actual = await loader.flashMd5sum(p.offset, p.data.length);
          if (this.lost || actual.toLowerCase() !== md5(p.data)) throw new Error(`${p.name} 写后 MD5 校验失败`);
        }
        if (plan.tableWrite) {
          const bytes = await loader.readFlash(TABLE_OFFSET, SECTOR);
          const parts = parseTable(bytes, device.flashSize);
          device.tableBytes = bytes.slice(); device.partitions = parts; device.tableError = '';
        }
        progress(100, '全部文件已校验');
        return plan;
      } catch (error) {
        // A partially flashed device must be reconnected and re-read.
        try { await this.close(); } catch { /* Keep original write failure. */ }
        throw error;
      }
    });
  }
  async restart() {
    return this.exclusive(async () => {
      assertDevice(this.device);
      try { await this.loader.after('hard_reset'); }
      finally { await this.close(); }
    });
  }
}
/** Read only explicitly selected local files. Files are never uploaded. */
export async function loadRows(rows) {
  const selected = rows.filter(row => row.selected);
  if (!selected.length) throw new Error('请勾选至少一个分区');
  if (selected.length > 32 || selected.reduce((sum, row) => sum + (row.file?.size || 0), 0) > 16 * 1048576) throw new Error('单次选择的文件总量不能超过 16 MiB');
  const result = [];
  for (const row of selected) {
    if (!row.file || row.file.size === 0) throw new Error(`${row.name} 尚未选择文件或文件为空`);
    if (row.file.size > number(row.size) || row.file.size > 16 * 1048576) throw new Error(`${row.name} 文件超过分区容量`);
    const data = new Uint8Array(await row.file.arrayBuffer());
    result.push({ ...row, file: undefined, fileName: row.file.name, data, sha256: await sha256(data) });
  }
  return result;
}
