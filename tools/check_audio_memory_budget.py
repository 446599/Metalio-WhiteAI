#!/usr/bin/env python3
"""Fail product builds when linked BLE or shared internal RAM consumes audio headroom.

Uses binutils on the final ELF, not Flash image size. This is a static guard,
not proof of runtime contiguous heap or an on-device TTS/stack test.
"""
from __future__ import annotations
import argparse
import json
import re
import subprocess
from pathlib import Path

# The handoff reports 146155 static bytes in 093e9df and 166455 in 57df420.
# Leave a small, explicit margin for UI/diagnostics, not another BLE controller.
MAX_IRAM_BYTES = 95 * 1024
MAX_INTERNAL_STATIC_BYTES = 148 * 1024
BLE_ENTRY_POINTS = frozenset({
    'nimble_port_init', 'nimble_port_run', 'ble_gap_disc',
    'ble_hs_start', 'esp_bt_controller_init',
})

def sections_from_size(text: str) -> list[tuple[str, int, int]]:
    sections = []
    seen = set()
    for line in text.splitlines():
        m = re.fullmatch(r'\s*(\.[\w.]+)\s+(\d+|0[xX][0-9a-fA-F]+)\s+(\d+|0[xX][0-9a-fA-F]+)\s*', line)
        if not m:
            continue
        name, size, address = m.groups()
        if name in seen:
            raise ValueError(f'duplicate ELF section {name}')
        seen.add(name)
        sections.append((name, int(size, 16 if size.lower().startswith('0x') else 10),
                         int(address, 16 if address.lower().startswith('0x') else 10)))
    if not sections:
        raise ValueError('no ELF sections parsed from size -A')
    return sections

def inspect(sections_text: str, symbols_text: str) -> dict:
    sections = sections_from_size(sections_text)
    iram, dram = [], []
    for name, size, address in sections:
        # Linker alias padding reserves the DRAM view of IRAM already counted
        # above. Counting .dram0.dummy again would double-charge shared SRAM.
        if name == ".dram0.dummy":
            continue
        if 0x40370000 <= address < 0x40400000:
            if address + size > 0x40400000:
                raise ValueError('IRAM section overflows address region')
            iram.append((name, size))
        elif 0x3FC80000 <= address < 0x3FD00000:
            if address + size > 0x3FD00000:
                raise ValueError('DRAM section overflows address region')
            dram.append((name, size))
    if not iram or not dram:
        raise ValueError('expected ESP32-S3 IRAM and DRAM sections; wrong or incomplete ELF')
    # nm -P --defined-only: symbol, type, value, optional size.
    symbols = {line.split()[0] for line in symbols_text.splitlines() if len(line.split()) >= 3}
    if not symbols:
        raise ValueError('no defined ELF symbols parsed')
    forbidden = sorted(symbols & BLE_ENTRY_POINTS)
    iram_bytes = sum(size for _, size in iram)
    dram_bytes = sum(size for _, size in dram)
    errors = []
    if forbidden:
        errors.append('product links BLE controller entry points: ' + ', '.join(forbidden))
    if iram_bytes > MAX_IRAM_BYTES:
        errors.append(f'IRAM {iram_bytes} exceeds {MAX_IRAM_BYTES} bytes')
    if iram_bytes + dram_bytes > MAX_INTERNAL_STATIC_BYTES:
        errors.append(f'shared internal static {iram_bytes + dram_bytes} exceeds {MAX_INTERNAL_STATIC_BYTES} bytes')
    return dict(iram_bytes=iram_bytes, dram_bytes=dram_bytes,
                internal_static_bytes=iram_bytes + dram_bytes,
                iram_sections=dict(iram), dram_sections=dict(dram),
                forbidden_ble_symbols=forbidden, errors=errors,
                limits=dict(iram=MAX_IRAM_BYTES, internal_static=MAX_INTERNAL_STATIC_BYTES),
                runtime_audio_validated=False)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--sdkconfig', type=Path, required=True)
    parser.add_argument('--tool-prefix', default='xtensa-esp32s3-elf-')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if not args.elf.is_file() or not args.sdkconfig.is_file():
        parser.error('ELF and sdkconfig must both exist')
    try:
        config = args.sdkconfig.read_text()
        sizes = subprocess.check_output([args.tool_prefix + 'size', '-A', str(args.elf)], text=True)
        symbols = subprocess.check_output([args.tool_prefix + 'nm', '-P', '--defined-only', str(args.elf)], text=True)
        report = inspect(sizes, symbols)
        if re.search(r'^CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY=y$', config, re.M):
            report['errors'].append('experimental BLE build is not an audio-safe product build')
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        parser.exit(1, f'audio memory budget unavailable: {exc}\n')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if report['errors']:
        print('Audio memory budget FAILED (do not bypass by shrinking unmeasured stacks)')
        return 1
    print('Audio memory budget OK (runtime largest block and audible TTS still require hardware)')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
