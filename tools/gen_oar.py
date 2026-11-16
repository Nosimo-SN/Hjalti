"""Builds the Canine Drives OAR replacer: Valiant-only swaps of the dog graph's random "spice" clips, chosen by
the mood code the DLL keeps on his Variable08 actor value (ones digit = strongest drive, +10 = Quiet Barks on).

Usage: python -I gen_oar.py <out root>   (writes <out root>/meshes/actors/canine/animations/OpenAnimationReplacer/Canine Drives)
Clip sources: the vanilla spice clips extracted to oar/vanilla only. (Until 2026-11-13 some swaps used clips from
Casual / Playful Dog Animations; their authors couldn't be reached, so those were dropped. New moves are made in
tools\\DogAnimKit, which owns the "Sleeping ..." submods in the same folder: this script never touches those.)
"""
import json
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
VANILLA = HERE / "oar" / "vanilla" / "meshes" / "actors" / "canine" / "animations"

# Submods this script used to write that are gone now (borrowed clips only): removed when it runs.
RETIRED = ["Tail wags", "Chasing tail", "Lying down tired", "Lying down restless"]

# Named clips (vanilla spice clips; the head moves carry their own whine/sniff sounds)
CLIPS = {
    "whine": VANILLA / "dog_idle_spice_head1.hkx",
    "whine_sniff": VANILLA / "dog_idle_spice_head2.hkx",
    "sniff": VANILLA / "dog_spice_head3.hkx",
    "look": VANILLA / "dog_spice_head4.hkx",
    "whine_sit": VANILLA / "dog_spice_sit_head1.hkx",
    "whine_sniff_sit": VANILLA / "dog_spice_sit_head2.hkx",
    "sniff_sit": VANILLA / "dog_spice_sit_head3.hkx",
    "look_sit": VANILLA / "dog_spice_sit_head4.hkx",
    "lay": VANILLA / "dog_spice_lay_start.hkx",
    "bark": VANILLA / "dog_spice_bark.hkx",                      # the plain vanilla barks
    "bark_sit": VANILLA / "dog_spice_sit_bark.hkx",
}

CONTENT, HUNGRY, AFFECTION, RESTLESS, TIRED, SEPARATION = range(6)


def quiet(mood):
    return [10 + mood]


def any_quiet(mood):
    return [mood, 10 + mood]


# (submod name, description, priority, codes, {original clip: [(clip, weight), ...]})
SUBMODS = [
    ("Deliberate bark", "Canine Drives has him bark on purpose (code 20 and up for a few seconds): the plain bark, never swapped by any other animation mod.",
     66790090, ">=20",
     {"dog_spice_bark": [("bark", 1)], "dog_spice_sit_bark": [("bark_sit", 1)]}),
    ("Barks hungry", "Quiet Barks on, hungry: sniffing instead of a random bark.", 66790015, quiet(HUNGRY),
     {"dog_spice_bark": [("sniff", 1), ("whine_sniff", 1)], "dog_spice_sit_bark": [("sniff_sit", 1), ("whine_sniff_sit", 1)]}),
    ("Barks affection", "Quiet Barks on, wants affection: soft whines instead of a random bark.", 66790014, quiet(AFFECTION),
     {"dog_spice_bark": [("whine", 2), ("look", 1)], "dog_spice_sit_bark": [("whine_sit", 2), ("look_sit", 1)]}),
    ("Barks restless", "Quiet Barks on, restless: looking around and sniffing instead of a random bark.", 66790013, quiet(RESTLESS),
     {"dog_spice_bark": [("look", 2), ("sniff", 1)], "dog_spice_sit_bark": [("look_sit", 2), ("sniff_sit", 1)]}),
    ("Barks tired", "Quiet Barks on, tired: a slow look around or a low whine instead of a random bark.", 66790012, quiet(TIRED),
     {"dog_spice_bark": [("look", 2), ("whine", 1)], "dog_spice_sit_bark": [("look_sit", 2), ("whine_sit", 1)]}),
    ("Barks separation", "Quiet Barks on, missing the player: whining and searching looks instead of a random bark.", 66790011, quiet(SEPARATION),
     {"dog_spice_bark": [("whine", 2), ("whine_sniff", 1), ("look", 1)], "dog_spice_sit_bark": [("whine_sit", 2), ("whine_sniff_sit", 1), ("look_sit", 1)]}),
    ("Barks content", "Quiet Barks on, content: a sniff or a look instead of a random bark.", 66790010, quiet(CONTENT),
     {"dog_spice_bark": [("sniff", 1), ("look", 1)], "dog_spice_sit_bark": [("sniff_sit", 1), ("look_sit", 1)]}),
]

