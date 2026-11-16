# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
"""Builds the public Hjalti + Canine Drives release zip (one mod to install).

Usage: python -I package_release.py <project root> <Hjalti mod folder>
  e.g. python -I tools/package_release.py . H:/CSVO/mods/SkyrimNet_Hjalti
Writes dist/Hjalti_CanineDrives_<version>.zip, the version taken from the layer's manifest.json:
  SN_Dog.esp, meshes/, textures/                                 (Hjalti himself)
  meshes/actors/canine/animations/OpenAnimationReplacer/Canine Drives/...   (his animations, our own builds)
  Sound/FX/CanineDrives/*.wav                                    (his voice and ability sounds, made with ElevenLabs)
  SKSE/Plugins/CanineDrives.dll (+ licence files)
  SKSE/Plugins/SkyrimNet/external/nosimo.canine_drives/...      (the SkyrimNet layer)
  README.txt                                                     (from dist/README-release.txt)
Left out: the "Barks ..." OAR submods (plain copies of vanilla Bethesda clips), .pdb, meta.ini, ESP backups, and any
layer prompt that is a local override of another mod's prompt (listed in SKIP, or marked TEMPORARY). Every file is
read back and compared.
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
readme = root / "dist/README-release.txt"
out = root / "dist" / f"Hjalti_CanineDrives_{version}.zip"

# Local copies of other mods' prompts with the user's own tweaks: they'd override a player's own copies.
SKIP = {"0005_sexlab_variables.prompt", "0401_appearance_body_shape.prompt", "0501_exposure.prompt"}
TEMPORARY = "TEMPORARY - REMOVE BEFORE PUBLISHING"
OAR = "OpenAnimationReplacer"
# OAR submods that only hold vanilla clips (copied, or with their sound markers moved): not ours to redistribute.
OAR_LEFT_OUT = ("Barks ",)
# The dev setup also lets Valiant (DragonsKeep-BE.esp, the old test dog) use the clips. Players won't have that plugin,
# and OAR flags a condition naming a missing plugin, so the release copies drop it.
DEV_ONLY_PLUGIN = "DragonsKeep-BE.esp"


def strip_dev_actor(node):
    """Remove IsActorBase conditions on the dev-only plugin, anywhere in an OAR condition tree."""
    if isinstance(node, dict):
        for key, val in list(node.items()):
            if isinstance(val, list):
                node[key] = [strip_dev_actor(c) for c in val
                             if not (isinstance(c, dict) and c.get("condition") == "IsActorBase" and
                                     c.get("Actor base", {}).get("pluginName") == DEV_ONLY_PLUGIN)]
            else:
                strip_dev_actor(val)
    return node


def oar_config(src):
    data = json.loads(src.read_text(encoding="utf-8"))
    return (json.dumps(strip_dev_actor(data), indent=4) + "\n").encode("utf-8")

entries = {}  # path in zip -> source
rewritten = {}  # path in zip -> bytes, for files changed on the way in
entries["SN_Dog.esp"] = dog / "SN_Dog.esp"
for sub in ("meshes", "textures", "Sound"):
    for f in (dog / sub).rglob("*"):
        if not f.is_file():
            continue
        parts = f.relative_to(dog).parts
        if OAR in parts:
            i = parts.index(OAR)
            submod = parts[i + 2] if len(parts) > i + 3 else ""   # .../OpenAnimationReplacer/Canine Drives/<submod>/...
            if submod.startswith(OAR_LEFT_OUT):
                continue
            if f.name == "config.json":
                rewritten[f.relative_to(dog).as_posix()] = oar_config(f)
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
entries["README.txt"] = readme

missing = [str(src) for src in entries.values() if not src.is_file()]
if missing:
    sys.exit("missing: " + ", ".join(missing))
if version not in readme.read_text(encoding="utf-8").splitlines()[0]:
    sys.exit(f"the readme's title line doesn't name version {version}")

out.parent.mkdir(parents=True, exist_ok=True)
if out.exists():
    out.unlink()
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for arc in sorted(entries):
        if arc in rewritten:
            z.writestr(arc, rewritten[arc])
        else:
            z.write(entries[arc], arc)

bad = 0
with zipfile.ZipFile(out) as z:
    for arc, src in entries.items():
        want = rewritten.get(arc) or src.read_bytes()
        if hashlib.md5(z.read(arc)).digest() != hashlib.md5(want).digest():
            print("MISMATCH", arc)
            bad += 1
    names = z.namelist()
    submods = sorted({n.split("/")[6] for n in names if f"/{OAR}/" in n and n.count("/") > 6})
    print(f"OAR submods ({len(submods)}): {', '.join(submods)}")
    print(f"sounds: {sum(1 for n in names if n.startswith('Sound/'))}")
    left = [n for n in rewritten if DEV_ONLY_PLUGIN.encode() in z.read(n)]
    print(f"OAR configs rewritten: {len(rewritten)}, still naming {DEV_ONLY_PLUGIN}: {len(left)}")
    bad += len(left)
    print(f"{out.name}: {len(names)} files, {bad} mismatches, {out.stat().st_size / 1e6:.1f} MB")
sys.exit(1 if bad else 0)
