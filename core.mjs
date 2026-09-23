import { md5, hex } from './hashes.mjs';
export const FLASH_SIZE = 16 * 1024 * 1024;
export const SECTOR = 4096;
export const TABLE_OFFSET = 0x8000;
export const DEFAULT_CSV = `# Metalio-WhiteAI / partitions/v1/16m.csv
nvs,data,nvs,0x9000,16K,
otadata,data,ota,0xd000,8K,
phy_init,data,phy,0xf000,4K,
model,data,spiffs,0x10000,400K,
ota_0,app,ota_0,0x80000,5M,
ota_1,app,ota_1,0x580000,5M,
resources,data,spiffs,0xa80000,400K,
font_data,data,0x40,0xae4000,5M,
coredump,data,coredump,0xfe4000,64K,
`;
export const SYSTEM_TARGETS = [
  { name: 'bootloader', type: -1, subtype: 0, offset: 0, size: TABLE_OFFSET, flags: 0 },
  { name: 'partition-table', type: -2, subtype: 0, offset: TABLE_OFFSET, size: SECTOR, flags: 0 },
];
const DATA_TYPES = { ota: 0, phy: 1, nvs: 2, coredump: 3, nvs_keys: 4, efuse: 5, fat: 0x81, spiffs: 0x82, littlefs: 0x83 };
const check = (condition, message) => { if (!condition) throw new Error(message); };
export const address = n => `0x${n.toString(16).toUpperCase().padStart(6, '0')}`;
export const sizeLabel = n => n >= 1048576 ? `${+(n / 1048576).toFixed(2)} MiB` : `${+(n / 1024).toFixed(2)} KiB`;
export const align = (n, unit = SECTOR) => Math.ceil(n / unit) * unit;
export const overlaps = (a, b) => a.offset < b.offset + b.size && b.offset < a.offset + a.size;
export const samePartition = (a, b) => !!a && !!b && ['name', 'type', 'subtype', 'offset', 'size', 'flags'].every(k => a[k] === b[k]);
export function number(text) {
  if (typeof text === 'number') { check(Number.isSafeInteger(text) && text >= 0, '数值无效'); return text; }
  const match = /^(0x[\da-f]+|\d+)([km])?$/i.exec(String(text).trim());
  check(match, `无效地址或容量：${text}`);
  const result = Number(match[1]) * (match[2] ? match[2].toLowerCase() === 'k' ? 1024 : 1048576 : 1);
  check(Number.isSafeInteger(result), '数值超出范围');
  return result;
}
function byte(value) { const n = number(value); check(n <= 255, '类型值必须在 0..255'); return n; }
function subtype(type, value) {
  if (type === 0) {
    if (value === 'factory') return 0;
    if (value === 'test') return 0x20;
    if (/^ota_(?:[0-9]|1[0-5])$/.test(value)) return 0x10 + Number(value.slice(4));
  }
  if (type === 1 && Object.hasOwn(DATA_TYPES, value)) return DATA_TYPES[value];
  return byte(value);
}
export function validateLayout(partitions, flashSize = FLASH_SIZE) {
  check(Array.isArray(partitions) && partitions.length > 0 && partitions.length <= 95, '分区表为空或分区过多');
  const sorted = [...partitions].sort((a, b) => a.offset - b.offset), names = new Set();
  for (const p of sorted) {
    check(/^[A-Za-z0-9_.-]{1,16}$/.test(p.name) && !names.has(p.name) && !SYSTEM_TARGETS.some(s => s.name === p.name), `分区名称无效或重复：${p.name}`);
    names.add(p.name);
    check(Number.isInteger(p.type) && p.type >= 0 && p.type <= 254 && Number.isInteger(p.subtype) && p.subtype >= 0 && p.subtype <= 255, '分区类型无效');
    check(Number.isSafeInteger(p.offset) && Number.isSafeInteger(p.size) && p.offset >= TABLE_OFFSET + SECTOR && p.size > 0 && p.offset + p.size <= flashSize, `${p.name} 超出 Flash 范围`);
    check(p.offset % SECTOR === 0 && p.size % SECTOR === 0, `${p.name} 必须按 4 KiB 对齐`);
    check(p.type !== 0 || p.offset % 0x10000 === 0, `${p.name} 应用地址必须按 64 KiB 对齐`);
    check(Number.isInteger(p.flags) && p.flags >= 0 && p.flags <= 3, `${p.name} 含不支持的分区标志`);
  }
  for (let i = 1; i < sorted.length; i++) check(!overlaps(sorted[i - 1], sorted[i]), '分区范围重叠');
  return sorted;
}
export function parseCsv(text, flashSize = FLASH_SIZE) {
  check(typeof text === 'string' && text.length <= 65536, 'CSV 文件过大');
  const result = []; let next = TABLE_OFFSET + SECTOR;
  for (const raw of text.replace(/^\uFEFF/, '').split(/\r?\n/)) {
    const line = raw.split('#')[0].trim(); if (!line) continue;
    const fields = line.split(',').map(s => s.trim());
    check(fields.length >= 5 && fields.length <= 6, 'CSV 应为 Name,Type,SubType,Offset,Size,Flags');
    const [name, typeText, subText, offsetText, sizeText, flagsText = ''] = fields;
    const type = typeText === 'app' ? 0 : typeText === 'data' ? 1 : byte(typeText);
    let flags = 0;
    for (const flag of flagsText.split(':').filter(Boolean)) {
      check(flag === 'encrypted' || flag === 'readonly', `不支持的标志：${flag}`);
      flags |= flag === 'encrypted' ? 1 : 2;
    }
    const offset = offsetText ? number(offsetText) : align(next, type === 0 ? 0x10000 : SECTOR);
    const size = number(sizeText);
    result.push({ name, type, subtype: subtype(type, subText), offset, size, flags });
    next = offset + size;
  }
  return validateLayout(result, flashSize);
}
export function parseTable(bytes, flashSize = FLASH_SIZE) {
  check(bytes instanceof Uint8Array && bytes.length >= 32 && bytes.length <= SECTOR && bytes.length % 32 === 0, '分区表二进制长度无效');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), result = [];
  let ended = false, digestSeen = false;
  for (let offset = 0; offset + 32 <= bytes.length; offset += 32) {
    const entry = bytes.subarray(offset, offset + 32), magic = view.getUint16(offset, true);
    if (magic === 0xffff && entry.every(b => b === 0xff)) {
      check(bytes.subarray(offset).every(b => b === 0xff), '分区表末尾含未知数据'); ended = true; break;
    }
    if (magic === 0xebeb) {
      check(!digestSeen && result.length > 0 && entry.subarray(2, 16).every(b => b === 0xff), '分区表 MD5 记录无效');
      check(hex(entry.subarray(16)) === md5(bytes.subarray(0, offset)), '分区表 MD5 校验失败');
      digestSeen = true; continue;
    }
    check(!digestSeen && magic === 0x50aa, '分区表损坏或未烧录');
    const label = entry.subarray(12, 28), end = label.indexOf(0);
    const name = new TextDecoder('utf-8', { fatal: true }).decode(end === -1 ? label : label.subarray(0, end));
    result.push({ name, type: entry[2], subtype: entry[3], offset: view.getUint32(offset + 4, true), size: view.getUint32(offset + 8, true), flags: view.getUint32(offset + 28, true) });
  }
  check(ended || digestSeen, '分区表缺少结束标记');
  return validateLayout(result, flashSize);
}
export function layoutCsv(parts) {
  return '# Name,Type,SubType,Offset,Size,Flags\n' + parts.map(p => `${p.name},${p.type},${p.subtype},${address(p.offset)},${address(p.size)},${[p.flags & 1 ? 'encrypted' : '', p.flags & 2 ? 'readonly' : ''].filter(Boolean).join(':')}`).join('\n') + '\n';
}
export function isSensitive(p) { return p.type !== 0 && !(p.type === 1 && p.subtype === 0x40 && p.name === 'font_data'); }
export function assertDevice(device) {
  check(device && device.chip === 'ESP32-S3', '仅支持 ESP32-S3，请核对所选设备');
  check(Number.isSafeInteger(device.flashSize) && device.flashSize > 0 && device.flashSize <= 128 * 1048576, '无法确认实际 Flash 容量，禁止写入');
  const info = device.security;
  check(info && Number.isInteger(info.flags) && info.flags >= 0 && info.flags <= 0xffffffff && Number.isInteger(info.flashCryptCnt) && info.flashCryptCnt >= 0 && info.flashCryptCnt <= 255, '无法确认芯片安全状态，禁止写入');
  const encrypted = info.flashCryptCnt.toString(2).replaceAll('0', '').length % 2 === 1;
  check(!(info.flags & 5) && !encrypted, '设备启用了 Secure Boot、Flash 加密或安全下载模式；本页不支持刷写');
}
function checkImage(data, name) {
  check(data.length >= 24 && data[0] === 0xe9 && data[1] > 0 && data[1] <= 16, `${name} 不是有效的 ESP 固件镜像`);
  check(new DataView(data.buffer, data.byteOffset, data.byteLength).getUint16(12, true) === 9, `${name} 不是 ESP32-S3 镜像`);
}
/** Pure preflight. No operation here reads or writes a device. */
export function buildPlan(rows, layout, device) {
  assertDevice(device); validateLayout(layout, device.flashSize);
  check(rows.length > 0 && rows.length <= 32, '请选择至少一个分区（最多 32 项）');
  const selected = rows.map(row => {
    const p = { ...row, offset: number(row.offset), size: number(row.size) };
    check(p.data instanceof Uint8Array && p.data.length > 0, `${p.name} 尚未选择有效文件`);
    check(p.offset % SECTOR === 0 && p.size > 0 && p.size % SECTOR === 0, `${p.name} 地址和容量必须按 4 KiB 对齐`);
    check(p.data.length <= p.size && p.offset + p.size <= device.flashSize && align(p.data.length) <= p.size, `${p.name} 文件超出目标容量或 Flash 边界`);
    p.eraseSize = align(p.data.length);
    return p;
  }).sort((a, b) => a.offset - b.offset);
  for (let i = 1; i < selected.length; i++) check(!overlaps({ offset: selected[i - 1].offset, size: selected[i - 1].eraseSize }, { offset: selected[i].offset, size: selected[i].eraseSize }), '待写入文件的擦除扇区重叠');
  const tableWrite = selected.find(p => p.offset === TABLE_OFFSET);
  let targetLayout = layout;
  const warnings = new Set(['只擦除写入文件覆盖的 4 KiB 扇区，不清空整个 Flash；扇区尾部会被擦除。']);
  if (tableWrite) {
    check(tableWrite.data.length <= SECTOR, '分区表文件超过 4 KiB；不接受合并镜像');
    targetLayout = parseTable(tableWrite.data, device.flashSize);
    check(targetLayout.length === layout.length && layout.every(p => targetLayout.some(q => samePartition(p, q))), '待刷分区表与页面布局不一致，请先导入对应 CSV 或 BIN');
    warnings.add('将修改分区表。布局变化可能让旧数据失去原来的含义，请先备份。');
  }
  let dangerous = false;
  for (const p of selected) {
    const range = { offset: p.offset, size: p.eraseSize };
    const system = SYSTEM_TARGETS.find(s => overlaps(range, s));
    if (system) {
      check(p.offset === system.offset && p.eraseSize <= system.size, '自定义范围穿越 bootloader / 分区表边界');
      if (system.type === -1) checkImage(p.data, p.fileName || p.name);
      else check(tableWrite === p, '分区表只能从 0x8000 写入');
      dangerous = true;
    } else {
      const target = targetLayout.find(t => p.offset >= t.offset && p.offset + p.eraseSize <= t.offset + t.size);
      if (!p.custom) check(target && samePartition(p, target), `${p.name} 与当前页面分区定义不匹配`);
      else check(!targetLayout.some(t => overlaps(range, t) && t !== target), '自定义范围穿越分区边界');
      if (target?.type === 0) {
        check(p.offset === target.offset, '应用镜像必须从应用分区起始地址写入');
        checkImage(p.data, p.fileName || p.name);
        warnings.add(`写入 ${target.name} 不会自动切换启动槽。请确认当前使用的 OTA 槽。`);
      }
      check(!target || !(target.flags & 1), '不支持直接写入标记 encrypted 的分区');
      if (!tableWrite && !p.custom) {
        check(device.partitions && device.partitions.some(d => samePartition(p, d)), `${p.name} 与设备分区表不一致或尚未读取；禁止直接更新`);
      }
      if (p.custom) { dangerous = true; warnings.add('自定义地址属于高级操作，请自行核对文件用途和起始地址。'); }
      if (!target || isSensitive(target) || (target.flags & 2)) dangerous = true;
      if (target?.name === 'nvs' || (target?.type === 1 && [2, 4].includes(target.subtype))) warnings.add('将覆盖 NVS / 密钥数据，可能丢失 Wi-Fi、小智凭据、闹钟和最近胶囊。');
      if (target?.type === 1 && target.subtype === 0) {
        check(p.offset === target.offset && p.data.length === target.size, 'OTA 选择数据必须完整覆盖 otadata 分区');
        warnings.add('将覆盖 OTA 选择数据，改变启动槽；请使用本次构建的 ota_data_initial.bin。');
      }
    }
    check(!device.partitions?.some(d => (d.flags & 1) && overlaps(range, d)), '写入范围触及设备的加密分区，禁止写入');
  }
  if (tableWrite && !device.partitions) {
    check(selected.some(p => p.offset === 0) && selected.some(p => targetLayout.some(t => t.type === 0 && p.offset === t.offset)), '空白或损坏分区表：请同时选择 bootloader 和至少一个应用镜像');
  }
  return { files: selected, warnings: [...warnings], dangerous, tableWrite: !!tableWrite, total: selected.reduce((sum, p) => sum + p.data.length, 0) };
}
export function confirmPlan(plan, acknowledged, riskText) {
  check(acknowledged === true, '请确认已核对设备、启动槽并备份重要数据');
  check(!plan.dangerous || riskText === 'FLASH', '敏感分区或自定义地址需输入 FLASH 确认');
}