# The dogs these swaps are for: Valiant (the test stand-in) and Hjalti (SN_Dog.esp, the companion to be).
VALIANT = {"condition": "OR", "requiredVersion": "1.0.0.0", "Conditions": [
    {"condition": "IsActorBase", "requiredVersion": "1.0.0.0", "Actor base": {"pluginName": "DragonsKeep-BE.esp", "formID": "E21FBD"}},
    {"condition": "IsActorBase", "requiredVersion": "1.0.0.0", "Actor base": {"pluginName": "SN_Dog.esp", "formID": "800"}},
]}
NOT_COMBAT = {"condition": "IsInCombat", "requiredVersion": "1.0.0.0", "negated": True}


def mood_is(code, comparison="=="):
    return {"condition": "CompareValues", "requiredVersion": "1.0.0.0",
            "Value A": {"actorValue": 75, "actorValueType": "Value"},  # Variable08
            "Comparison": comparison, "Value B": {"value": float(code)}}


def main():
    out = pathlib.Path(sys.argv[1]) / "meshes" / "actors" / "canine" / "animations" / "OpenAnimationReplacer" / "Canine Drives"
    out.mkdir(parents=True, exist_ok=True)
    # Replace only this script's own submods (and drop retired ones): other submods in the folder, like
    # DogAnimKit's "Sleeping ..." set, are left alone.
    for name in [s[0] for s in SUBMODS] + RETIRED:
        if (out / name).exists():
            shutil.rmtree(out / name)
            if name in RETIRED:
                print(f"removed retired submod {name}")
    (out / "config.json").write_text(json.dumps({
        "name": "Canine Drives",
        "author": "Nosimo-SN",
        "description": "Valiant only, out of combat: the dog animations' random idle moves follow his drives, and his random barks are swapped for quiet moves while Quiet Barks is on.",
    }, indent=4) + "\n", encoding="utf-8")

    priorities = set()
    for name, description, priority, codes, swaps in SUBMODS:
        assert priority not in priorities, priority
        priorities.add(priority)
        folder = out / name
        folder.mkdir()
        if isinstance(codes, str):  # ">=20": a comparison
            mood = mood_is(int(codes[2:]), codes[:2])
        else:
            mood = None
        mood = mood or (mood_is(codes[0]) if len(codes) == 1 else {
            "condition": "OR", "requiredVersion": "1.0.0.0", "Conditions": [mood_is(c) for c in codes]})
        anim_datas = []
        for original, picks in swaps.items():
            if len(picks) == 1:
                shutil.copyfile(CLIPS[picks[0][0]], folder / f"{original}.hkx")
                continue
            variants = folder / f"_variants_{original}"
            variants.mkdir()
            entries = []
            for clip, weight in picks:
                shutil.copyfile(CLIPS[clip], variants / f"{clip}.hkx")
                entries.append({"filename": f"{clip}.hkx", "weight": float(weight)})
            anim_datas.append({
                "projectName": "DogProject",
                "path": "data\\meshes\\actors\\canine\\animations\\OpenAnimationReplacer\\Canine Drives\\"
                        f"{name}\\_variants_{original}",
                "variants": entries})
        config = {
            "name": name,
            "description": description,
            "priority": priority,
            "conditions": [VALIANT, NOT_COMBAT, mood],
        }
        if anim_datas:
            config["replacementAnimDatas"] = anim_datas
        (folder / "config.json").write_text(json.dumps(config, indent=4) + "\n", encoding="utf-8")
    print(f"wrote {len(SUBMODS)} submods to {out}")


if __name__ == "__main__":
    main()
