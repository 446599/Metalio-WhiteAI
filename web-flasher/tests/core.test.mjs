import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { md5 } from '../hashes.mjs';
import { DEFAULT_CSV, FLASH_SIZE, SYSTEM_TARGETS, number, parseCsv, parseTable, layoutCsv, validateLayout, assertDevice, buildPlan, confirmPlan, isSensitive } from '../core.mjs';
import { FlashSession, loadRows } from '../session.mjs';
import { layout, tableBytes, device, image, row, mockSdk } from './fixtures.mjs';

for (const text of ['', 'a', 'abc', 'message digest', 'abcdefghijklmnopqrstuvwxyz', 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', '1234567890'.repeat(8)]) {
  test(`MD5 standard vector ${JSON.stringify(text)}`, () => assert.equal(md5(new TextEncoder().encode(text)), createHash('md5').update(text).digest('hex')));
}
for (const size of [1, 55, 56, 63, 64, 65, 127, 128, 4095, 4096, 65536, 1048576]) {
  test(`MD5 binary ${size} bytes`, () => { const data = randomBytes(size); assert.equal(md5(data), createHash('md5').update(data).digest('hex')); });
}
for (const text of ['', '123abc', '1e6', '-1', '0x', '0xgg', '2.5M', 'Infinity', 'NaN']) test(`reject malformed number ${text}`, () => assert.throws(() => number(text)));
test('numeric units and addresses', () => { assert.equal(number('0x80000'), 524288); assert.equal(number('16K'), 16384); assert.equal(number('5M'), 5242880); assert.throws(() => number(-1)); });
test('preset exactly matches repository partition table', () => {
  assert.deepEqual(parseCsv(readFileSync(new URL('../../partitions/v1/16m.csv', import.meta.url), 'utf8')), layout);
  assert.equal(layout.find(p => p.name === 'font_data').offset, 0xae4000);
  assert.equal(layout.find(p => p.name === 'ota_0').size, 5 * 1048576);
});
test('CSV supports BOM CRLF comments and auto alignment', () => {
  const p = parseCsv('\ufeff# comment\r\nnvs,data,nvs,,16K,\r\napp,app,factory,,1M, # tail\r\n');
  assert.equal(p[0].offset, 0x9000); assert.equal(p[1].offset, 0x10000);
  assert.deepEqual(parseCsv(layoutCsv(layout)), layout);
});
for (const csv of ['', 'nvs,data,nvs,0x9000,0,', 'nvs,data,nvs,0x9001,4K,', 'nvs,data,nvs,0x9000,4K,magic', DEFAULT_CSV + 'nvs,data,nvs,0xff4000,4K,', 'a,app,ota_16,0x10000,1M,', 'a,app,factory,0x9000,1M,', 'a,data,nvs,0x9000,8K,\nb,data,nvs,0xa000,4K,', '<script>,data,nvs,0x9000,4K,', 'bootloader,data,nvs,0x9000,4K,']) {
  test(`invalid CSV ${csv.slice(0, 45)}`, () => assert.throws(() => parseCsv(csv)));
}
test('CSV encryption/read-only flags round trip', () => { const p = parseCsv('a,data,spiffs,0x9000,4K,encrypted:readonly'); assert.equal(p[0].flags, 3); assert.deepEqual(parseCsv(layoutCsv(p)), p); });
test('binary table round trip with digest', () => assert.deepEqual(parseTable(tableBytes()), layout));
test('binary table corruption and truncation refused', () => {
  const data = tableBytes(); data[12] ^= 1; assert.throws(() => parseTable(data), /MD5/);
  assert.throws(() => parseTable(new Uint8Array(4096).fill(255)));
  assert.throws(() => parseTable(tableBytes().slice(0, 63)));
  assert.throws(() => parseTable(tableBytes(), 4 * 1048576));
  const duplicate = [...layout, { ...layout[0] }]; assert.throws(() => validateLayout(duplicate));
});
for (const info of [{ flags: 1, flashCryptCnt: 0 }, { flags: 4, flashCryptCnt: 0 }, { flags: 0, flashCryptCnt: 1 }, { flags: 0, flashCryptCnt: 7 }, null, { flags: 0 }, { flags: 0, flashCryptCnt: -1 }]) test(`security block ${JSON.stringify(info)}`, () => assert.throws(() => assertDevice({ ...device(), security: info })));
test('flash encryption uses bit parity, not nonzero', () => assert.doesNotThrow(() => assertDevice({ ...device(), security: { flags: 0, flashCryptCnt: 3 } })));
test('wrong chip or unknown capacity refused', () => {
  assert.throws(() => assertDevice({ ...device(), chip: 'ESP32' })); assert.throws(() => assertDevice({ ...device(), flashSize: 0 }));
});
test('default app plan preserves unselected partitions', () => {
  const p = buildPlan([row()], layout, device()); assert.equal(p.files.length, 1); assert.equal(p.dangerous, false);
  assert.equal(p.files[0].offset, 0x80000); assert.equal(p.files[0].eraseSize, 4096); assert(p.warnings.some(w => w.includes('启动槽')));
});
test('font selection is independent and not NVS', () => { const p = buildPlan([row('font_data', new Uint8Array(1024))], layout, device()); assert.equal(p.dangerous, false); assert.equal(isSensitive({ ...layout[0], name: 'font_data' }), true); });
test('empty, oversize, wrong chip image or unaligned data refused', () => {
  assert.throws(() => buildPlan([], layout, device()));
  assert.throws(() => buildPlan([row('ota_0', new Uint8Array())], layout, device()));
  assert.throws(() => buildPlan([row('ota_0', image(5 * 1048576 + 1))], layout, device()));
  const bad = image(); bad[12] = 0; assert.throws(() => buildPlan([row('ota_0', bad)], layout, device()), /ESP32-S3/);
  assert.throws(() => buildPlan([{ ...row(), offset: 0x80001 }], layout, device()));
});
test('device layout mismatch and unknown table block ordinary update', () => {
  assert.throws(() => buildPlan([row()], layout, { ...device(), partitions: null }));
  assert.throws(() => buildPlan([row()], layout, { ...device(), partitions: layout.filter(p => p.name !== 'ota_0') }));
});
test('overlapping erase sectors and crossing boundaries refused', () => {
  assert.throws(() => buildPlan([row(), { ...row(), custom: true }], layout, device()), /重叠/);
  assert.throws(() => buildPlan([{ name: 'raw', custom: true, offset: 0xc000, size: 0x2000, data: new Uint8Array(0x2000) }], layout, device()), /边界/);
  assert.throws(() => buildPlan([{ name: 'raw', custom: true, offset: 0x1000, size: 0x9000, data: new Uint8Array(0x9000) }], layout, device()));
});
test('NVS requires explicit destructive acknowledgement', () => {
  const p = buildPlan([row('nvs', new Uint8Array(128))], layout, device()); assert(p.dangerous);
  assert.throws(() => confirmPlan(p, false, 'FLASH')); assert.throws(() => confirmPlan(p, true, '')); assert.throws(() => confirmPlan(p, true, 'flash'));
  assert.doesNotThrow(() => confirmPlan(p, true, 'FLASH'));
});
test('custom address requires risk confirmation', () => {
  const p = buildPlan([{ name: 'raw', custom: true, offset: 0x74000, size: 4096, data: new Uint8Array(128) }], layout, device()); assert(p.dangerous);
});
test('custom writes cannot bypass encrypted partitions', () => {
  const encrypted = layout.map(p => p.name === 'nvs' ? { ...p, flags: 1 } : p);
  assert.throws(() => buildPlan([{ ...row('nvs', new Uint8Array(8)), custom: true }], layout, { ...device(), partitions: encrypted }), /加密/);
});
test('OTA metadata must be a full partition image', () => {
  assert.throws(() => buildPlan([row('otadata', new Uint8Array(4096))], layout, device()));
  const p = buildPlan([row('otadata', new Uint8Array(8192).fill(255))], layout, device()); assert(p.dangerous);
});
const initialRows = () => [row(), row('font_data', new Uint8Array(120)), row('otadata', new Uint8Array(8192).fill(255)), { ...SYSTEM_TARGETS[0], data: image(), fileName: 'bootloader.bin' }, { ...SYSTEM_TARGETS[1], data: tableBytes(), fileName: 'partition-table.bin' }];
test('first install does not select NVS and has a consistent table', () => {
  const p = buildPlan(initialRows(), layout, { ...device(), partitions: null }); assert(p.dangerous); assert(p.tableWrite); assert.equal(p.files.length, 5); assert(!p.files.some(p => p.offset === 0x9000));
});
test('blank-table recovery needs bootloader and an app', () => {
  assert.throws(() => buildPlan([{ ...SYSTEM_TARGETS[1], data: tableBytes() }], layout, { ...device(), partitions: null }));
});
test('a selected replacement table must match the page layout', () => {
  const changed = layout.map(p => p.name === 'nvs' ? { ...p, name: 'settings' } : p);
  assert.throws(() => buildPlan([{ ...SYSTEM_TARGETS[1], data: tableBytes(changed) }], layout, device()), /不一致/);
});
test('loadRows reads only selected local files, hashes without uploading', async () => {
  const loaded = await loadRows([{ ...row(), selected: true, file: new File([image()], '<img onerror=alert(1)>.bin') }, { selected: false, file: { arrayBuffer() { throw new Error('not selected'); } } }]);
  assert.equal(loaded.length, 1); assert.equal(loaded[0].sha256, createHash('sha256').update(image()).digest('hex')); assert.equal(loaded[0].file, undefined);
  await assert.rejects(() => loadRows([{ ...row(), selected: true, file: new File([], 'empty.bin') }]));
  await assert.rejects(() => loadRows([{ ...row(), selected: true, file: { size: FLASH_SIZE + 1, arrayBuffer() { throw new Error('must not read'); } } }]));
});
async function connectMock(options = {}) {
  const mock = mockSdk(options), session = new FlashSession(async () => mock.sdk);
  await session.connect({ id: 'test-port' }); return { ...mock, session };
}
test('connection verifies chip, security, capacity and actual table', async () => { const { session, calls } = await connectMock(); assert.equal(session.device.flashSize, FLASH_SIZE); assert.deepEqual(session.device.partitions, layout); assert.equal(calls.options.romBaudrate, 115200); await session.disconnect(); assert.equal(calls.closes, 1); });
for (const options of [{ connectError: true }, { chip: 'ESP32-C3' }, { flashSize: undefined }, { securityError: true }, { security: { flags: 1, flashCryptCnt: 0 } }, { readError: true }, { shortRead: true }]) {
  test(`failed connection cleans up ${JSON.stringify(options)}`, async () => {
    const mock = mockSdk(options), session = new FlashSession(async () => mock.sdk);
    await assert.rejects(() => session.connect({})); assert.equal(session.device, null); assert.equal(session.transport, null); assert.equal(mock.calls.writes.length, 0); assert.equal(mock.calls.closes, 1);
  });
}
test('acknowledgement validated before any write', async () => {
  const { session, calls } = await connectMock();
  await assert.rejects(() => session.flash([row()], layout, false, '')); assert.equal(calls.writes.length, 0); assert.equal(calls.reads.length, 1);
  await assert.rejects(() => session.flash([row('nvs', new Uint8Array(32))], layout, true, '')); assert.equal(calls.writes.length, 0);
});
test('flash uses Uint8Array, keep parameters, no full erase and MD5 verification', async () => {
  const { session, calls, memory } = await connectMock(); const progress = [];
  memory.fill(123, 0x9000, 0xd000); const plan = await session.flash([row()], layout, true, '', p => progress.push(p));
  const opts = calls.writes[0]; assert.equal(opts.eraseAll, false); assert.equal(opts.flashSize, 'keep'); assert.equal(opts.flashMode, 'keep'); assert.equal(opts.flashFreq, 'keep'); assert(opts.compress); assert(opts.fileArray[0].data instanceof Uint8Array);
  assert.equal(plan.files.length, 1); assert.equal(progress.at(-1), 100); assert(memory.subarray(0x9000, 0xd000).every(n => n === 123));
});
test('table changed after review blocks all writes', async () => {
  const { session, calls, memory } = await connectMock(); memory[0x8000] ^= 1;
  await assert.rejects(() => session.flash([row()], layout, true, ''), /改变/); assert.equal(calls.writes.length, 0);
});
for (const options of [{ writeError: true }, { corrupt: true }]) test(`write failure never reports completion ${JSON.stringify(options)}`, async () => {
  const { session } = await connectMock(options), progress = [];
  await assert.rejects(() => session.flash([row()], layout, true, '', p => progress.push(p))); assert(!progress.includes(100)); assert.equal(session.device, null); assert.equal(session.transport, null);
});
test('metadata written after application and font files', async () => {
  const { session, calls } = await connectMock(); await session.flash(initialRows(), layout, true, 'FLASH');
  assert.deepEqual(calls.writes[0].fileArray.map(f => f.address), [0x80000, 0xae4000, 0, 0x8000, 0xd000]);
});
test('backups are bounded reads only; foreign ranges refused', async () => {
  const { session, calls } = await connectMock(); const nvs = layout.find(p => p.name === 'nvs');
  const data = await session.backup(nvs); assert.equal(data.length, nvs.size); assert.equal(calls.writes.length, 0);
  await assert.rejects(() => session.backup({ ...nvs, offset: 0 }));
});
test('serial operations cannot run concurrently', async () => {
  let release, entered; const waitEntered = new Promise(resolve => { entered = resolve; });
  const { session } = await connectMock({ onRead: async (_address, _size, count) => { if (count === 2) { entered(); await new Promise(resolve => { release = resolve; }); } } });
  const backup = session.backup(layout[0]); await waitEntered;
  await assert.rejects(() => session.flash([row()], layout, true, ''), /设备忙/);
  release(); await backup; assert.equal(session.busy, false);
});
test('device removal during write cannot become success', async () => {
  let sessionRef; const port = {};
  const mock = mockSdk({ onWrite: () => sessionRef.markLost(port) }); sessionRef = new FlashSession(async () => mock.sdk);
  await sessionRef.connect(port); await assert.rejects(() => sessionRef.flash([row()], layout, true, ''), /断开/); assert.equal(sessionRef.device, null);
});
test('restart uses documented API and releases port', async () => {
  const { session, calls } = await connectMock(); await session.restart(); assert.equal(calls.resets.at(-1), 'hard_reset'); assert.equal(session.device, null); assert.equal(calls.closes, 1);
});
