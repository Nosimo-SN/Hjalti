# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
"""Writes the Canine Drives settings schema (the plugin's Settings page in the SkyrimNet dashboard).

Usage: python -I gen_settings.py <project root> <version>
Writes skyrimnet/nosimo.canine_drives/settings/CanineDrives.yaml.
The defaults here must match src/settings.h and the default text in src/settings.cpp.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1])
version = sys.argv[2]
out = root / "skyrimnet" / "nosimo.canine_drives" / "settings" / "CanineDrives.yaml"

TIERS = ["noticeable", "strong", "urgent"]
DRIVES = [
    ("hunger", "Hunger", [
        "{name} is getting hungry; his nose follows any smell of food.",
        "{name} is very hungry. His stomach growls, and his nose keeps turning toward any smell of meat or bread.",
        "{name} is starving. Food is all he can think about; he whines and searches every corner for something to eat.",
    ]),
    ("affection", "Affection", [
        "{name} wants some attention and keeps drifting close to {player}.",
        "{name} badly wants affection. He presses against {player} and nudges for a scratch.",
        "{name} is starved for affection; he whines softly and won't leave {player}'s side, desperate to be touched.",
    ]),
    ("restless", "Restlessness", [
        "{name} is restless; he paces and sniffs around.",
        "{name} is bursting with energy. He can't keep still, circling and bouncing, eager to run.",
        "{name} is wild with pent-up energy; he dashes about barking, desperate to run and play.",
    ]),
    ("tired", "Tiredness", [
        "{name} is getting sleepy; he yawns, and his steps are slower.",
        "{name} is very tired. His eyelids droop, and he keeps looking for a soft spot to lie down.",
        "{name} is exhausted. He can barely keep his eyes open and wants nothing but to curl up and sleep.",
    ]),
    ("separation", "Separation", [
        "{name} feels {player} is too far away and keeps checking which way {player} went.",
        "{name} misses {player} badly; he whines and paces, sniffing the air for {player}'s scent.",
        "{name} is frantic without {player}. He howls and searches everywhere, unable to settle.",
    ]),
]

lines = [
    "plugin:",
    '  name: "Canine Drives"',
    f'  version: "{version}"',
    "  description: >-",
    "    Bodily needs for a dog companion: hunger, affection, restlessness, tiredness and missing you. They rise",
    "    and fall with game time and what the dog is doing, are saved with your game, and his strongest needs",
    "    reach his prompt as plain facts about his body. Rates are per game hour; 0-100 for every drive, higher",
    "    means a stronger need. Changes apply within about five seconds, no restart needed.",
    '  icon: "🐕"',
    "",
    "schema:",
    "  fields:",
]


def q(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def field(name, path, ftype, description, default, category, lo=None, hi=None):
    lines.append(f"    - name: {q(name)}")
    lines.append(f"      path: {q(path)}")
    lines.append(f"      type: {q(ftype)}")
    lines.append(f"      description: {q(description)}")
    lines.append(f"      defaultValue: {q(default) if ftype == 'string' else default}")
    if lo is not None:
        lines.append(f"      min: {lo}")
        lines.append(f"      max: {hi}")
    lines.append(f"      category: {q(category)}")
    lines.append("")


def section(title):
    lines.append(f"    # --- {title} " + "-" * max(4, 92 - len(title)))


section("General")
field("Enabled", "general.enabled", "bool",
      "Drives change over time. Off freezes them where they are (the prompt keeps showing the current needs).",
      "true", "General")
field("Dog", "general.actor", "string",
      "The dog to track, as Plugin.esp|0xLocalID: a placed reference, or an NPC record to track whichever reference of it is loaded. The default is Hjalti (SN_Dog.esp).",
      "SN_Dog.esp|0x800", "General")

section("Rates")
field("Hunger Rise", "hunger.rate", "float", "Hunger gained per game hour, asleep or awake.", 4.0, "Rates", 0.0, 100.0)
field("Affection Rise", "affection.rate", "float", "Need for affection gained per waking game hour.", 3.0, "Rates", 0.0, 100.0)
field("Restlessness Rise (idle)", "restless.idle_rate", "float",
      "Restlessness gained per waking game hour while he isn't travelling.", 10.0, "Rates", 0.0, 100.0)
field("Restlessness Drain (travelling)", "restless.travel_drain", "float",
      "Restlessness lost per game hour while he is on the move.", 25.0, "Rates", 0.0, 200.0)
field("Tiredness Rise", "tired.rate", "float", "Tiredness gained per waking game hour.", 3.0, "Rates", 0.0, 100.0)
field("Tiredness at Night", "tired.night_mult", "float",
      "Tiredness rises this many times faster between 9 pm and 6 am.", 1.5, "Rates", 1.0, 5.0)
field("Tiredness when Travelling", "tired.travel_mult", "float",
      "Tiredness rises this many times faster while he is on the move.", 1.5, "Rates", 1.0, 5.0)
field("Tiredness Drain (asleep)", "tired.sleep_drain", "float",
      "Tiredness lost per game hour of sleep. He sleeps when you sleep with him close by.", 15.0, "Rates", 0.0, 200.0)
field("Nap Rest", "tired.nap_drain", "float",
      "Tiredness lost per game hour while he naps on his own. Naps are short, so this is faster than sleep.",
      30.0, "Rates", 0.0, 200.0)
field("Restlessness While Tired", "tired.restless_mult", "float",
      "While tiredness is strong, idle restlessness rises only this fraction as fast (a worn-out dog doesn't fidget), and it drops out of his prompt.",
      0.2, "Rates", 0.0, 1.0)
field("Separation Rise (apart)", "separation.apart_rate", "float",
      "How fast he starts missing you, per game hour away from you.", 20.0, "Rates", 0.0, 200.0)
field("Separation Drain (together)", "separation.near_drain", "float",
      "How fast that settles, per game hour close to you.", 30.0, "Rates", 0.0, 200.0)
field("Close-by Distance", "separation.near_distance", "float",
      "Within this many units of you (and in the same space) counts as together. About 70 units is a metre.",
      1500.0, "Rates", 200.0, 10000.0)

section("Combat")
field("Hunger in Combat", "combat.hunger_mult", "float",
      "Hunger rises this many times faster while he is fighting.", 1.5, "Combat", 1.0, 5.0)
field("Restlessness Drain in Combat", "combat.restless_drain", "float",
      "Restlessness lost per game hour of fighting.", 60.0, "Combat", 0.0, 300.0)
field("Tiredness in Combat", "combat.tired_mult", "float",
      "Tiredness rises this many times faster while he is fighting.", 2.0, "Combat", 1.0, 5.0)

section("Feeding")
field("Hunger per Food Item", "feeding.per_item", "float",
      "Hunger removed for each food item he eats. Any food that lands in his inventory (handed over, a gift menu, another mod's action) is eaten once no menu is open.",
      25.0, "Feeding", 0.0, 100.0)
field("Raw Meat Bonus", "feeding.raw_bonus", "float",
      "Extra hunger removed per raw food item (a dog's favourite).", 5.0, "Feeding", 0.0, 50.0)
field("Most per Meal", "feeding.max_meal", "float",
      "The most hunger one meal can remove, however much you give.", 70.0, "Feeding", 0.0, 100.0)
field("Hunger per Carcass", "feeding.carcass_meal", "float",
      "Hunger removed when he eats from a carcass.", 50.0, "Feeding", 0.0, 100.0)
field("Carcass Meat", "feeding.carcass_meat", "int",
      "Whose bodies he'll eat from. 0 = animals only. 1 = auto: people too while you wear the Ring of Namira or are a werewolf. 2 = any meat: people too, always. Never children, at any setting. People get the bloody feeding effect; animals don't.",
      0, "Feeding", 0, 2)

section("Petting")
field("Pet Hotkey", "petting.hotkey", "hotkey",
      "Press near him to kneel and scratch his ears. Use it together with the modifiers below. -1 = off.",
      -1, "Petting", -1, 255)
field("Pet Hotkey Modifiers", "petting.modifiers", "int",
      "Keys to hold with the hotkey: 1 = Ctrl, 2 = Shift, 4 = Alt; add them up (3 = Ctrl+Shift). 0 = none. Exactly these must be held.",
      2, "Petting", 0, 7)
field("Pet Range", "petting.range", "float",
      "How close you must be for the hotkey to work, in units (about 70 to a metre).", 250.0, "Petting", 100.0, 1000.0)
field("Pet by Activating", "petting.on_activate", "bool",
      "Also count activating him with empty hands as petting. Off by default: other mods' dialogue menus open on activation.",
      "false", "Petting")
field("Affection per Pet", "petting.affection", "float",
      "Need for affection removed when you pet him.", 25.0, "Petting", 0.0, 100.0)
field("Separation per Pet", "petting.separation", "float",
      "How much a pet settles his missing you.", 15.0, "Petting", 0.0, 100.0)
field("Petting Cooldown (seconds)", "petting.cooldown", "float",
      "Real seconds before another pet counts.", 60.0, "Petting", 0.0, 600.0)
field("Activation Only While Sneaking", "petting.require_sneak", "bool",
      "With Pet by Activating on: count it only while you are crouched, so ordinary activations stay ordinary.",
      "false", "Petting")

section("Behavior Loop")
field("Behavior Loop", "loop.enabled", "bool",
      "In quiet moments (nobody talking, no fighting, no menus, you close by) SkyrimNet's decisions model picks what he does next from his needs: beg, nap, seek affection or nothing. One small decision call per check.",
      "true", "Behavior Loop")
field("Check Every (min seconds)", "loop.min_seconds", "float",
      "Shortest real-time wait between checks.", 45.0, "Behavior Loop", 15.0, 1800.0)
field("Check Every (max seconds)", "loop.max_seconds", "float",
      "Longest real-time wait between checks; each wait is random in between.", 90.0, "Behavior Loop", 15.0, 1800.0)
field("Quiet For (seconds)", "loop.quiet_seconds", "float",
      "Nobody nearby may have spoken for this long before he does something on his own.", 20.0, "Behavior Loop", 0.0, 300.0)
field("Confidence Needed", "loop.confidence", "float",
      "How sure the decisions model must be (0-1) before he acts on its choice.", 0.5, "Behavior Loop", 0.0, 1.0)

section("Hearing")
field("Hearing", "perception.enabled", "bool",
      "When you speak to him (aloud or over the mind-link), SkyrimNet's decisions model hears how you meant it: praise, a scolding, an offer of food, or anything else. One small decision call per line addressed to him.",
      "true", "Hearing")
field("Affection per Praise", "perception.praise_affection", "float",
      "How much praise (\"good boy!\") satisfies his need for affection.", 10.0, "Hearing", 0.0, 100.0)
field("Unsettled by Scolding", "perception.scold_affection", "float",
      "How much a scolding raises his need for affection: he wants to make up.", 5.0, "Hearing", 0.0, 100.0)
field("Talk to Feed", "perception.talk_feed", "bool",
      "When you offer him food in words (\"here boy, have this sweetroll\"), one matching food moves from your pack to him and he eats it. The food you name wins; otherwise raw meat, then any food. Drinks only when named.",
      "true", "Hearing")
field("Confidence Needed", "perception.confidence", "float",
      "How sure the decisions model must be (0-1) before a line counts as praise, scolding or food.", 0.6, "Hearing", 0.0, 1.0)

section("Training")
field("Training", "training.enabled", "bool",
      "Praise or a scolding within a minute after one of his actions (playing, sniffing someone, begging, napping, seeking a scratch, eating) is remembered for that action. Once a lean is clear he has \"learned\" it: his dialogue and his choice of what to do next see a plain sentence such as \"he has learned that you are usually pleased when he races about\". No numbers ever reach his prompts, and no memories are written.",
      "true", "Training")
field("Lessons Fade (game hours)", "training.half_life", "float",
      "Game hours for a lesson to fade to half its strength, so old lessons soften and he can be retrained. 0 = lessons never fade.",
      72.0, "Training", 0.0, 1000.0)

section("Dreams")
field("Dreams", "dreams.enabled", "bool",
      "When you sleep with him close by (one dream per 4 hours slept, up to 3), or after a long nap, SkyrimNet writes a short dream from his side and keeps it as one of his memories: smells, chases, food, you. One small LLM call per sleep or nap.",
      "true", "Dreams")
field("Nap Dream After (minutes)", "dreams.nap_minutes", "float",
      "Real minutes he must have lain asleep before a nap can bring a dream. 0 = only nights bring dreams.",
      5.0, "Dreams", 0.0, 60.0)
field("Dream Importance", "dreams.importance", "float",
      "How strongly a dream memory competes with his other memories when SkyrimNet recalls them (0-1).",
      0.6, "Dreams", 0.0, 1.0)

section("Events")
field("Tell SkyrimNet", "events.enabled", "bool",
      "Post a short event when he eats, is petted or does something on his own, so he and the people nearby know it happened.",
      "true", "Events")
field("As Narration", "events.narrate", "bool",
      "Send his meals, pets and the things he does on his own as narration, so he reacts to them (a bark, a wag, a few words over the mind-link). Off: they are quiet events that only add to what he and the people nearby remember. Each narration is one dialogue call.",
      "true", "Events")
field("Quiet Barks", "events.quiet_barks", "int",
      "Which of his barks are switched off, for him only (other dogs are unaffected). 0 = none. 1 = the random barks while he idles (DogBarkStanding, DogBarkRunIdle, IdleDogBarkStanding); he still barks when he talks. 2 = also the barks that play while he talks, so he only barks in combat and when Canine Drives has him bark (after praise or petting, at the end of a play), which then plays as a bark sound. At 1 or 2 his random bark animations (the ones the dog animations pick on their own) are also swapped for quiet ones: a head tilt, a look around, or a yawn when he's tired.",
      1, "Events", 0, 2)
field("Greet", "events.greet", "bool",
      "When he isn't following you and you come back after leaving him a while, he runs over to greet you (a few happy barks if he can't get to you).",
      "true", "Events")
field("Smell", "events.smell", "bool",
      "He smells hostile creatures and people nearby before you see them, and his prompt says what and roughly where, so he can growl or warn you.",
      "true", "Events")
field("Take Owned Food", "events.take_owned_food", "bool",
      "He may help himself to food that belongs to someone: off their table, or out of their pack. Off: only scraps nobody owns. Nobody is charged with a crime either way (a dog can't be); the owner may well have something to say about it.",
      "true", "Events")
field("Owners Charge You", "events.charge_player", "bool",
      "When he takes owned food, the game counts it as your theft: the owner may confront you or want paying, since a dog can't be fined. Off: he takes it without any crime, and only the narration tells the owner.",
      "true", "Events")
field("NPCs Pet Him", "events.npc_pet", "bool",
      "A person standing right beside him can pet him: they kneel where they stand and scratch his ears, and his need for affection drops. They never walk over to him, so nothing about their AI changes. Adds one action to the list for anyone that close. It's a quiet event, so it doesn't add a narration.",
      "true", "Events")
field("NPC Pet Cooldown", "events.npc_pet_cooldown", "float",
      "Real seconds before anyone else can pet him this way.", 180.0, "Events", 0.0, 3600.0)
field("Hunting", "events.hunting", "bool",
      "When he's hungry or restless he can run down small game on his own (never farm animals, pets or anything someone owns) and eat what he catches, and he can eat from carcasses he finds. He says so in a narration when he eats; a hunt that fails is only a quiet event.",
      "true", "Events")
field("Hunt Cooldown", "events.hunt_cooldown", "float",
      "Real minutes before he may set off on another hunt.", 10.0, "Events", 0.0, 600.0)
field("Smell Range", "events.smell_range", "float",
      "How far his nose reaches, in units (about 70 per meter). He growls or whines out loud at a new scent within three quarters of this.",
      4000.0, "Events", 1000.0, 8000.0)
field("Smell Quiet Time", "events.smell_quiet", "float",
      "A new scent is on his mind for about 30 seconds, and he may growl or whine at it. After that, this many seconds pass before another new scent gets his attention, so a field full of animals doesn't fill his talk. A threat close by always does.",
      300.0, "Events", 0.0, 3600.0)
field("Sing Cooldown", "events.sing_cooldown", "float",
      "Real minutes before he may howl along with a bard again. He only joins in when he's very restless, at most once per song.",
      30.0, "Events", 0.0, 600.0)
field("Mind-Link Repair", "events.leak_repair", "bool",
      "If he ever answers out loud in words, post a quiet event that the words reached only you, through the mind-link; everyone else heard barks and whines. Keeps the town from remembering a talking dog.",
      "true", "Events")
field("Notifications", "events.notify", "bool",
      "Show a note in the top-left corner when he settles down for a nap, wakes from one, or keeps a dream.",
      "true", "Events")

section("Combat")
field("Spare the Beaten", "combat.spare_beaten", "bool",
      "He lets go of an enemy who yields or goes down beaten (bleedout), instead of biting them to death. Telling him to 'leave it' or 'back off' mid-fight calls him off too.",
      "true", "Combat")
field("Combat Awareness", "combat.awareness", "bool",
      "While he or you are fighting, his prompt lists who is in the fight within about 3000 units and how they're doing, in plain words: hurt or winded, low on magic, who they're going for, how far off. It can shape what he does, such as who he picks to attack. Adds a few lines to his prompt during fights only.",
      "true", "Combat")

section("Progression")
field("Enabled", "progression.enabled", "bool",
      "He grows with your level, or his own if that's higher: bite, armor, magic resistance, health and speed rise in steps up to level 30 (a fifth of the level-30 value at 5, two fifths at 10, three at 15, four at 20) and smoothly after that up to the top level.",
      "true", "Progression")
field("Damage Taken at Level 1", "progression.damage_taken_start", "float",
      "How much of every hit he takes at level 1, in percent (100 = full damage, 50 = half). It eases to the top-level value as he levels. Works with any combat overhaul, since it scales the damage itself; lethal setups like SkyValor want it low.",
      80.0, "Progression", 5.0, 200.0)
field("Damage Taken at Top Level", "progression.damage_taken_top", "float", "How much of every hit he takes at the top level, in percent.", 50.0, "Progression", 5.0, 200.0)
field("Size at Level 1", "progression.scale_start", "float",
      "How big he is at level 1 (1 = a normal husky). He grows evenly from this to the top-level size as he levels, and his prompt describes his size to match, with 1 as ordinary. Going much over about 1.2 can make tight spaces hard for him to get through.",
      0.9, "Progression", 0.5, 2.0)
field("Size at Top Level", "progression.scale_top", "float", "How big he is at the top level (1 = a normal husky).", 1.1, "Progression", 0.5, 2.0)
field("Top Level", "progression.max_level", "int", "The level his growth reaches its full value; past it he stays there.", 50, "Progression", 31, 200)
field("Bite at 30", "progression.bite_30", "float", "At level 30: extra bite damage.", 12.0, "Progression", 0.0, 500.0)
field("Bite at Top", "progression.bite_50", "float", "At the top level: extra bite damage.", 20.0, "Progression", 0.0, 500.0)
field("Armor at 30", "progression.armor_30", "float", "At level 30: extra armor rating (vanilla: each point is about 0.12% less damage taken).", 160.0, "Progression", 0.0, 2000.0)
field("Armor at Top", "progression.armor_50", "float", "At the top level: extra armor rating (vanilla: each point is about 0.12% less damage taken).", 250.0, "Progression", 0.0, 2000.0)
field("Magic Resist at 30", "progression.magic_30", "float", "At level 30: magic resistance in percent (the game caps it at 85).", 30.0, "Progression", 0.0, 85.0)
field("Magic Resist at Top", "progression.magic_50", "float", "At the top level: magic resistance in percent (the game caps it at 85).", 50.0, "Progression", 0.0, 85.0)
field("Health at 30", "progression.health_30", "float", "At level 30: extra health.", 150.0, "Progression", 0.0, 5000.0)
field("Health at Top", "progression.health_50", "float", "At the top level: extra health.", 300.0, "Progression", 0.0, 5000.0)
field("Speed at 30", "progression.speed_30", "float", "At level 30: faster movement in percent (100 = twice as fast).", 10.0, "Progression", 0.0, 100.0)
field("Speed at Top", "progression.speed_50", "float", "At the top level: faster movement in percent (100 = twice as fast).", 20.0, "Progression", 0.0, 100.0)
field("Steady Stance I Level", "progression.steady1_level", "int", "The level he gains Steady Stance I: less stagger from hits (a quarter, then half, then 70% less).", 5, "Progression", 1, 200)
field("Steady Stance II Level", "progression.steady2_level", "int", "The level he gains Steady Stance II: less stagger from hits (a quarter, then half, then 70% less).", 15, "Progression", 1, 200)
field("Steady Stance III Level", "progression.steady3_level", "int", "The level he gains Steady Stance III: less stagger from hits (a quarter, then half, then 70% less).", 30, "Progression", 1, 200)
field("Bowling Charge Level", "progression.bowling_level", "int", "The level he gains Bowling Charge: his own attacks stagger 30% harder.", 20, "Progression", 1, 200)

section("Prompt")
field("Noticeable From", "tiers.noticeable", "float", "A drive at or above this shows as a light fact.", 40.0, "Prompt", 1.0, 100.0)
field("Strong From", "tiers.strong", "float", "At or above this the need colors everything he does.", 65.0, "Prompt", 1.0, 100.0)
field("Urgent From", "tiers.urgent", "float", "At or above this the need leads.", 85.0, "Prompt", 1.0, 100.0)
field("Needs Shown", "prompt.max_drives", "int", "How many of his strongest needs reach the prompt.", 2, "Prompt", 1, 5)

section("Prompt text")
for key, label, texts in DRIVES:
    for tier, text in zip(TIERS, texts):
        field(f"{label}: {tier}", f"text.{key}_{tier}", "string",
              "Placeholders: {name} = the dog, {player} = you. Facts about his body, not instructions.",
              text, "Prompt Text")

section("Runes")
field("Runes", "runes.enabled", "bool",
      "Hjalti (SN_Dog) gains runes on his coat from dragon souls: each soul the player takes while he is close by counts, and every few of them works another rune in (forelegs, then hind legs, brow, back). The runes stay dark and light up at moments.",
      "true", "Runes")
field("Souls per Rune", "runes.souls_per_unlock", "int", "The first rune and soul gift come with the first dragon soul he witnesses; each further one needs this many more (default 2: souls 1, 3, 5, 7...). Four runes; seven gifts (the last at 13 souls by default).", 2, "Runes", 1, 20)
field("Bystanders React", "runes.witness", "bool",
      "When he witnesses a dragon soul taken, the nearest person who can see him (a stranger before one of your companions) is the one who reacts to the narration. Off: SkyrimNet picks, which in practice is usually him or whoever you face.",
      "true", "Runes")
field("Soul Gifts", "runes.gifts", "bool",
      "Gifts from the dragon souls he witnesses, one with each rune step: Frost Bite (his bite chills and slows; stronger at the fourth and sixth steps), then Perseverance (badly hurt, the runes knit his wounds), then Frost Step (he crosses a gap in an instant). Quicker cooldowns come at the fifth step, and Frost Howl (he knocks down the foes around him) at the seventh.",
      "true", "Runes")
field("Perseverance Heal", "runes.persevere_heal", "float",
      "Perseverance (second gift): when he falls to a fifth of his health in a fight, or goes down, the runes knit this much of his health back (percent) over five seconds.",
      40.0, "Runes", 5.0, 100.0)
field("Perseverance Cooldown", "runes.persevere_cooldown", "float",
      "Seconds before Perseverance can come again (shorter from the fifth gift on).", 120.0, "Runes", 10.0, 1200.0)
field("Frost Step", "runes.frost_step", "bool",
      "Third gift. In a fight he can cross a gap in an instant: behind a foe he can see far off, or to your side when he falls to 30% health. He only lands on solid ground with a clear line from where he's going.",
      "true", "Runes")
field("Frost Step Cooldown", "runes.frost_step_cooldown", "float",
      "Seconds before he can Frost Step at a foe again (shorter from the fifth gift on).", 30.0, "Runes", 5.0, 600.0)
field("Frost Step Escape Cooldown", "runes.frost_step_escape_cooldown", "float",
      "Seconds before he can Frost Step to your side again (shorter from the fifth gift on).", 90.0, "Runes", 10.0, 1200.0)
field("Frost Howl", "runes.frost_howl", "bool",
      "Seventh gift. When two or more foes are close around him, or one is close while he is badly hurt, he howls, frost bursts from him, and every foe within about 300 units is knocked down where they stand. Only foes: never you, your companions, bystanders or children. No damage; it buys him a moment while they get up.",
      "true", "Runes")
field("Frost Howl Cooldown", "runes.frost_howl_cooldown", "float",
      "Seconds before he can Frost Howl again (shorter from the fifth gift on).", 90.0, "Runes", 10.0, 1200.0)
field("Frost Howl Force", "runes.frost_howl_force", "float",
      "How hard the howl shoves them. Near 0 they fall where they stand; higher throws them back. If they don't go down at all, raise it a little.", 2.0, "Runes", 0.0, 50.0)
field("Rune Glow Strength", "runes.max_glow", "float",
      "How bright the runes glow at their strongest, when he witnesses a dragon soul taken. Everything else lights them less: a soul gift, a power attack, a threat on the air, a song, and faintly while he fights. 0 = never glow.",
      3.0, "Runes", 0.0, 10.0)

section("Debug")
field("Log Barks", "debug.log_barks", "bool",
      "Write each idle he starts and each bark or sound animation event to CanineDrives.log, to trace where a bark came from.", "true", "Debug")
field("Log Combat", "debug.log_combat", "bool",
      "Write his fights to CanineDrives.log: the hits he takes (one line per attacker and weapon or spell each second), each attack he swings, and his combat state once a second.", "false", "Debug")
field("Log Drives", "debug.log_changes", "bool",
      "Write the drives to CanineDrives.log (next to skse64.log) every game hour.", "false", "Debug")
field("Set Drives", "debug.set", "string",
      "For testing: e.g. hunger=80, tired=90 (or all=50). Applied once each time you change this text while playing. graph <label> (e.g. graph lying) writes his animation state to the log instead. bark makes him bark now; bark sound plays only the husky bark sound.",
      "", "Debug")

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
print(f"wrote {out}")
