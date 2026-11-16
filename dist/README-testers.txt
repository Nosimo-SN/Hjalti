Hjalti + Canine Drives 0.26.5 - TEST BUILD
===========================================

Thank you for testing. This is an early build: expect rough edges, and please keep your
saves backed up. Install it as one mod with Mod Organizer 2 (Install a mod from archive)
or Vortex.

What it is
----------
Hjalti is a husky companion (SN_Dog.esp). Canine Drives is the SKSE plugin and SkyrimNet
layer that gives him a dog's mind:

- Needs that rise and fall with time: hunger, affection, restlessness, tiredness, missing you.
- Things he does on his own when it's quiet: begs, naps, plays, sniffs people over, howls
  with bards, scavenges or steals food, hunts small game and eats what he catches.
- His senses: he smells threats and prey before you see them, and in a fight he knows
  who is doing what to whom.
- He learns from praise and scolding, dreams when he sleeps, greets you after you've
  been apart, and warms you when you're cold (with Frostfall).
- He grows with your level, and runes work into his coat as he witnesses dragon souls
  taken, each step bringing a gift: Frost Bite, Perseverance, Frost Step and Frost Howl.
- He's a dog: he barks and whines out loud. Speak to him over SkyrimNet's mind-link
  (telepathy) to hear his thoughts in words.

Where to find him
-----------------
Outside the Sleeping Giant Inn in Riverwood. He wanders around there until something
gives him a follow package; ask him to come along through SkyrimNet. Works on a new or
existing save; a new game is the cleanest test.

Requirements
------------
- Skyrim Special Edition / AE with Dawnguard
- SKSE64 and Address Library for SKSE Plugins
- SkyrimNet (recent; built against the 1.1 devkit)
- SeverActions (his running about, sniffing and hunting use its travel package)
- PapyrusUtil SE

Optional: Frostfall (he presses close and warms you when you're cold).

Not included in this test build: the mood animations (head tilts, yawns, tail wags and
so on). They use animations from other mods that can't be re-shared. Everything else
works without them.

If you use BodyLanguage: it may describe Hjalti as naked or comment on his body. That's
a known gap in this build; his own prompt tells him clothes mean nothing to a dog.

Settings
--------
SkyrimNet dashboard > Settings > Plugins > Canine Drives. Every number is adjustable and
changes apply within a few seconds, no restart needed. Worth knowing:

- Carcass Meat (Feeding): 0 = he only eats animals (default), 1 = people too while you
  wear the Ring of Namira or are a werewolf, 2 = people too, always. Never children.
- Take Owned Food and Owners Charge You (Events), both on by default: when he's hungry he
  helps himself to food off people's tables or out of their packs, and the game counts it
  as YOUR theft, since a dog can't be fined. Expect bounties and angry innkeepers. Turn off
  Owners Charge You to keep the snacking without the crime, or Take Owned Food to stop it.
- Damage Taken / Bite / Armor / Health (Progression): tune how strong he gets.
- Set Drives (Debug): for testing, e.g. hunger=80 or souls=13 (souls=13 unlocks every
  gift at once; load a save from before to undo it).

Reporting
---------
Please send:
- What happened, and roughly when (game time or real time).
- Documents\My Games\Skyrim Special Edition\SKSE\CanineDrives.log
  (it's rewritten each launch, so copy it before restarting the game)
- Your SkyrimNet version and whether you use SeverActions' follow or another follower
  framework.

Credits and licence
-------------------
Canine Drives (CanineDrives.dll) is GPL-3.0-or-later; see SKSE\Plugins\CanineDrives_LICENSE.txt
and CanineDrives_THIRD_PARTY_NOTICES.md. Hjalti's meshes and textures are based on the
vanilla Dawnguard husky. Requires, and does not include, Skyrim, SKSE and SkyrimNet.
