"""Build an upload-ready Thunderstore package from the release DLL."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
PACKAGE_NAME = re.compile(r"^[A-Za-z0-9_]+$")
REQUIRED_MANIFEST_FIELDS = {
    "name",
    "version_number",
    "website_url",
    "description",
    "dependencies",
}


def read_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    missing = REQUIRED_MANIFEST_FIELDS - manifest.keys()
    if missing:
        raise ValueError(f"manifest is missing: {', '.join(sorted(missing))}")
    if not isinstance(manifest["name"], str) or not PACKAGE_NAME.fullmatch(manifest["name"]):
        raise ValueError("manifest name must contain only letters, digits, and underscores")
    if not isinstance(manifest["version_number"], str) or not SEMVER.fullmatch(
        manifest["version_number"]
    ):
        raise ValueError("manifest version_number must be Major.Minor.Patch")
    if not isinstance(manifest["dependencies"], list) or not all(
        isinstance(dependency, str) for dependency in manifest["dependencies"]
    ):
        raise ValueError("manifest dependencies must be a list of strings")
    return manifest


def validate_icon(path: Path) -> None:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or len(data) < 24:
        raise ValueError("icon.png is not a valid PNG")
    width, height = struct.unpack(">II", data[16:24])
    if (width, height) != (256, 256):
        raise ValueError(f"icon.png must be 256x256, got {width}x{height}")


def build_package(root: Path, output_directory: Path) -> Path:
    manifest_path = root / "manifest.json"
    manifest = read_manifest(manifest_path)
    validate_icon(root / "icon.png")

    dll_path = root / "build" / "windows" / "x64" / "release" / "lua_hot_reload.dll"
    if not dll_path.is_file():
        raise FileNotFoundError(f"release DLL not found: {dll_path}")
    readme_path = root / "README.md"
    if not readme_path.is_file():
        raise FileNotFoundError(f"README not found: {readme_path}")

    output_directory.mkdir(parents=True, exist_ok=True)
    version = manifest["version_number"]
    output_path = output_directory / f"Lua_Hot_Reload-{version}.zip"
    with ZipFile(output_path, "w", ZIP_DEFLATED) as package:
        package.write(manifest_path, "manifest.json")
        package.write(root / "icon.png", "icon.png")
        package.write(readme_path, "README.md")
        package.write(dll_path, "lua_hot_reload.dll")

    with ZipFile(output_path) as package:
        expected = {"manifest.json", "icon.png", "README.md", "lua_hot_reload.dll"}
        actual = set(package.namelist())
        if actual != expected:
            raise ValueError(f"package contents differ: {sorted(actual)}")
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, default=None)
    arguments = parser.parse_args()
    output_directory = arguments.output or arguments.root / "artifacts"
    print(build_package(arguments.root, output_directory))


if __name__ == "__main__":
    main()
