// Fixed release, not "latest". Runtime library is loaded only after Connect.
// Serve a vetted copy as vendor/esptool-js-0.7.0.bundle.js for offline use.
// Local library failure does not silently fall back to a remote dependency.
export const SDK_VERSION = '0.7.0';
let pending;
export function loadSdk() {
  pending ??= (async () => {
    const response = await fetch('./vendor/esptool-js-0.7.0.bundle.js', { method: 'HEAD' });
    if (response.ok) {
      if (!/(?:javascript|ecmascript)/i.test(response.headers.get('content-type') || '')) throw new Error('Local SDK MIME type is invalid');
      return import('./vendor/esptool-js-0.7.0.bundle.js');
    }
    return import('https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js');
  })().catch(() => {
    pending = undefined;
    throw new Error('刷机库加载失败。请确认网络可访问 jsDelivr，或按 README 放置本地 esptool-js 0.7.0。');
  });
  return pending;
}
