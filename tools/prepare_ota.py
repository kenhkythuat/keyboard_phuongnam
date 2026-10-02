#!/usr/bin/env python3
"""Copy the ESP-IDF application image into OTA/ and write its manifest."""

import argparse
import json
import pathlib
import re
import shutil
import struct
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_IMAGE = ROOT / "build" / "KEYBOARD_PHUONG_NAM.bin"
OTA_DIR = ROOT / "OTA"
CMAKE_FILE = ROOT / "CMakeLists.txt"
RAW_BASE_URL = (
    "https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam"
)
ESP_APP_DESC_OFFSET = 0x20
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_APP_VERSION_OFFSET = ESP_APP_DESC_OFFSET + 0x10
ESP_APP_VERSION_LENGTH = 32


def project_version() -> str:
    content = CMAKE_FILE.read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([0-9]+(?:\.[0-9]+){1,3})"\)', content)
    if not match:
        raise SystemExit("PROJECT_VER is missing or is not a numeric semantic version")
    return match.group(1)


def image_version() -> str:
    header_length = ESP_APP_VERSION_OFFSET + ESP_APP_VERSION_LENGTH
    with BUILD_IMAGE.open("rb") as image:
        header = image.read(header_length)
    if len(header) < header_length:
        raise SystemExit(f"Invalid ESP-IDF application image: {BUILD_IMAGE}")

    magic = struct.unpack_from("<I", header, ESP_APP_DESC_OFFSET)[0]
    if magic != ESP_APP_DESC_MAGIC:
        raise SystemExit(
            f"ESP app descriptor not found in build image: {BUILD_IMAGE}"
        )

    raw_version = header[
        ESP_APP_VERSION_OFFSET : ESP_APP_VERSION_OFFSET + ESP_APP_VERSION_LENGTH
    ]
    try:
        version = raw_version.split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise SystemExit("Build image contains an invalid app version") from error
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", version):
        raise SystemExit(f"Invalid app version embedded in build image: {version!r}")
    return version


def current_git_branch() -> str:
    result = subprocess.run(
        ["git", "branch", "--show-current"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    branch = result.stdout.strip()
    if not branch:
        raise SystemExit("Cannot prepare OTA from a detached Git HEAD; pass --branch")
    return branch


def validate_branch(branch: str) -> str:
    if (
        not re.fullmatch(r"[A-Za-z0-9._/-]+", branch)
        or branch.startswith("/")
        or branch.endswith("/")
        or ".." in branch
    ):
        raise SystemExit(f"Invalid Git branch for OTA URL: {branch!r}")
    return branch


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default=None)
    parser.add_argument(
        "--branch",
        default=None,
        help="GitHub release branch (default: current Git branch)",
    )
    args = parser.parse_args()
    if not BUILD_IMAGE.is_file():
        raise SystemExit(f"Build image not found: {BUILD_IMAGE}")

    configured_version = project_version()
    embedded_version = image_version()
    release_version = args.version or configured_version
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", release_version):
        raise SystemExit("Version must look like 1.2.3")
    if configured_version != embedded_version or release_version != embedded_version:
        raise SystemExit(
            "OTA version mismatch: "
            f"PROJECT_VER={configured_version}, image={embedded_version}, "
            f"requested={release_version}. Rebuild firmware before packaging OTA."
        )

    branch = validate_branch(args.branch or current_git_branch())
    firmware_url = f"{RAW_BASE_URL}/{branch}/OTA/file.bin"

    OTA_DIR.mkdir(parents=True, exist_ok=True)
    target = OTA_DIR / "file.bin"
    shutil.copyfile(BUILD_IMAGE, target)
    manifest = {
        "version": embedded_version,
        "firmware_url": firmware_url,
        "size": target.stat().st_size,
    }
    (OTA_DIR / "version.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Prepared {target} ({manifest['size']} bytes), "
        f"version {embedded_version}, branch {branch}"
    )
    print("Commit and push OTA/file.bin and OTA/version.json together.")


if __name__ == "__main__":
    main()
