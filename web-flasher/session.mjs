import { assertDevice, buildPlan, confirmPlan, parseTable, TABLE_OFFSET, SECTOR, number, samePartition, SYSTEM_TARGETS } from './core.mjs';
import { md5, sha256 } from './hashes.mjs';
import { bounded, SerialPortLease } from './serial-port.mjs';

const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
/** Owns the serial port; all serial operations are mutually exclusive. */
export class FlashSession {
  constructor(loadSdk, log = () => {}, options = {}) {
    this.loadSdk = loadSdk; this.log = log;
    this.device = null; this.transport = null; this.loader = null;
    this.port = null; this.busy = false; this.lost = false;
    this.onStage = options.onStage || (() => {});
    this.timeouts = { sdk: 15000, handshake: 35000, security: 8000, flash: 12000, table: 15000, close: 3000, ...options.timeouts };
    this.lease = null; this.connecting = null;
  }
  async exclusive(work) {
    if (this.busy) throw new Error('设备忙，请等待当前操作完成');
    this.busy = true;
    try { return await work(); } finally { this.busy = false; }
  }
  async close() {
    const transport = this.transport, lease = this.lease;
    this.device = null; this.transport = null; this.loader = null; this.port = null; this.lease = null;
    lease?.stop();
    // Do not await the SDK's unbounded waitForUnlock loop. Our lease owns the
    // native handles and closes them even when an SDK write rejected early.
    const closing = [];
    if (lease) closing.push(lease.dispose());
    if (transport) closing.push(bounded(() => transport.disconnect(), this.timeouts.close, '释放串口'));
    await Promise.all(closing);
  }
  cancelConnect() {
    if (!this.connecting) return false; // Never expose cancellation during writes.
    const error = new Error('连接已取消，未写入 Flash');
    this.connecting.abort(error); this.lease?.stop(error); return true;
  }
  markLost(port) {
    if (port === this.port) {
      this.lost = true; this.device = null;
      const error = new Error('USB 已断开；若复位后重新枚举，请重新选择串口');
      this.connecting?.abort(error); this.lease?.stop(error);
    }
  }
  async connect(port, baudrate = 115200, reset = 'default_reset') {
    return this.exclusive(async () => {
      if (this.transport) throw new Error('请先断开现有设备');
      if (![115200, 460800, 921600].includes(baudrate) || !['default_reset', 'no_reset'].includes(reset)) throw new Error('连接参数无效');
      this.port = port; this.lost = false;
      const controller = new AbortController(); this.connecting = controller;
      const stage = async (name, timeout, work) => {
        controller.signal.throwIfAborted(); this.onStage(name); this.log(`连接阶段：${name}`);
        const value = await bounded(work, timeout, name, controller.signal);
        controller.signal.throwIfAborted(); return value;
      };
      try {
        const { ESPLoader, Transport } = await stage('加载刷机库', this.timeouts.sdk, () => this.loadSdk());
        this.lease = new SerialPortLease(port, { closeTimeout: this.timeouts.close, onStage: this.onStage });
        this.transport = new Transport(this.lease, false);
        this.transport.setDeviceLostCallback?.(() => this.markLost(port));
        const info = port.getInfo?.() || {};
        // Keep native USB-Serial/JTAG open rather than provoking a close/reopen
        // and DTR/RTS transition just to change a USB line-coding baud value.
        const nativeUsb = info.usbVendorId === 0x303a && info.usbProductId === 0x1001;
        const effectiveBaud = nativeUsb ? 115200 : baudrate;
        if (nativeUsb && baudrate !== effectiveBaud) this.log('原生 USB 使用 115200，避免切换波特率时重新打开端口');
        const terminal = text => {
          if (controller.signal.aborted) return;
          this.log(text);
          const value = String(text);
          if (/Detecting chip|Chip is/.test(value)) this.onStage('识别芯片');
          else if (/Uploading stub|Running stub/.test(value)) this.onStage('加载下载程序');
          else if (/Connecting/.test(value)) this.onStage('进入下载模式');
        };
        this.loader = new ESPLoader({ transport: this.transport, baudrate: effectiveBaud, romBaudrate: 115200,
          debugLogging: false, terminal: { clean() {}, write: terminal, writeLine: terminal } });
        const loader = this.loader; // Never let a late completion touch a newer connection.
        await stage('下载模式握手', this.timeouts.handshake, () => loader.main(reset));
        const security = await stage('检查安全状态', this.timeouts.security, () => loader.getSecurityInfo());
        const detected = await stage('读取 Flash 容量', this.timeouts.flash, () => loader.detectFlashSize());
        const match = /^(\d+)(MB|KB)$/.exec(detected || '');
        const flashSize = match ? Number(match[1]) * (match[2] === 'MB' ? 1048576 : 1024) : 0;
        const device = { chip: loader.chip?.CHIP_NAME, flashSize, security, partitions: null, tableBytes: null, tableError: '' };
        assertDevice(device);
        const bytes = await stage('读取分区表', this.timeouts.table, () => loader.readFlash(TABLE_OFFSET, SECTOR));
        if (!(bytes instanceof Uint8Array) || bytes.length !== SECTOR) throw new Error('设备分区表读取不完整');
        device.tableBytes = bytes.slice();
        try { device.partitions = parseTable(bytes, flashSize); }
        catch (error) { device.tableError = error.message; }
        controller.signal.throwIfAborted();
        if (this.lost) throw new Error('设备已断开');
        this.device = device; return device;
      } catch (error) {
        controller.abort(error); this.lease?.stop(error);
        try { await this.close(); }
        catch { this.log('串口尚未释放：请拔插 USB 后重试。'); }
        throw error;
      } finally { this.connecting = null; }
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
