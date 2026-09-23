// Fixed, self-hosted release first; no unbounded network import after selection.
import { bounded } from './serial-port.mjs';
export const SDK_VERSION = '0.7.0';
let pending;
export function loadSdk() {
  if (pending) return pending;
  const attempt = (async () => {
    const controller = new AbortController();
    let response;
    try {
      response = await bounded(() => fetch('./vendor/esptool-js-0.7.0.bundle.js',
        { method: 'HEAD', signal: controller.signal }), 8000, '检查本地刷机库');
    } finally { controller.abort(); }
    let module;
    if (response.ok) {
      if (!/(?:javascript|ecmascript)/i.test(response.headers.get('content-type') || '')) throw new Error('本地刷机库类型错误，请重新部署网页');
      module = await bounded(() => import('./vendor/esptool-js-0.7.0.bundle.js'), 12000, '加载本地刷机库');
    } else if (response.status === 404) {
      module = await bounded(() => import('https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js'), 12000, '下载刷机库');
    } else { throw new Error(`本地刷机库请求失败（HTTP ${response.status}）`); }
    if (typeof module.ESPLoader !== 'function' || typeof module.Transport !== 'function') throw new Error('刷机库导出不完整');
    return module;
  })();
  pending = attempt.catch(error => {
    pending = undefined;
    throw new Error(`刷机库加载失败：${error.message}。请检查网络或按 README 安装本地 SDK。`);
  });
  return pending;
}
