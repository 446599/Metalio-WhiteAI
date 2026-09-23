/** Bounded Web Serial ownership for the pinned esptool-js adapter.
 * Never retry an unfinished native open/close on the same port. Cancelling a
 * Promise alone is not cancellation of USB I/O: revoke the lease and release
 * its readers/writers before allowing another connection.
 */
const owners = new WeakMap();
const reason = signal => signal?.reason instanceof Error ? signal.reason : new Error('连接已取消');
export function bounded(work, ms, label, signal) {
  if (signal?.aborted) return Promise.reject(reason(signal));
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (fn, value) => {
      if (settled) return;
      settled = true; clearTimeout(timer); signal?.removeEventListener('abort', abort); fn(value);
    };
    const abort = () => finish(reject, reason(signal));
    const timer = ms > 0 ? setTimeout(() => finish(reject, new Error(`${label}超时，请重新连接`)), ms) : null;
    signal?.addEventListener('abort', abort, { once: true });
    try { Promise.resolve(work()).then(v => finish(resolve, v), e => finish(reject, e)); }
    catch (error) { finish(reject, error); }
  });
}

export class SerialPortLease {
  constructor(port, { ioTimeout = 10000, closeTimeout = 2500, onStage = () => {} } = {}) {
    if (owners.has(port)) throw new Error('上次串口尚未释放，请拔插 USB 后重新选择设备');
    owners.set(port, this);
    this.raw = port; this.ioTimeout = ioTimeout; this.closeTimeout = closeTimeout; this.onStage = onStage;
    this.controller = new AbortController(); this.readers = new Set(); this.writers = new Set();
    this.operations = new Set(); this.pendingOpen = null; this.closing = null; this.opened = false; this.nativeClose = null;
  }
  check() { if (this.controller.signal.aborted) throw reason(this.controller.signal); }
  getInfo() { return this.raw.getInfo?.() || {}; }
  async open(options) {
    this.check(); this.onStage('打开串口'); this.nativeClose = null;
    this.pendingOpen = Promise.resolve().then(() => { this.check(); return this.raw.open(options); });
    // close() waits for this exact open, including when the native API resolves
    // after the UI deadline. The lease remains reserved until cleanup finishes.
    this.pendingOpen.then(() => { this.opened = true; }, () => {});
    return this.io(() => this.pendingOpen, '打开串口');
  }
  async io(work, label, timeout = this.ioTimeout) {
    this.check();
    try {
      const native = Promise.resolve(work()); this.operations.add(native);
      native.then(() => this.operations.delete(native), () => this.operations.delete(native));
      return await bounded(() => native, timeout, label, this.controller.signal);
    }
    catch (error) { this.stop(error); throw error; }
  }
  async setSignals(signals) { return this.io(() => this.raw.setSignals(signals), '切换下载模式'); }
  async getSignals() { return this.io(() => this.raw.getSignals(), '读取串口状态'); }
  get readable() { return this.stream(this.raw.readable, false); }
  get writable() { return this.stream(this.raw.writable, true); }
  stream(stream, output) {
    if (!stream || this.controller.signal.aborted) return null;
    const lease = this;
    return new Proxy(stream, {
      get(target, key) {
        if (key === (output ? 'getWriter' : 'getReader')) return (...args) => {
          lease.check(); const handle = target[key](...args), handles = output ? lease.writers : lease.readers;
          handles.add(handle);
          const release = () => { try { handle.releaseLock(); } finally { handles.delete(handle); } };
          return new Proxy(handle, { get(h, member) {
            if (member === 'releaseLock') return release;
            if (member === 'write') return async data => {
              try { return await lease.io(() => h.write(data), '串口写入'); }
              catch (error) { try { release(); } catch {} throw error; }
            };
            if (member === 'read') return () => lease.io(() => h.read(), '串口读取', 0);
            const value = Reflect.get(h, member, h); return typeof value === 'function' ? value.bind(h) : value;
          } });
        };
        const value = Reflect.get(target, key, target); return typeof value === 'function' ? value.bind(target) : value;
      },
    });
  }
  stop(error = new Error('连接已取消')) {
    if (!this.controller.signal.aborted) this.controller.abort(error);
    for (const reader of this.readers) {
      try { Promise.resolve(reader.cancel()).catch(() => {}); } catch {}
      try { reader.releaseLock(); } catch {}
    }
    for (const writer of this.writers) {
      try { Promise.resolve(writer.abort(error)).catch(() => {}); } catch {}
      try { writer.releaseLock(); } catch {}
    }
    this.readers.clear(); this.writers.clear();
  }
  closeNative() {
    if (!this.opened) return Promise.resolve();
    this.nativeClose ??= Promise.resolve().then(() => this.raw.close()).then(() => { this.opened = false; });
    return this.nativeClose;
  }
  // SDK closes/reopens when changing bridge baudrate or retrying autodetection.
  // Only dispose(), not a normal SDK close(), permanently revokes ownership.
  async close() {
    if (this.controller.signal.aborted) return this.dispose();
    return this.io(() => this.closeNative(), '关闭串口');
  }
  async dispose() {
    if (!this.closing) {
      this.stop();
      this.closing = (async () => {
        try { await this.pendingOpen; } catch {}
        // SDK may fail before port.open() (or an occupied port may reject it).
        // Do not close someone else's active port when we never opened it.
        await this.closeNative();
        await Promise.allSettled([...this.operations]);
        if (owners.get(this.raw) === this) owners.delete(this.raw);
      })();
      this.closing.catch(() => {});
    }
    return bounded(() => this.closing, this.closeTimeout, '释放串口');
  }
}
