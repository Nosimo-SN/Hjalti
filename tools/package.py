# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
"""Builds the Canine Drives mod zip.

Usage: python -I package.py <project root> <version>
Writes build/dist/CanineDrives-<version>.zip:
  SKSE/Plugins/CanineDrives.dll (+ licence files)
  SKSE/Plugins/SkyrimNet/external/nosimo.canine_drives/...   (SkyrimNet plugin: manifest, settings page, prompt)
"""
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
version = sys.argv[2]
dist = root / "build" / "dist"
dist.mkdir(parents=True, exist_ok=True)
out = dist / f"CanineDrives-{version}.zip"

files = {
    "SKSE/Plugins/CanineDrives.dll": root / "build/CanineDrives.dll",
    "SKSE/Plugins/CanineDrives_LICENSE.txt": root / "LICENSE",
    "SKSE/Plugins/CanineDrives_THIRD_PARTY_NOTICES.md": root / "THIRD_PARTY_NOTICES.md",
}
plugin_dir = root / "skyrimnet/nosimo.canine_drives"
TEMPORARY = "TEMPORARY - REMOVE BEFORE PUBLISHING"
for f in plugin_dir.rglob("*"):
    if f.is_file():
        if f.suffix == ".prompt" and TEMPORARY in f.read_text(encoding="utf-8", errors="ignore"):
            print(f"left out (temporary local override): {f.relative_to(plugin_dir).as_posix()}")
            continue
        files["SKSE/Plugins/SkyrimNet/external/nosimo.canine_drives/" + f.relative_to(plugin_dir).as_posix()] = f

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for arc, src in sorted(files.items()):
        z.write(src, arc)

print(f"{out}: {len(files)} files")
