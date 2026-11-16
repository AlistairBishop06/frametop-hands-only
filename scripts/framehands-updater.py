#!/usr/bin/env python3
"""Check and install published FrameHands releases without modifying SteamOS."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import Request, urlopen

APP = Path(__file__).resolve().parents[1]
STATUS = Path.home() / ".cache/framehands/update-status"
API = "https://api.github.com/repos/AlistairBishop06/frametop-hands-only/releases/latest"
TAG = re.compile(r"v(\d+)\.(\d+)\.(\d+)$")


def report(value):
    STATUS.parent.mkdir(parents=True, exist_ok=True)
    STATUS.write_text(value + "\n")


def fetch(url):
    req = Request(url, headers={"User-Agent": "FrameHands-updater", "Accept": "application/vnd.github+json"})
    with urlopen(req, timeout=30) as response:
        return response.read()


def latest():
    release = json.loads(fetch(API))
    tag = release["tag_name"]
    if not TAG.fullmatch(tag):
        raise ValueError("unexpected release version")
    assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
    for name in ("framehands-aarch64.tar.gz", "SHA256SUMS"):
        expected = f"https://github.com/AlistairBishop06/frametop-hands-only/releases/download/{tag}/{name}"
        if assets.get(name) != expected:
            raise ValueError(f"missing or unexpected {name} URL")
    return tag, assets


def newer(tag):
    installed = (APP / ".framehands-release-tag")
    current = installed.read_text().strip() if installed.exists() else ""
    return not TAG.fullmatch(current) or tuple(map(int, TAG.fullmatch(tag).groups())) > tuple(map(int, TAG.fullmatch(current).groups()))


def install(tag, assets):
    # The dashboard supplies a pipe, never a password file or argument.
    auth_fd = 3 if os.environ.get("FRAMEHANDS_SUDO_FD") == "3" else None
    if auth_fd is None and subprocess.run(["sudo", "-n", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        report("SUDO REQUIRED")
        return 2
    report("DOWNLOADING")
    with tempfile.TemporaryDirectory(prefix="framehands-update-") as temp:
        root = Path(temp)
        archive = fetch(assets["framehands-aarch64.tar.gz"])
        sums = fetch(assets["SHA256SUMS"]).decode("ascii")
        match = re.search(r"^([0-9a-f]{64})  framehands-aarch64\.tar\.gz$", sums, re.MULTILINE)
        if not match or hashlib.sha256(archive).hexdigest() != match.group(1):
            raise ValueError("release checksum failed")
        path = root / "framehands-aarch64.tar.gz"
        path.write_bytes(archive)
        with tarfile.open(path, "r:gz") as tar:
            for member in tar.getmembers():
                parts = Path(member.name).parts
                if member.name.startswith("/") or ".." in parts or not (member.isfile() or member.isdir()):
                    raise ValueError("unsafe release archive")
            tar.extractall(root / "release")
        installer = root / "release/install.sh"
        if not installer.is_file():
            raise ValueError("release installer missing")
        report("INSTALLING")
        result = subprocess.run(["bash", str(installer)], cwd=root / "release",
                                env={**os.environ, "FRAMEHANDS_NONINTERACTIVE": "1"},
                                pass_fds=(auth_fd,) if auth_fd is not None else ())
        if result.returncode:
            raise RuntimeError(f"installer exited {result.returncode}")
    report("RESTART HEADSET")
    return 0


def main():
    try:
        tag, assets = latest()
        if not newer(tag):
            report("UP TO DATE")
            return 0
        if len(sys.argv) == 2 and sys.argv[1] == "check":
            report("AVAILABLE " + tag)
            return 0
        if len(sys.argv) == 3 and sys.argv[1] == "install" and sys.argv[2] == tag:
            return install(tag, assets)
        raise ValueError("usage: framehands-updater.py check|install <version>")
    except Exception as exc:
        report("ERROR " + str(exc)[:80])
        return 1


if __name__ == "__main__":
    sys.exit(main())
