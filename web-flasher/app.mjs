import { DEFAULT_CSV, SYSTEM_TARGETS, FLASH_SIZE, TABLE_OFFSET, parseCsv, parseTable, buildPlan, confirmPlan, samePartition, address, sizeLabel, number, isSensitive } from './core.mjs';
import { FlashSession, loadRows } from './session.mjs';
import { loadSdk, SDK_VERSION } from './sdk.mjs';
const $ = id => document.getElementById(id);
const element = (tag, text, className = '') => {
  const node = document.createElement(tag); if (text !== undefined) node.textContent = text;
  if (className) node.className = className; return node;
};
let layout = parseCsv(DEFAULT_CSV), rows = [], busy = false, pending = null, customCount = 0, resetting = false;
let layoutName = 'Metalio 16 MB 预设', logs = [];
const supported = window.isSecureContext && typeof navigator.serial?.requestPort === 'function' && location.protocol !== 'file:';
function log(text) {
  for (const line of String(text).replace(/\x1b\[[0-9;]*m/g, '').split(/[\r\n]+/).filter(Boolean)) {
    logs.push(`[${new Date().toLocaleTimeString('zh-CN', { hour12: false })}] ${line.slice(0, 500)}`);
  }
  logs = logs.slice(-240); $('log').textContent = logs.join('\n'); $('log-count').textContent = String(logs.length);
  $('log').scrollTop = $('log').scrollHeight;
}
let connecting = false, connectionStage = '';
const session = new FlashSession(loadSdk, log, { onStage: name => {
  connectionStage = name; if (connecting) { result(`${name}…`); updateControls(); }
} });
function result(message, kind = '') { $('result').textContent = message; $('result').className = `result ${kind}`; }
function invalidate() { pending = null; $('progress').value = 0; }
function setRows(parts) {
  layout = parts;
  rows = [...SYSTEM_TARGETS, ...parts].map(p => ({ ...p, selected: p.name === 'ota_0', file: null }));
  if (!parts.some(p => p.name === 'ota_0')) { const app = rows.find(p => p.type === 0); if (app) app.selected = true; }
  invalidate(); renderRows();
}
function active() { return !!session.device && !session.lost; }
function actualTarget(row) {
  return active() && !row.custom && [...SYSTEM_TARGETS, ...(session.device.partitions || [])].find(p => samePartition(p, row));
}
function updateControls() {
  document.querySelectorAll('button,input,select').forEach(node => { node.disabled = busy; });
  $('connect').disabled = busy || active() || !supported;
  $('connect').hidden = active(); $('disconnect').hidden = !active();
  $('cancel-connect').hidden = !connecting; $('cancel-connect').disabled = !connecting || !session.connecting;
  $('save-log').disabled = false;
  $('review').disabled = busy || !active() || !rows.some(p => p.selected);
  $('restart').disabled = busy || !active();
  $('baud').disabled = busy || active(); $('reset').disabled = busy || active();
  $('device-layout').disabled = busy || !session.device?.partitions;
  document.querySelectorAll('[data-backup]').forEach(button => {
    const row = rows[Number(button.dataset.backup)]; button.disabled = busy || !actualTarget(row);
  });
  $('selection-count').textContent = `已选 ${rows.filter(p => p.selected).length} 项`;
  $('connection-state').textContent = connecting ? (connectionStage || '选择串口') : busy ? '处理中' : active() ? '已连接' : '未连接';
  $('connection-state').className = `badge${active() ? ' connected' : ''}`;
  $('device-name').textContent = active() ? `${session.device.chip} · ${sizeLabel(session.device.flashSize)} Flash` : '通过 USB 连接你的 WhiteAI';
  $('device-detail').textContent = active() ? '芯片与安全状态已确认 · 设备处于下载模式' : connecting ? '仅连接和读取信息，不会擦写 Flash。可随时取消。' : '先关闭串口监视器。连接操作会使设备进入下载模式。';
  const matches = active() && session.device.partitions && layout.length === session.device.partitions.length && layout.every(p => session.device.partitions.some(d => samePartition(p, d)));
  $('layout-status').textContent = `布局：${layoutName} · ${!active() ? '尚未与设备核对' : matches ? '与设备分区表一致' : session.device.partitions ? '与设备布局不同，刷写时将逐项校验' : '设备分区表无效，仅允许确认后的恢复部署'}`;
  $('layout-status').className = `layout-status${active() && !matches ? ' mismatch' : ''}`;
}
function clearPreset() { document.querySelectorAll('[data-preset]').forEach(b => b.classList.remove('active')); }
function renderRows() {
  $('partitions').replaceChildren();
  rows.forEach((row, index) => {
    if (!$('show-system').checked && !row.selected && !row.custom && isSensitive(row)) return;
    const tr = element('tr'); tr.dataset.target = row.name;
    const selection = element('td'), check = element('input'); check.type = 'checkbox'; check.checked = row.selected;
    check.setAttribute('aria-label', `刷写 ${row.name}`);
    check.addEventListener('change', () => { row.selected = check.checked; invalidate(); clearPreset(); updateControls(); });
    selection.append(check);
    const name = element('td'); name.append(element('span', row.name, 'name'));
    const hints = { ota_0: '应用 · 槽 0', ota_1: '应用 · 槽 1', font_data: '完整字体', bootloader: '引导程序 · 敏感', 'partition-table': '分区布局 · 敏感', nvs: '配置与凭据 · 敏感', otadata: '启动槽选择 · 敏感' };
    name.append(element('small', row.custom ? '高级操作' : hints[row.name] || '数据分区'));
    const range = element('td');
    if (row.custom) {
      range.className = 'custom-cell';
      for (const [key, label] of [['offset', '起始地址'], ['size', '目标容量']]) {
        const input = element('input'); input.type = 'text'; input.value = typeof row[key] === 'number' ? address(row[key]) : row[key];
        input.setAttribute('aria-label', `${row.name} ${label}`); input.placeholder = label;
        input.addEventListener('input', () => { row[key] = input.value; invalidate(); updateControls(); }); range.append(input);
      }
    } else { range.append(element('code', address(row.offset)), element('small', sizeLabel(row.size))); }
    const fileCell = element('td'), box = element('div', undefined, 'file-box');
    const input = element('input'); input.type = 'file'; input.accept = '.bin,.fontpack'; input.hidden = true;
    input.setAttribute('aria-label', `选择 ${row.name} 文件`);
    const pick = element('button', row.file ? '更换' : '选择文件', 'file-button'); pick.addEventListener('click', () => input.click());
    const info = element('span', row.file ? `${row.file.name} · ${sizeLabel(row.file.size)}` : '未选择', 'file-text');
    input.addEventListener('change', () => {
      row.file = input.files?.[0] || null; if (row.file) row.selected = true;
      invalidate(); clearPreset(); renderRows();
    });
    box.append(pick, info, input); fileCell.append(box);
    const actions = element('td');
    const backup = element('button', row.custom ? '移除' : '备份', 'backup-button');
    if (row.custom) backup.addEventListener('click', () => { rows = rows.filter(p => p !== row); invalidate(); renderRows(); });
    else {
      backup.dataset.backup = String(index); backup.setAttribute('aria-label', `备份 ${row.name}`);
      backup.addEventListener('click', () => run(async () => {
        const target = actualTarget(row); if (!target) throw new Error('分区与设备不匹配，不能备份');
        result(`正在备份 ${row.name}，不会写入设备。`);
        const bytes = await session.backup(target, (read, size) => { $('progress').value = read / size * 100; });
        download(bytes, `whiteai-${row.name}-${address(row.offset)}-${new Date().toISOString().replaceAll(':', '-')}.bin`, 'application/octet-stream');
        result(`${row.name} 备份已读完并交给浏览器下载。含凭据的备份请妥善保管。`, 'success');
      }));
    }
    actions.append(backup); tr.append(selection, name, range, fileCell, actions); $('partitions').append(tr);
  });
  updateControls();
}
async function run(work) {
  if (busy) return;
  busy = true; updateControls();
  try { await work(); }
  catch (error) { result(error.message || String(error), 'error'); log(`错误：${error.message || error}`); }
  finally {
    if (session.lost && session.transport && !session.busy) {
      try { await session.disconnect(); } catch { log('断连后的端口清理失败，请重新插拔 USB。'); }
    }
    busy = false; updateControls();
  }
}
function download(data, name, type = 'text/plain;charset=utf-8') {
  const url = URL.createObjectURL(new Blob([data], { type })), a = element('a');
  a.href = url; a.download = name; document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
$('connect').addEventListener('click', () => run(async () => {
  connecting = true; connectionStage = '选择串口'; updateControls();
  result('请选择串口。连接仅检查设备，不会写入 Flash。');
  try {
    // Must remain in the user's click; never reopen a chooser in a timer.
    const port = await navigator.serial.requestPort();
    const promise = session.connect(port, Number($('baud').value), $('reset').value);
    updateControls();
    const device = await promise;
    log(`ESP32-S3 / ${sizeLabel(device.flashSize)} / esptool-js ${SDK_VERSION}`);
    result(device.partitions ? '连接成功。设备分区表已读取；请添加所需的本地文件。' : `已连接，但设备分区表无效：${device.tableError}。请使用对应布局恢复部署。`, device.partitions ? 'success' : '');
  } catch (error) {
    if (error.name === 'NotFoundError') { result('未选择串口。'); return; }
    if (!String(error.message).includes('连接已取消')) {
      document.querySelector('.connection-settings').open = true;
      document.querySelector('.logs').open = true;
      throw new Error(`${error.message}。自动复位失败时，按住 BOOT（AI）并复位，松开后选择“已手动进入下载模式”重连。`);
    }
    throw error;
  } finally { connecting = false; connectionStage = ''; }
}));
$('cancel-connect').addEventListener('click', () => {
  if (session.cancelConnect()) { $('cancel-connect').disabled = true; result('正在取消连接并释放串口…'); }
});
$('disconnect').addEventListener('click', () => run(async () => { invalidate(); await session.disconnect(); result('已断开。设备可能仍在下载模式，可手动复位。'); }));
$('restart').addEventListener('click', () => run(async () => {
  resetting = true;
  try { await session.restart(); result('已发送复位并断开连接，请检查设备启动。'); }
  finally { resetting = false; }
}));
$('show-system').addEventListener('change', renderRows);
document.querySelectorAll('[data-preset]').forEach(button => button.addEventListener('click', () => {
  const sets = { app: ['ota_0'], 'app-font': ['ota_0', 'font_data'], initial: ['bootloader', 'partition-table', 'otadata', 'ota_0', 'font_data'] };
  const names = sets[button.dataset.preset];
  if (!names.every(name => rows.some(p => p.name === name))) { result('当前布局不支持此 Metalio 快捷方案，请逐项选择。', 'error'); return; }
  rows.forEach(p => { p.selected = names.includes(p.name); });
  if (button.dataset.preset === 'initial') $('show-system').checked = true;
  clearPreset(); button.classList.add('active'); invalidate(); renderRows();
}));
function replaceLayout(parts, name) {
  if (rows.some(p => p.file) && !window.confirm('更换布局将清除已选文件，不会写入设备。继续？')) return;
  layoutName = name; clearPreset(); setRows(parts);
}
$('default-layout').addEventListener('click', () => replaceLayout(parseCsv(DEFAULT_CSV), 'Metalio 16 MB 预设'));
$('device-layout').addEventListener('click', () => { if (session.device?.partitions) replaceLayout(session.device.partitions.map(p => ({ ...p })), '设备实际布局'); });
$('import-layout').addEventListener('click', () => $('layout-file').click());
$('layout-file').addEventListener('change', () => run(async () => {
  const file = $('layout-file').files?.[0]; $('layout-file').value = ''; if (!file) return;
  if (file.size > 65536 || !file.size) throw new Error('分区表文件为空或超过 64 KiB');
  const size = session.device?.flashSize || FLASH_SIZE;
  const parts = file.name.toLowerCase().endsWith('.csv') ? parseCsv(await file.text(), size) : parseTable(new Uint8Array(await file.arrayBuffer()), size);
  replaceLayout(parts, file.name); result('分区布局已导入。导入只改变页面配置，不会自动刷写分区表。');
}));
$('add-custom').addEventListener('click', () => {
  if (rows.filter(p => p.custom).length >= 8) return result('最多添加 8 个自定义范围。', 'error');
  rows.push({ name: `custom-${++customCount}`, custom: true, offset: '', size: '', selected: true, file: null, flags: 0 });
  invalidate(); clearPreset(); renderRows();
});
$('review').addEventListener('click', () => run(async () => {
  const device = session.device;
  const loaded = await loadRows(rows);
  if (!device || device !== session.device) throw new Error('设备状态已改变，请重新连接');
  const plan = buildPlan(loaded, layout, device);
  pending = { rows: loaded, layout: layout.map(p => ({ ...p })), device, plan };
  $('confirm-device').textContent = `${device.chip} · ${sizeLabel(device.flashSize)} · ${plan.files.length} 个文件 / ${sizeLabel(plan.total)}`;
  $('plan-files').replaceChildren(); $('plan-warnings').replaceChildren();
  for (const p of plan.files) {
    const row = element('div', undefined, 'plan-file');
    row.append(element('strong', `${p.name} ← ${p.fileName}`), element('div', `${address(p.offset)} · 文件 ${sizeLabel(p.data.length)} · 擦除至 ${address(p.offset + p.eraseSize)}（不含）`), element('code', `SHA-256 ${p.sha256}`));
    $('plan-files').append(row);
  }
  plan.warnings.forEach(w => $('plan-warnings').append(element('p', w)));
  $('risk-label').hidden = !plan.dangerous; $('risk-text').value = ''; $('acknowledge').checked = false;
  $('confirm-error').textContent = ''; $('confirmation').showModal();
}));
$('start').addEventListener('click', () => {
  if (busy || !pending) return;
  try {
    confirmPlan(pending.plan, $('acknowledge').checked, $('risk-text').value);
    if (!active() || pending.device !== session.device) throw new Error('连接已改变，请重新检查计划');
  } catch (error) { $('confirm-error').textContent = error.message; return; }
  const job = pending, ack = $('acknowledge').checked, risk = $('risk-text').value, autoRestart = $('auto-restart').checked;
  pending = null; $('confirmation').close();
  run(async () => {
    result('正在写入。请勿拔线、关闭页面或打开其他串口程序。');
    try {
      const plan = await session.flash(job.rows, job.layout, ack, risk, (percent, name) => {
        $('progress').value = percent;
        result(percent === 100 ? '全部文件已通过 MD5 校验。' : `${name} · ${Math.floor(percent)}% · 正在写入 / 校验，请勿断开`);
      });
      log(`完成：${plan.files.length} 个文件写后 MD5 校验通过。`);
      let suffix = '设备仍在下载模式，可点击“重启设备”。';
      if (autoRestart) {
        resetting = true;
        try { await session.restart(); suffix = '已发送复位，请检查设备启动。'; }
        catch { suffix = '数据已校验，但自动复位未确认，请手动复位。'; }
        finally { resetting = false; }
      }
      result(`刷写完成，${plan.files.length} 个文件已校验。${suffix}`, 'success');
    } finally { job.rows.length = 0; }
  });
});
$('confirmation').addEventListener('close', () => { pending = null; });
$('save-log').addEventListener('click', () => download(logs.join('\n'), 'whiteai-flash-log.txt'));
window.addEventListener('beforeunload', event => { if (busy && session.transport) { event.preventDefault(); event.returnValue = ''; } });
navigator.serial?.addEventListener('disconnect', event => {
  const port = event.port || event.target;
  if (port !== session.port || resetting) return;
  session.markLost(port); pending = null;
  if ($('confirmation').open) $('confirmation').close();
  result('USB 设备已断开。若正在刷写，结果不完整，请重新连接恢复。', 'error');
  log('USB 设备断开');
  if (!busy) run(() => session.disconnect());
});
if (!supported) {
  $('support').hidden = false;
  $('support').textContent = '请使用桌面版 Chrome / Edge，并通过 HTTPS 或 localhost 打开。本页不能直接双击 HTML 使用；Safari、Firefox 暂不支持此刷写方式。';
}
setRows(layout);
