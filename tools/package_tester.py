# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
"""Builds the combined Hjalti + Canine Drives tester zip (one mod to install).

Usage: python -I package_tester.py <project root> <Hjalti mod folder>
  e.g. python -I tools/package_tester.py . H:/CSVO/mods/SkyrimNet_Hjalti
Writes dist/CanineDrives_Hjalti_<version>_test.zip, the version taken from the layer's manifest.json:
  SN_Dog.esp, meshes/, textures/                                 (Hjalti himself)
  SKSE/Plugins/CanineDrives.dll (+ licence files)
  SKSE/Plugins/SkyrimNet/external/nosimo.canine_drives/...      (the SkyrimNet layer)
  README-testers.txt                                             (from dist/)
Left out: the OAR animations (other mods' files), .pdb, meta.ini, ESP backups, and any layer prompt that is a local
override of another mod's prompt (listed in SKIP, or marked TEMPORARY). Every file is read back and compared.
"""
import hashlib
import json
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1]).resolve()
dog = Path(sys.argv[2]).resolve()
layer = root / "skyrimnet/nosimo.canine_drives"
version = json.loads((layer / "manifest.json").read_text(encoding="utf-8"))["version"]
readme = root / "dist/README-testers.txt"
out = root / "dist" / f"CanineDrives_Hjalti_{version}_test.zip"

# Local copies of other mods' prompts with the user's own tweaks: they'd override a tester's own copies.
SKIP = {"0005_sexlab_variables.prompt", "0401_appearance_body_shape.prompt", "0501_exposure.prompt"}
TEMPORARY = "TEMPORARY - REMOVE BEFORE PUBLISHING"

entries = {}  # path in zip -> source
entries["SN_Dog.esp"] = dog / "SN_Dog.esp"
for sub in ("meshes", "textures"):
    for f in (dog / sub).rglob("*"):
        if f.is_file() and "OpenAnimationReplacer" not in f.parts:  # the merged mod folder also holds the OAR set
            entries[f.relative_to(dog).as_posix()] = f
entries["SKSE/Plugins/CanineDrives.dll"] = root / "build/CanineDrives.dll"
entries["SKSE/Plugins/CanineDrives_LICENSE.txt"] = root / "LICENSE"
entries["SKSE/Plugins/CanineDrives_THIRD_PARTY_NOTICES.md"] = root / "THIRD_PARTY_NOTICES.md"
for f in layer.rglob("*"):
    if not f.is_file():
        continue
    rel = f.relative_to(layer).as_posix()
    if f.name in SKIP or (f.suffix == ".prompt" and TEMPORARY in f.read_text(encoding="utf-8", errors="ignore")):
        print(f"left out (local override): {rel}")
        continue
    entries["SKSE/Plugins/SkyrimNet/external/nosimo.canine_drives/" + rel] = f
entries["README-testers.txt"] = readme

missing = [str(src) for src in entries.values() if not src.is_file()]
if missing:
    sys.exit("missing: " + ", ".join(missing))
if version not in readme.read_text(encoding="utf-8").splitlines()[0]:
    print(f"WARNING: the readme's title line doesn't name version {version}")

out.parent.mkdir(parents=True, exist_ok=True)
if out.exists():
    out.unlink()
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for arc in sorted(entries):
        z.write(entries[arc], arc)

bad = 0
with zipfile.ZipFile(out) as z:
    for arc, src in entries.items():
        if hashlib.md5(z.read(arc)).digest() != hashlib.md5(src.read_bytes()).digest():
            print("MISMATCH", arc)
            bad += 1
    print(f"{out.name}: {len(z.namelist())} files, {bad} mismatches, {out.stat().st_size / 1e6:.1f} MB")
sys.exit(1 if bad else 0)
