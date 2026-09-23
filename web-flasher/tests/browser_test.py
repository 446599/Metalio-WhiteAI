#!/usr/bin/env python3
"""Browser interaction tests with a mocked esptool transport, never real USB.
Requires Python Playwright and Chromium. No application dependencies / CDN needed.
"""
from __future__ import annotations
import argparse
import base64
import hashlib
import re
import uuid
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
SDK = """
import { mockSdk } from './tests/fixtures.mjs';
const mock = mockSdk(); window.__mock = mock;
export const SDK_VERSION = '0.7.0';
export async function loadSdk() { if (window.__holdSdk) await new Promise(resolve => { window.__releaseSdk=resolve; }); return mock.sdk; }
"""
SERIAL = """
window.__port = {getInfo: () => ({usbVendorId: 0x303a, usbProductId: 0x1001})};
const serial = new EventTarget(); serial.requestPort = async () => window.__port;
Object.defineProperty(navigator, 'serial', {configurable: true, value: serial});
"""
class Handler(SimpleHTTPRequestHandler):
    def log_message(self, *_): pass

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--chromium', default='/usr/bin/chromium')
    parser.add_argument('--screenshots', type=Path)
    parser.add_argument('--in-memory', action='store_true', help='Render owned sources through data modules; no browser networking or real downloads')
    args = parser.parse_args()
    http = ThreadingHTTPServer(('127.0.0.1', 0), partial(Handler, directory=str(ROOT)))
    thread = threading.Thread(target=http.serve_forever, daemon=True); thread.start()
    url = f'http://127.0.0.1:{http.server_port}/'
    digest_pages=set()
    def load_page(page, serial=True):
        if not args.in_memory:
            page.goto(url)
            return
        # This test-only adapter respects environments which block ALL browser
        # navigation. The production files and CSP are not changed. Web Serial,
        # secure-context detection and OS downloads are simulated, not tested.
        page.goto('about:blank')
        html = (ROOT/'index.html').read_text()
        html = re.sub(r'<meta http-equiv="Content-Security-Policy"[^>]+>', '', html)
        html = re.sub(r'<script type="module"[^>]+></script>', '', html)
        html = re.sub(r'<link rel="stylesheet"[^>]+>', '', html)
        page.set_content(html)
        page.add_style_tag(content=(ROOT/'style.css').read_text())
        page.evaluate("Object.defineProperty(window,'isSecureContext',{configurable:true,value:true})")
        if page not in digest_pages:
            page.expose_function('__test_sha256', lambda values: list(hashlib.sha256(bytes(values)).digest()))
            digest_pages.add(page)
        page.evaluate("if(!crypto.subtle) Object.defineProperty(crypto,'subtle',{value:{digest:async (_algorithm,bytes)=>new Uint8Array(await window.__test_sha256(Array.from(bytes))).buffer}})")
        page.evaluate(SERIAL if serial else "Object.defineProperty(navigator,'serial',{configurable:true,value:undefined})")
        page.evaluate("""() => {
            window.__downloads=[]; const blobs=new Map();
            const create=URL.createObjectURL.bind(URL);
            URL.createObjectURL=blob=>{const url=create(blob);blobs.set(url,blob);return url;};
            HTMLAnchorElement.prototype.click=function(){
                const blob=blobs.get(this.href);
                if(blob) window.__downloads.push({name:this.download,size:blob.size});
            };
        }""")
        cache={}; nonce=uuid.uuid4().hex
        def module(path):
            path=path.resolve()
            if path in cache:return cache[path]
            source=SDK if path==ROOT/'sdk.mjs' else path.read_text()
            if path.name in ('sdk.mjs','app.mjs'):source+='\n// '+nonce
            def dependency(match):
                return match[1] + module(path.parent/match[2]) + match[3]
            source=re.sub(r"(from ['\"])(\.[^'\"]+)(['\"])",dependency,source)
            cache[path]='data:text/javascript;base64,'+base64.b64encode(source.encode()).decode()
            return cache[path]
        page.evaluate('(url)=>import(url).then(()=>true)',module(ROOT/'app.mjs'))
    checks = []
    def passed(label): checks.append(label); print(f'PASS {label}')
    firmware = bytearray(128); firmware[0] = 0xe9; firmware[1] = 1; firmware[12] = 9
    def file(name='app.bin', data=firmware): return {'name': name, 'mimeType': 'application/octet-stream', 'buffer': bytes(data)}
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=args.chromium, headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context(viewport={'width': 1280, 'height': 1160}, device_scale_factor=1)
        context.add_init_script(SERIAL)
        context.route('**/sdk.mjs', lambda route: route.fulfill(status=200, body=SDK, content_type='text/javascript'))
        errors = []
        page = context.new_page(); page.on('pageerror', lambda err: errors.append(str(err)))
        load_page(page); expect(page.locator('#partitions tr')).to_have_count(3)
        expect(page.get_by_role('button', name='检查刷写计划')).to_be_disabled()
        expect(page.locator('#support')).to_be_hidden(); passed('default app-only selection and offline controls')
        if args.screenshots:
            args.screenshots.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(args.screenshots/'whiteai-flasher-desktop.png'), full_page=True)
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#connection-state')).to_have_text('已连接')
        expect(page.locator('#layout-status')).to_contain_text('与设备分区表一致')
        passed('connect: chip, security, flash and partition checks')
        page.get_by_label('选择 ota_0 文件', exact=True).set_input_files(file())
        page.get_by_role('button', name='检查刷写计划').click()
        expect(page.locator('#confirmation')).to_be_visible(); expect(page.locator('#plan-files')).to_contain_text('SHA-256')
        page.get_by_role('button', name='开始刷写').click(); expect(page.locator('#confirm-error')).to_contain_text('请确认')
        assert page.evaluate('window.__mock.calls.writes.length') == 0; passed('review and acknowledgement are mandatory')
        page.locator('#acknowledge').check(); page.get_by_role('button', name='开始刷写').click()
        expect(page.locator('#result')).to_contain_text('刷写完成')
        assert page.evaluate('window.__mock.calls.writes[0].fileArray.map(f => f.address)') == [0x80000]
        assert page.evaluate('window.__mock.calls.writes[0].eraseAll') is False
        expect(page.locator('#connection-state')).to_have_text('未连接'); passed('app-only flash, verified completion and automatic reset')
        page.get_by_role('button', name='连接设备').click(); expect(page.locator('#connection-state')).to_have_text('已连接')
        page.locator('#show-system').check(); expect(page.locator('#partitions tr')).to_have_count(11)
        page.get_by_label('刷写 ota_0', exact=True).uncheck(); page.get_by_label('选择 nvs 文件', exact=True).set_input_files(file('nvs.bin', bytes(32)))
        page.get_by_role('button', name='检查刷写计划').click(); expect(page.locator('#confirmation')).to_be_visible(); page.locator('#acknowledge').check()
        page.get_by_role('button', name='开始刷写').click(); expect(page.locator('#confirm-error')).to_contain_text('FLASH')
        assert page.evaluate('window.__mock.calls.writes.length') == 1
        page.locator('#risk-text').fill('FLASH'); page.get_by_role('button', name='开始刷写').click()
        expect(page.locator('#result')).to_contain_text('刷写完成'); passed('NVS protected by additional typed confirmation')
        page.get_by_role('button', name='连接设备').click(); expect(page.locator('#connection-state')).to_have_text('已连接')
        page.get_by_role('button', name='仅应用', exact=False).click()
        bad = bytearray(firmware); bad[12] = 0
        page.get_by_label('选择 ota_0 文件', exact=True).set_input_files(file('wrong-chip.bin', bad))
        page.get_by_role('button', name='检查刷写计划').click(); expect(page.locator('#result')).to_contain_text('不是 ESP32-S3')
        assert page.evaluate('window.__mock.calls.writes.length') == 2; passed('wrong-chip image cannot reach writer')
        page.get_by_label('选择 ota_0 文件', exact=True).set_input_files(file('oversize.bin', bytes(5*1048576+1)))
        page.get_by_role('button', name='检查刷写计划').click(); expect(page.locator('#result')).to_contain_text('超过分区容量')
        passed('oversized local file blocked before writing')
        page.get_by_label('选择 ota_0 文件', exact=True).set_input_files(file('<img src=x onerror=alert(1)>.bin'))
        assert page.locator('img').count() == 0
        page.get_by_role('button', name='检查刷写计划').click()
        expect(page.locator('#plan-files')).to_contain_text('<img src=x onerror=alert(1)>.bin')
        assert page.locator('img').count() == 0; passed('filenames treated as text, not HTML')
        page.evaluate('window.__mock.memory[0x8000] ^= 1')
        page.locator('#acknowledge').check(); page.get_by_role('button', name='开始刷写').click()
        expect(page.locator('#result')).to_contain_text('确认后改变'); assert page.evaluate('window.__mock.calls.writes.length') == 2
        page.evaluate('window.__mock.memory[0x8000] ^= 1'); passed('table mutation between review and write refused')
        if args.in_memory:
            page.get_by_role('button', name='备份 nvs', exact=True).click()
            page.wait_for_function('window.__downloads.length > 0')
            saved=page.evaluate('window.__downloads.at(-1)')
            assert 'nvs' in saved['name'] and saved['size']==16384
        else:
            with page.expect_download() as downloaded:
                page.get_by_role('button', name='备份 nvs', exact=True).click()
            assert 'nvs' in downloaded.value.suggested_filename
            assert Path(downloaded.value.path()).stat().st_size == 16384
        assert page.evaluate('window.__mock.calls.writes.length') == 2; passed('partition backup is a local download with no writes')
        page.get_by_role('button', name='首次部署', exact=False).click()
        assert page.locator('#partitions input[type=checkbox]:checked').count() == 5
        assert not page.get_by_label('刷写 nvs', exact=True).is_checked(); passed('first-install preset selects 5 targets, excludes NVS')
        page.get_by_role('button', name='＋ 自定义地址').click()
        expect(page.get_by_label('custom-1 起始地址')).to_be_visible()
        page.get_by_label('custom-1 起始地址').fill('0x74000'); page.get_by_label('custom-1 目标容量').fill('4K')
        page.locator('tr[data-target="custom-1"] button', has_text='移除').click()
        assert page.locator('tr[data-target="custom-1"]').count() == 0; passed('custom address row can be added, edited and removed')
        page.on('dialog', lambda dialog: dialog.accept())
        csv = b'# imported\nnvs,data,nvs,0x9000,16K,\nota_0,app,ota_0,0x10000,1M,\n'
        page.locator('#layout-file').set_input_files(file('other.csv', csv))
        expect(page.locator('#layout-status')).to_contain_text('与设备布局不同')
        page.get_by_label('选择 ota_0 文件', exact=True).set_input_files(file())
        page.get_by_role('button', name='检查刷写计划').click(); expect(page.locator('#result')).to_contain_text('与设备分区表不一致')
        passed('imported layout never silently overwrites different device layout')
        page.evaluate("const event = new Event('disconnect'); Object.defineProperty(event,'port',{value:window.__port}); navigator.serial.dispatchEvent(event);")
        expect(page.locator('#connection-state')).to_have_text('未连接'); expect(page.get_by_role('button', name='连接设备')).to_be_enabled()
        passed('native disconnect releases stale port and re-enables connection')
        load_page(page); page.set_viewport_size({'width': 390, 'height': 1000})
        if args.screenshots: page.screenshot(path=str(args.screenshots/'whiteai-flasher-mobile.png'), full_page=True)
        if not page.evaluate('document.documentElement.scrollWidth <= innerWidth'):
            print(page.evaluate("Array.from(document.querySelectorAll('*')).filter(e=>e.getBoundingClientRect().right>innerWidth).map(e=>[e.tagName,e.id,e.className,e.getBoundingClientRect().right]).slice(0,25)"))
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        if args.screenshots: page.screenshot(path=str(args.screenshots/'whiteai-flasher-mobile.png'), full_page=True)
        passed('390px layout has no document-level horizontal overflow')
        no_serial = browser.new_context(viewport={'width': 1280, 'height': 900})
        no_serial.add_init_script("Object.defineProperty(navigator,'serial',{configurable:true,value:undefined});")
        no_serial.route('**/sdk.mjs', lambda route: route.fulfill(status=200, body=SDK, content_type='text/javascript'))
        unsupported = no_serial.new_page(); load_page(unsupported, serial=False)
        expect(unsupported.locator('#support')).to_be_visible(); expect(unsupported.get_by_role('button', name='连接设备')).to_be_disabled()
        passed('unsupported browser displays instructions without a broken page')
        # Regression: UI stays cancellable while the port or SDK is silent.
        load_page(page)
        page.evaluate("window.__holdSdk = true")
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#connection-state')).to_have_text('加载刷机库')
        expect(page.locator('#cancel-connect')).to_be_enabled()
        page.locator('#cancel-connect').click()
        expect(page.locator('#result')).to_contain_text('连接已取消')
        expect(page.get_by_role('button', name='连接设备')).to_be_enabled()
        page.evaluate("window.__holdSdk=false; window.__releaseSdk()")
        assert page.evaluate('window.__mock.calls.options') is None
        passed('cancel SDK loading; late module cannot open a stale port')
        page.evaluate("window.__originalMain=window.__mock.sdk.ESPLoader.prototype.main; window.__mock.sdk.ESPLoader.prototype.main=()=>new Promise(resolve=>{window.__releaseMain=resolve}); void 0")
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#connection-state')).to_have_text('下载模式握手')
        expect(page.locator('#cancel-connect')).to_be_enabled()
        expect(page.locator('#save-log')).to_be_enabled()
        expect(page.locator('#review')).to_be_disabled()
        page.locator('#cancel-connect').click()
        expect(page.locator('#result')).to_contain_text('连接已取消')
        page.evaluate("window.__releaseMain(); window.__mock.sdk.ESPLoader.prototype.main=window.__originalMain; void 0")
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#connection-state')).to_have_text('已连接')
        assert page.evaluate('window.__mock.calls.writes.length') == 0
        passed('cancel handshake, retain diagnostics, reconnect without writing Flash')
        page.get_by_role('button', name='断开', exact=True).click()
        page.evaluate("window.__mock.sdk.ESPLoader.prototype.main=()=>new Promise(resolve=>{window.__releaseMain=resolve}); void 0")
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#connection-state')).to_have_text('下载模式握手')
        page.evaluate("const e=new Event('disconnect');Object.defineProperty(e,'port',{value:window.__port});navigator.serial.dispatchEvent(e)")
        expect(page.locator('#result')).to_contain_text('USB 已断开')
        expect(page.get_by_role('button', name='连接设备')).to_be_enabled()
        page.evaluate('window.__releaseMain()')
        expect(page.locator('#review')).to_be_disabled()
        passed('unplug during connect releases UI; late success never enables flashing')
        load_page(page)
        page.evaluate("navigator.serial.requestPort=async()=>{throw new DOMException('No port','NotFoundError')}; void 0")
        page.get_by_role('button', name='连接设备').click()
        expect(page.locator('#result')).to_have_text('未选择串口。')
        expect(page.get_by_role('button', name='连接设备')).to_be_enabled()
        assert page.locator('.download-card').count() == 6
        passed('native picker cancellation and all latest firmware links preserved')
        assert not errors, errors
        passed('no uncaught page errors throughout browser flow')
        browser.close()
    http.shutdown(); http.server_close()
    print(f'Browser tests: {len(checks)} flows passed (MOCK serial; not hardware validation).')

if __name__ == '__main__': main()
