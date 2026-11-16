Hjalti + Canine Drives 0.26.7 - PRE-RELEASE
============================================

An early public build: expect rough edges, and keep your saves backed up. Install it as one
mod with Mod Organizer 2 (Install a mod from archive) or Vortex.

What it is
----------
Hjalti is a husky companion (SN_Dog.esp). Canine Drives is the SKSE plugin and SkyrimNet
layer that gives him a dog's mind:

- Needs that rise and fall with time: hunger, affection, restlessness, tiredness, missing you.
- Things he does on his own when it's quiet: begs, naps (on his side, chin on his paws, or
  curled up tight in the cold), plays, play bows, wiggles with joy, rolls over, sniffs the
  ground, the air and the people he meets, sneezes, cocks his head at odd sounds, howls with
  bards, scavenges or steals food, hunts game he can take and eats what he catches.
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
existing save; a new game is the cleanest start.

Requirements
------------
- Skyrim Special Edition / AE with Dawnguard
- SKSE64 and Address Library for SKSE Plugins
- SkyrimNet 1.1 or later
- SeverActions (his running about, sniffing and hunting use its travel package)
- PapyrusUtil SE
- Open Animation Replacer (his own animations: rollover, play bow, wiggle, sleep and
  lying poses, sniffs, sneeze, head tilt, barks)

Optional: Frostfall (he presses close and warms you when you're cold).

If you use BodyLanguage: it may describe Hjalti as naked or comment on his body. That's
a known gap; his own prompt tells him clothes mean nothing to a dog.

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
- Kill Bite Multiple (Events): prey whose health is within this many of his bites dies to
  his first bite; anything tougher is a real fight.
- Damage Taken / Bite / Armor / Health (Progression): tune how strong he gets.
- Set Drives (Debug): for testing, e.g. hunger=80 or souls=13 (souls=13 unlocks every
  gift at once; load a save from before to undo it), or rollover / playbow / wiggle /
  sneeze to see a move now.

Reporting problems
------------------
Open an issue at https://github.com/Nosimo-SN/Hjalti/issues with:
- What happened, and roughly when (game time or real time).
- Documents\My Games\Skyrim Special Edition\SKSE\CanineDrives.log
  (it's rewritten each launch, so copy it before restarting the game)
- Your SkyrimNet version and whether you use SeverActions' follow or another follower
  framework.

Credits and licence
-------------------
Canine Drives (CanineDrives.dll) is GPL-3.0-or-later; see SKSE\Plugins\CanineDrives_LICENSE.txt
and CanineDrives_THIRD_PARTY_NOTICES.md. Source: https://github.com/Nosimo-SN/Hjalti

- Sound effects (barks, whines, growls, sniffs, sneezes, eating, and the Frost Step, Frost
  Howl and Perseverance sounds) were generated with ElevenLabs (elevenlabs.io). They are
  shared for non-commercial use only.
- Hjalti's meshes and textures are based on the vanilla Dawnguard husky. His animations
  were made by script from the vanilla dog skeleton and clips, using PyNifly
  (BadDogSkyrim) and serde-hkx.
- Built on CommonLibSSE-NG and SkyrimNet's plugin API.

Free and non-commercial. Requires, and does not include, Skyrim, SKSE and SkyrimNet.
