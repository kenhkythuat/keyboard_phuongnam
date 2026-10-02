#!/usr/bin/env python3
"""Copy the ESP-IDF application image into OTA/ and write its manifest."""

import argparse
import json
import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_IMAGE = ROOT / "build" / "KEYBOARD_PHUONG_NAM.bin"
OTA_DIR = ROOT / "OTA"
CMAKE_FILE = ROOT / "CMakeLists.txt"
RAW_BASE_URL = (
    "https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam"
)


def project_version() -> str:
    content = CMAKE_FILE.read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([0-9]+(?:\.[0-9]+){1,3})"\)', content)
    if not match:
        raise SystemExit("PROJECT_VER is missing or is not a numeric semantic version")
    return match.group(1)


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
    parser.add_argument("--version", default=project_version())
    parser.add_argument(
        "--branch",
        default=None,
        help="GitHub release branch (default: current Git branch)",
    )
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", args.version):
        raise SystemExit("Version must look like 1.2.3")
    if not BUILD_IMAGE.is_file():
        raise SystemExit(f"Build image not found: {BUILD_IMAGE}")

    branch = validate_branch(args.branch or current_git_branch())
    firmware_url = f"{RAW_BASE_URL}/{branch}/OTA/file.bin"

    OTA_DIR.mkdir(parents=True, exist_ok=True)
    target = OTA_DIR / "file.bin"
    shutil.copyfile(BUILD_IMAGE, target)
    manifest = {
        "version": args.version,
        "firmware_url": firmware_url,
        "size": target.stat().st_size,
    }
    (OTA_DIR / "version.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Prepared {target} ({manifest['size']} bytes), "
        f"version {args.version}, branch {branch}"
    )
    print("Commit and push OTA/file.bin and OTA/version.json together.")


if __name__ == "__main__":
    main()
