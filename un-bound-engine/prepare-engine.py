#!/usr/bin/env python3
"""Prepare Chromium and its ARM64 Ubuntu libraries on macOS or Linux."""

import gzip
import hashlib
import importlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request


CHROMIUM_URL = "https://cdn.playwright.dev/builds/chromium/1140/chromium-linux-arm64.zip"
UBUNTU_BASE = "https://ports.ubuntu.com/ubuntu-ports/"
INDEXES = [
    "dists/jammy/main/binary-arm64/Packages.gz",
    "dists/jammy/universe/binary-arm64/Packages.gz",
    "dists/jammy-updates/main/binary-arm64/Packages.gz",
    "dists/jammy-updates/universe/binary-arm64/Packages.gz",
    "dists/jammy-security/main/binary-arm64/Packages.gz",
    "dists/jammy-security/universe/binary-arm64/Packages.gz",
]
PACKAGES = [
    "libnss3", "libnspr4", "libsqlite3-0",
    "libatk1.0-0", "libatk-bridge2.0-0", "libatspi2.0-0",
    "libx11-6", "libxcomposite1", "libxdamage1", "libxext6", "libxfixes3",
    "libxrandr2", "libxrender1", "libxi6", "libxau6", "libxdmcp6",
    "libxcb1", "libxcb-randr0", "libxcb-dri3-0", "libxcb-present0",
    "libxcb-sync1", "libxcb-xfixes0", "libxcb-shm0", "libxcb-render0",
    "libxcb-shape0", "libasound2", "libcups2", "libgbm1", "libdrm2",
    "libxkbcommon0", "libpango-1.0-0", "libharfbuzz0b", "libgraphite2-3",
    "libfribidi0", "libthai0", "libdatrie1", "libgssapi-krb5-2", "libkrb5-3",
    "libk5crypto3", "libcom-err2", "libkrb5support0", "libkeyutils1",
    "libavahi-common3", "libavahi-client3", "libgnutls30", "libp11-kit0",
    "libidn2-0", "libunistring2", "libtasn1-6", "libnettle8", "libhogweed6",
    "libgmp10", "libwayland-server0", "libwayland-client0", "libffi8",
    "libbsd0", "libmd0",
]


def download(url: str, destination: Path, expected_sha256: str = "") -> None:
    if destination.exists() and (not expected_sha256 or sha256(destination) == expected_sha256):
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    print(f"Downloading {destination.name}")
    with urllib.request.urlopen(url) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output)
    if expected_sha256 and sha256(temporary) != expected_sha256:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"checksum mismatch for {url}")
    temporary.replace(destination)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_index(data: bytes, selected: dict) -> None:
    for stanza in gzip.decompress(data).decode("utf-8").split("\n\n"):
        fields = {}
        for line in stanza.splitlines():
            if ": " in line:
                key, value = line.split(": ", 1)
                fields[key] = value
        name = fields.get("Package")
        if name in PACKAGES and fields.get("Architecture") == "arm64":
            selected[name] = fields


def copy_library(source: Path, destination: Path) -> None:
    target = destination / source.name
    if target.exists() or target.is_symlink():
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        else:
            target.unlink()
    if source.is_symlink():
        target.symlink_to(os.readlink(source))
    elif source.is_dir():
        shutil.copytree(source, target, symlinks=True)
    else:
        shutil.copy2(source, target)


def extract_data_archive(archive: Path, root: Path, work: Path) -> None:
    if archive.suffix != ".zst":
        subprocess.run(["tar", "-xf", str(archive), "-C", str(root)], check=True)
        return

    # macOS bsdtar delegates Zstandard to a separate executable that is not
    # installed by default. Keep the fallback private to this bootstrap.
    modules = work / "python"
    sys.path.insert(0, str(modules))
    try:
        zstandard = importlib.import_module("zstandard")
    except ImportError:
        print("Installing the isolated Zstandard extraction helper")
        subprocess.run([
            sys.executable, "-m", "pip", "install",
            "--disable-pip-version-check", "--target", str(modules),
            "zstandard==0.23.0",
        ], check=True)
        importlib.invalidate_caches()
        zstandard = importlib.import_module("zstandard")

    uncompressed = archive.with_suffix("")
    with archive.open("rb") as source, uncompressed.open("wb") as destination:
        zstandard.ZstdDecompressor().copy_stream(source, destination)
    subprocess.run(["tar", "-xf", str(uncompressed), "-C", str(root)], check=True)


def main() -> int:
    here = Path(__file__).resolve().parent
    bundle = here / "bundle"
    work = here / ".work"
    debs = work / "debs"
    bundle.mkdir(parents=True, exist_ok=True)
    debs.mkdir(parents=True, exist_ok=True)

    for tool in ("ar", "tar"):
        if not shutil.which(tool):
            raise RuntimeError(f"required tool not found: {tool}")

    chrome_zip = bundle / "chromium-linux-arm64.zip"
    download(CHROMIUM_URL, chrome_zip)

    selected = {}
    for relative_url in INDEXES:
        print(f"Reading {relative_url}")
        with urllib.request.urlopen(UBUNTU_BASE + relative_url) as response:
            parse_index(response.read(), selected)
    missing = sorted(set(PACKAGES) - set(selected))
    if missing:
        raise RuntimeError("packages missing from Ubuntu indexes: " + ", ".join(missing))

    with tempfile.TemporaryDirectory(prefix="un-bound-engine-") as temp_name:
        root = Path(temp_name) / "root"
        root.mkdir()
        for name in PACKAGES:
            fields = selected[name]
            filename = fields["Filename"]
            deb = debs / Path(filename).name
            download(UBUNTU_BASE + filename, deb, fields["SHA256"])
            with tempfile.TemporaryDirectory(prefix="deb-") as extract_name:
                extract = Path(extract_name)
                subprocess.run(["ar", "x", str(deb)], cwd=extract, check=True)
                archives = list(extract.glob("data.tar.*"))
                if len(archives) != 1:
                    raise RuntimeError(f"could not locate data archive in {deb.name}")
                extract_data_archive(archives[0], root, work)

        lib_source = root / "usr/lib/aarch64-linux-gnu"
        if not lib_source.is_dir():
            raise RuntimeError("Ubuntu ARM64 library directory was not extracted")
        lib_destination = bundle / "lib"
        if lib_destination.exists():
            shutil.rmtree(lib_destination)
        lib_destination.mkdir()
        for source in lib_source.glob("*.so*"):
            copy_library(source, lib_destination)
        nss = lib_source / "nss"
        if nss.is_dir():
            copy_library(nss, lib_destination)

    print(f"Engine bundle ready: {bundle}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"prepare-engine: {error}", file=sys.stderr)
        sys.exit(1)
