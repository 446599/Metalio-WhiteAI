#!/usr/bin/env python3
"""Validate IDF flash artifacts and write a hash-bound, read-only delivery manifest.

This tool never opens a serial port or writes device flash. Optional readback
files let an operator verify the installed layout before app/font updates and
verify a font readback against the exact build artifact.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def flash_pairs(path):
    lines = path.read_text().splitlines()
    pairs = []
    for line in lines[1:]:
        if not line.strip():
            continue
        values = shlex.split(line)
        require(len(values) == 2, f"Unexpected flash arguments in {path.name}")
        pairs.append((int(values[0], 0), values[1]))
    return pairs


def validate(build, device_table=None, font_readback=None):
    project = json.loads((build / "project_description.json").read_text())
    flash = json.loads((build / "flasher_args.json").read_text())
    config = json.loads((build / "config/sdkconfig.json").read_text())
    root = Path(project["project_path"])
    partition_tool = Path(project["idf_path"]) / "components/partition_table/gen_esp32part.py"
    spec = importlib.util.spec_from_file_location("delivery_partitions", partition_tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    table_offset = int(flash["partition-table"]["offset"], 0)
    module.offset_part_table = table_offset
    table_path = build / flash["partition-table"]["file"]
    table = module.PartitionTable.from_binary(table_path.read_bytes())
    table.verify()
    flash_bytes = int(flash["flash_settings"]["flash_size"].removesuffix("MB")) * 1024 * 1024
    table.verify_size_fits(flash_bytes)

    font = table.find_by_name("font_data")
    require(font is not None and font.type == 1 and font.subtype == 0x40,
            "Expected font_data data/0x40 partition is missing")
    require("font_data" in flash, "Standard flash manifest omits font_data")
    font_entry = flash["font_data"]
    require(int(font_entry["offset"], 0) == font.offset, "Font flash address differs from partition table")
    font_path = build / font_entry["file"]
    font_report = json.loads((root / "assets/fonts/harmony_full.json").read_text())
    font_blob = font_path.read_bytes()
    require(len(font_blob) == font_report["bytes"] <= font.size, "Font size mismatch or partition overflow")
    require(sha256(font_path) == font_report["sha256"], "Font SHA-256 differs from source report")
    magic, font_version, _, glyphs = struct.unpack_from("<4sHHI", font_blob)
    require(magic == b"EFNT" and font_version == 1 and glyphs == font_report["glyphs"],
            "Invalid font header/version/glyph count")

    expected_pairs = [(int(offset, 0), name) for offset, name in flash["flash_files"].items()]
    require(flash_pairs(build / "flash_args") == expected_pairs, "flash_args and flasher_args.json disagree")
    app_pair = (int(flash["app"]["offset"], 0), flash["app"]["file"])
    require(flash_pairs(build / "app-flash_args") == [app_pair],
            "app-flash must write only the application")
    require(flash_pairs(build / "font-flash_args") == [(font.offset, font_entry["file"])],
            "font-flash must write only the font partition")

    images = []
    for offset, name in expected_pairs:
        path = build / name
        size = path.stat().st_size
        require(size > 0 and offset + size <= flash_bytes, f"Image {name} outside flash bounds")
        if name == flash["bootloader"]["file"]:
            require(offset + size <= table_offset, "Bootloader overlaps partition table")
            partition_name = "bootloader"
        elif name == flash["partition-table"]["file"]:
            require(offset == table_offset and size <= 0x1000, "Invalid partition table image bounds")
            partition_name = "partition-table"
        else:
            partition = next((item for item in table if item.offset == offset), None)
            require(partition is not None and size <= partition.size, f"Image {name} does not fit its partition")
            require(not (partition.type == 1 and partition.subtype in (2, 4)),
                    "Delivery must not overwrite NVS or NVS keys")
            partition_name = partition.name
        images.append({"file": name, "offset": hex(offset), "bytes": size,
                       "partition": partition_name, "sha256": sha256(path)})
    ordered = sorted(images, key=lambda image: int(image["offset"], 0))
    for previous, current in zip(ordered, ordered[1:]):
        require(int(previous["offset"], 0) + previous["bytes"] <= int(current["offset"], 0),
                "Flash images overlap")

    if device_table:
        installed = module.PartitionTable.from_binary(device_table.read_bytes())
        # read-flash returns a full sector, whereas IDF's table binary is 3072 B.
        require(installed.to_binary() == table.to_binary(),
                "Installed partition layout differs: stop app/font update and plan migration")
    if font_readback:
        require(font_readback.read_bytes() == font_blob, "Font readback does not match build SHA-256/length")

    return {
        "schema_version": 1,
        "project": project["project_name"],
        "project_version": project["project_version"],
        "idf_version": project["git_revision"],
        "chip": project["target"],
        "flash_bytes": flash_bytes,
        "partition_table_sha256": sha256(table_path),
        "dependency_lock_sha256": sha256(root / "dependencies.lock"),
        "sdkconfig_sha256": sha256(Path(project["config_file"])),
        "flash_encryption_enabled": bool(config.get("SECURE_FLASH_ENC_ENABLED", False)),
        "font": {"format_version": font_version, "glyphs": glyphs, "bytes": len(font_blob),
                 "offset": hex(font.offset), "partition_bytes": font.size, "sha256": font_report["sha256"]},
        "images": images,
        "workflows": {
            "first_deployment": {"target": "flash", "args": "flash_args", "resets_ota_selection": True},
            "application_update": {"target": "app-flash", "args": "app-flash_args",
                                   "requires_matching_device_partition_table": True},
            "font_update": {"target": "font-flash", "args": "font-flash_args",
                            "requires_matching_device_partition_table": True},
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--device-partition-table", type=Path)
    parser.add_argument("--font-readback", type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    try:
        manifest = validate(build, args.device_partition_table, args.font_readback)
        output = build / "delivery_manifest.json"
        output.write_text(json.dumps(manifest, indent=2) + "\n")
    except (ValueError, OSError, KeyError, struct.error) as error:
        print(f"Build delivery FAILED: {error}", file=sys.stderr)
        return 1
    print(f"Build delivery OK: {len(manifest['images'])} images; font {manifest['font']['glyphs']} glyphs; {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
