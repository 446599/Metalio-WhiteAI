import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bounded, SerialPortLease } from '../serial-port.mjs';
import { FlashSession } from '../session.mjs';
import { mockSdk } from './fixtures.mjs';

const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const deferred = () => { let resolve, reject; const promise = new Promise((a,b) => { resolve=a; reject=b; }); return { promise, resolve, reject }; };
const limits = { sdk: 25, handshake: 25, security: 25, flash: 25, table: 25, close: 25 };
function fakePort({ writeError = false, holdWrite = false, holdOpen = false, holdSignals = false } = {}) {
  const openGate = deferred(), writeGate = deferred(), signalsGate = deferred();
  const port = { opens: 0, closes: 0, writes: [], signals: [], readable: null, writable: null,
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    async open() {
      port.opens++; if (holdOpen) await openGate.promise;
      port.readable = new ReadableStream({ start(controller) { port.input = controller; } });
      const stream = new WritableStream({ async write(data) { port.writes.push(data); if (holdWrite) await writeGate.promise; if (writeError) throw new Error('USB sink failed'); } });
      port.writable = stream;
      const getWriter = stream.getWriter.bind(stream);
      stream.getWriter = () => { const writer=getWriter(); port.lastWriter=writer; writer.closed.catch(()=>{}); return writer; };
    },
    async close() {
      if (port.readable?.locked || port.writable?.locked) throw new Error('locked at close');
      port.closes++; port.readable=null; port.writable=null;
    },
    async setSignals(s) { if (holdSignals) await signalsGate.promise; port.signals.push(s); },
    async getSignals() { return {}; }, openGate, writeGate, signalsGate,
  };
  return port;
}

test('deadline rejects missing progress, rejects sync errors, and never starts an aborted operation', async () => {
  await assert.rejects(bounded(() => new Promise(()=>{}), 5, 'test'), /超时/);
  await assert.rejects(bounded(() => { throw new Error('sync'); }, 5, 'test'), /sync/);
  const c = new AbortController(); c.abort(new Error('cancel'));
  await assert.rejects(bounded(() => assert.fail('must not start'), 5, 'test', c.signal), /cancel/);
});
for (const [method, phase] of [['main','下载模式握手'],['getSecurityInfo','检查安全状态'],['detectFlashSize','读取 Flash 容量'],['readFlash','读取分区表']]) {
  test(`timeout at ${method} restores UI state and never authorizes writes`, async () => {
    const mock=mockSdk(); mock.sdk.ESPLoader.prototype[method]=()=>new Promise(()=>{});
    const stages=[], s=new FlashSession(async()=>mock.sdk, ()=>{}, {timeouts:limits,onStage:n=>stages.push(n)});
    await assert.rejects(s.connect({}), new RegExp(`${phase}超时`));
    assert.equal(s.device,null); assert.equal(s.busy,false); assert.equal(s.connecting,null);
    assert.equal(mock.calls.closes,1); assert.equal(mock.calls.writes.length,0); assert.ok(stages.includes(phase));
  });
}
test('cancel slow SDK load; late resolution cannot open an old port', async () => {
  const gate=deferred(), mock=mockSdk(), s=new FlashSession(()=>gate.promise,()=>{}, {timeouts:limits});
  const connected=s.connect({}); assert.equal(s.cancelConnect(),true);
  await assert.rejects(connected,/连接已取消/); gate.resolve(mock.sdk); await pause(5);
  assert.equal(mock.calls.options,null); assert.equal(s.busy,false); assert.equal(s.device,null);
  s.loadSdk=async()=>mock.sdk; await s.connect({}); await s.disconnect();
});
test('SDK timeout is bounded and retry works',async()=>{
  const mock=mockSdk(),s=new FlashSession(()=>new Promise(()=>{}),()=>{}, {timeouts:limits});
  await assert.rejects(s.connect({}),/加载刷机库超时/);
  s.loadSdk=async()=>mock.sdk; await s.connect({}); assert.ok(s.device); await s.disconnect();
});
test('disconnect event during handshake rejects immediately; late handshake cannot publish device', async () => {
  const gate=deferred(), mock=mockSdk(); mock.sdk.ESPLoader.prototype.main=()=>gate.promise;
  const s=new FlashSession(async()=>mock.sdk,()=>{}, {timeouts:limits}),port={};
  const connected=s.connect(port); await pause(1); s.markLost(port);
  await assert.rejects(connected,/USB 已断开/); gate.resolve(); await pause(2);
  assert.equal(s.device,null); assert.equal(s.busy,false); assert.equal(mock.calls.reads.length,0);
});
test('native USB avoids baud reopen; UART bridge retains selected baud and manual-reset choice',async()=>{
  for(const [vid,pid,expected] of [[0x303a,0x1001,115200],[0x10c4,0xea60,460800]]) {
    const m=mockSdk(),s=new FlashSession(async()=>m.sdk);
    await s.connect({getInfo:()=>({usbVendorId:vid,usbProductId:pid})},460800,'no_reset');
    assert.equal(m.calls.options.baudrate,expected); assert.deepEqual(m.calls.resets,['no_reset']); await s.disconnect();
  }
});
test('late native open is closed before the same port may be leased again', async()=>{
  const port=fakePort({holdOpen:true}),lease=new SerialPortLease(port,{ioTimeout:10,closeTimeout:10});
  await assert.rejects(lease.open({baudRate:115200}),/打开串口超时/);
  await assert.rejects(lease.dispose(),/释放串口超时/);
  assert.throws(()=>new SerialPortLease(port),/尚未释放/);
  port.openGate.resolve(); await pause(5); assert.equal(port.closes,1);
  const fresh=new SerialPortLease(port); await fresh.dispose(); assert.equal(port.closes,1);
});
test('late setSignals keeps the port reserved and cannot collide with a new lease',async()=>{
  const port=fakePort({holdSignals:true}),lease=new SerialPortLease(port,{ioTimeout:5,closeTimeout:5});
  await lease.open({}); await assert.rejects(lease.setSignals({dataTerminalReady:true}),/超时/);
  await assert.rejects(lease.dispose(),/超时/); assert.throws(()=>new SerialPortLease(port),/尚未释放/);
  port.signalsGate.resolve(); await pause(5); const fresh=new SerialPortLease(port); await fresh.dispose();
});

