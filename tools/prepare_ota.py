#!/usr/bin/env python3
"""Copy the ESP-IDF application image into OTA/ and write its manifest."""

import argparse
import json
import pathlib
import re
import shutil


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_IMAGE = ROOT / "build" / "KEYBOARD_PHUONG_NAM.bin"
OTA_DIR = ROOT / "OTA"
CMAKE_FILE = ROOT / "CMakeLists.txt"
FIRMWARE_URL = (
    "https://raw.githubusercontent.com/kenhkythuat/"
    "keyboard_phuongnam/main/OTA/file.bin"
)


def project_version() -> str:
    content = CMAKE_FILE.read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([0-9]+(?:\.[0-9]+){1,3})"\)', content)
    if not match:
        raise SystemExit("PROJECT_VER is missing or is not a numeric semantic version")
    return match.group(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default=project_version())
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", args.version):
        raise SystemExit("Version must look like 1.2.3")
    if not BUILD_IMAGE.is_file():
        raise SystemExit(f"Build image not found: {BUILD_IMAGE}")

    OTA_DIR.mkdir(parents=True, exist_ok=True)
    target = OTA_DIR / "file.bin"
    shutil.copyfile(BUILD_IMAGE, target)
    manifest = {
        "version": args.version,
        "firmware_url": FIRMWARE_URL,
        "size": target.stat().st_size,
    }
    (OTA_DIR / "version.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Prepared {target} ({manifest['size']} bytes), version {args.version}")


if __name__ == "__main__":
    main()