// CI installs the exact deployed SDK. These are NOT mocked Transport methods.
let Transport;
try { ({Transport}=await import('../vendor/esptool-js-0.7.0.bundle.js')); }
catch(error) { throw new Error(`Install the pinned SDK with vendor_sdk.py before testing: ${error.message}`); }

test('reproduce upstream writer-lock hang with deployed SDK, then verify adapter releases on failure',async()=>{
  const raw=fakePort({writeError:true}),old=new Transport(raw,false); old.trace=()=>{};
  await old.connect(); await assert.rejects(old.write(new Uint8Array([1])),/sink failed/);
  assert.equal(raw.writable.locked,true,'baseline leaks writer lock');
  // Manually release the test-observed handle, otherwise SDK.disconnect loops forever.
  raw.lastWriter.releaseLock(); await old.disconnect();
  const port=fakePort({writeError:true}),lease=new SerialPortLease(port,{ioTimeout:15,closeTimeout:15}),fixed=new Transport(lease,false);
  fixed.trace=()=>{}; await fixed.connect(); const loop=fixed.readLoop();
  await assert.rejects(fixed.write(new Uint8Array([1])),/sink failed/);
  assert.equal(port.writable.locked,false); await lease.dispose(); await loop; assert.equal(port.closes,1);
});
test('actual SDK pending reader can be cancelled and closed without data',async()=>{
  const port=fakePort(),lease=new SerialPortLease(port),t=new Transport(lease,false); t.trace=()=>{};
  await t.connect();const loop=t.readLoop();await pause(1); assert.equal(port.readable.locked,true);
  await lease.dispose();await loop; assert.equal(port.closes,1);
});
test('actual SDK stuck writer is bounded; same port stays reserved until native I/O settles',async()=>{
  const port=fakePort({holdWrite:true}),lease=new SerialPortLease(port,{ioTimeout:5,closeTimeout:5}),t=new Transport(lease,false);t.trace=()=>{};
  await t.connect(); await assert.rejects(t.write(new Uint8Array([1])),/串口写入超时/);
  await assert.rejects(lease.dispose(),/释放串口超时/);assert.equal(port.writable,null);assert.throws(()=>new SerialPortLease(port),/尚未释放/);
  port.writeGate.resolve(); await pause(5);const fresh=new SerialPortLease(port);await fresh.dispose();
});
test('actual SDK normal baud change may close and reopen within the same lease',async()=>{
  const port=fakePort(),lease=new SerialPortLease(port),t=new Transport(lease,false);t.trace=()=>{};
  await t.connect();const loop=t.readLoop();await t.changeBaudrate(460800);await loop;
  assert.equal(port.opens,2);assert.equal(port.closes,1);assert.equal(t.baudrate,460800);
  await lease.dispose();assert.equal(port.closes,2);
});


test('late device-lost callback from an old Transport cannot disconnect a new session on the same port',async()=>{
  const mock=mockSdk(), callbacks=[];
  mock.sdk.Transport.prototype.setDeviceLostCallback=function(callback){callbacks.push(callback);};
  const s=new FlashSession(async()=>mock.sdk),port={};
  await s.connect(port); await s.disconnect();
  await s.connect(port); callbacks[0]();
  assert.ok(s.device); assert.equal(s.lost,false);
  callbacks[1](); assert.equal(s.device,null); assert.equal(s.lost,true);
  await s.disconnect();
});
