# Canine Drives — Design

Give Valiant real dog needs: five drives that rise and fall with what happens in the game. The drives
shape his prompt, choose which dog actions he can take, and feed a JEV decision loop that picks what
he does between conversations. Physical needs placed late in the prompt should outweigh the human
character framing in the rest of the payload (bio sections, court politics, exposure prompts).

Status: design only, nothing built. 2026-11-01.

## Scope

- **Valiant only** for now: `DragonsKeep-BE.esp|0xE21FBD` (`00DragonsKeepHuskyArmoredValiant`),
  race `DLC1HuskyArmoredRace` (Dawnguard). That race uses the vanilla dog behavior graph
  (`Actors\Canine\Behaviors\DogBehavior.hkx`), so every vanilla dog idle works on him.
- The target actor is set in config (one FormID), not hard-coded, so a separate general dog
  companion mod later only needs a list instead of one entry.
- Same shape as Pulse and Frostfall Awareness: an SKSE DLL plus a SkyrimNet layer
  (`external/nosimo.canine_drives/`) and a dashboard settings page (`Plugin_CanineDrives`).

## What stays as it is

The working speech setup isn't touched: the bio speech style ("an ordinary dog, not a human mind in a
dog's body…") plus the channel facts in `0481_canine_voice.prompt` (aloud = dog sounds, telepathy =
simple thoughts in words). Drives add reasons for dog behavior; they don't replace the speech fix.
SkyrimNet's Universal Translator rule isn't needed: it was retired after a long stretch of play with it
off and no word leaks (2026-11-11).

## The five drives

Each drive is a float, 0–100, where **higher = stronger need**. All rates are per **game hour**, so
waiting and sleeping in-game count, and every number is a dashboard setting. The starting values below
are guesses to tune in play.

| Drive | Rises from | Falls from | Rise/game h |
|---|---|---|---|
| **Hunger** | time; activity speeds it up (×1.5 after combat or long runs) | eating food (by item size), a kill in combat (small) | +4 |
| **Affection** (social) | time, faster when Saga ignores him | petting, praise, play; scolding *raises* it a little (unsettled) | +3 |
| **Restlessness** (activity) | time indoors or standing still | travel distance, sprinting, combat, play, sniffing around | +5 idle, ~0 travelling |
| **Tiredness** (sleep) | awake time; combat and long runs add more; night ×1.5 | sleeping (lying in the sleep pose), slowly while lying awake | +3 |
| **Separation** (loyalty) | distance from Saga (none within 1500 units; fast when she's out of the cell) | being near her; touch (petting) resets it faster | +20 apart |

*Loyalty* as a fixed trait already lives in the bio. The drive version is **separation**: the
need to be with her right now. That's what actually moves dog behavior (whining at doors, sticking
close after time apart).

### Tiers

The prompt and actions read tiers, not raw numbers:

| Value | Tier | In the prompt |
|---|---|---|
| 0–39 | content | nothing (render nothing when there's nothing to say) |
| 40–64 | noticeable | one light fact |
| 65–84 | strong | a fact that colors everything he does |
| 85–100 | urgent | the fact leads; it outranks almost anything else on his mind |

Only the **two strongest** non-content drives go into the prompt, so it stays short and the strongest
need wins. Hysteresis: a tier drops only after falling 5 below its threshold, so it doesn't flicker
at a boundary.

### Persistence

The drives are saved in the SKSE co-save (serialization interface, one record per tracked actor).
Elapsed game time is applied on load (capped at 24 game hours, so a long-unplayed save doesn't wake
up with a starving dog).

## Inputs: how the game moves the drives

All inputs are sinks or polls in the DLL. **Nothing depends on an optional mod.**

| Input | How it's detected | Effect |
|---|---|---|
| **Petting** | With *Pet The Dog*: its `AR_DogUp` global goes to 1 or 2 while petting (polled in the tick). Without it: `TESActivateEvent`, player → Valiant, with no weapon or spell drawn | Affection −25, Separation −15; 60 s cooldown |
| **Feeding** | `TESContainerChangedEvent`: a food item (`VendorItemFood` keyword, or raw meat) moves into Valiant's inventory. He eats it right away: the item is removed and an eat idle plays | Hunger −15 to −40 by item weight/value |
| **Talk-to-feed** | The player says "here boy, have this sweetroll" → the perception decision (below) says `feed` and names the item → the DLL moves one matching food from Saga's inventory to his, which then counts as Feeding | as Feeding |
| **Praise / scolding** | The perception decision on the player's spoken lines to Valiant | praise: Affection −10; scold: Affection +5 |
| **Travel / sprint** | position poll every 5 s; distance moved while following | Restlessness falls, Tiredness rises |
| **Combat** | combat state on/off, kills by Valiant (`TESDeathEvent` with killer = Valiant) | Restlessness −30, Tiredness +10, Hunger ×1.5 for 2 h; kill: Hunger −5 |
| **Sleep** | Valiant held in the lying sleep pose by the Nap behavior | Tiredness −15 per game h |
| **Distance** | distance to the player, and same-cell check | Separation |

**Feeding:** vanilla gives animal followers no inventory screen, and SeverActions' `GiveItem` is the
*speaker* handing something over. That's Valiant giving, not Saga. **Animated Dog Feeding SE** fills
the gap (see Mod integrations): its dialogue and lesser power call `ShowGiftMenu` with its food list,
so the food moves from Saga's inventory into the dog's (a `TESContainerChangedEvent`), and then the
dog eats it (`EquipItem`). The container-change rule above catches that with no integration code.
Talk-to-feed stays for food it doesn't cover, and for players without it.

## SkyrimNet integration

The rules from Min's example plugin apply (see the memory note): one bridge .cpp includes
`PublicAPI.h`; features are decided by resolved pointers; capture FormIDs, never `RE::Actor*`;
decorator names are prefixed; static CRT `/MT`; decorator calls go in a file-by-file folder, never
`character_bio`.

### 1. Decorators (`PublicRegisterDecorator`)

Callbacks run on SkyrimNet worker threads, so they read **atomic cached values only**, written by the
tick. Each returns `""` for any actor that isn't tracked.

| Decorator | Returns |
|---|---|
| `canine_hunger`, `canine_affection`, `canine_restless`, `canine_tired`, `canine_separation` | the value as an integer string, for action eligibility rules |
| `canine_needs` | the prompt text for the top two drives, already worded (see below), or `""` |
| `canine_state` | `"awake"`, `"napping"`, `"eating"`, `"playing"` |

### 2. Prompt (`user_final_instructions/0480_canine_needs.prompt`)

It sits near the end of the payload.
It's guarded so it renders only when the decorator exists and returns text. Facts, not instructions
(the inform-not-direct rule):

```
{% if npc.UUID and canine_needs(npc.UUID) %}
## What Valiant's Body Wants Right Now
{{ canine_needs(npc.UUID) }}
{% endif %}
```

Example render, for strong Hunger plus noticeable Tiredness:

> Valiant is very hungry. His stomach growls, and his nose keeps turning toward any smell of meat
> or bread. He is also getting sleepy; he yawns, and his steps are slower.

The wording lives in the DLL settings as a table per drive and tier (3–4 variants each, picked at
random), so it can be tuned without a rebuild.

### 3. Dog actions

A custom category, **DogBehavior**. Each action is registered from the DLL with
`PublicRegisterCPPAction`, so there's no Papyrus and no ESP. Eligibility callbacks read the cached
drives (thread-safe). Execute callbacks run on the game thread, so they stay quick: send an
animation-graph event, start or stop a held state.

| Action | Eligible when | Does |
|---|---|---|
| **AskForFood** (beg) | Hunger ≥ 50, Saga within 600 units, not in combat, no food already in his inventory | sit facing Saga + whine (`IdleDogSit`, `DogBarkSittingTalk`), then open the vanilla gift menu filtered to food; declining is fine |
| **SeekAffection** | Affection ≥ 50 | approach Saga, sit or lie at her feet, whine |
| **Nap** | Tiredness ≥ 60, not in combat, indoors or at camp | lie down (`IdleDogLay`) and hold the sleep state until woken or Tiredness < 15 |
| **Play** | Restlessness ≥ 55, Tiredness < 70 | zoomies: a short run around Saga (move-to points), barking (tail-chase / roll-over would need our own clips, DogAnimKit) |
| **Sniff** | Restlessness ≥ 40 | wander to a nearby object or person, pause, move on |
| **StayClose** | Separation ≥ 50 | hold close follow distance; whine if Saga moves away |
| **Warn** | a stranger or hostile nearby | growl (`DogIdleWarn`) |
| **Eat** | food in his inventory and Hunger ≥ 20 | eat it (see Feeding) |

Dialogue-driven use: SkyrimNet offers these to the model in conversation like any action, so "come
lie down, boy" over telepathy can fire Nap.

### 4. JEV: perception decision

When the player speaks to Valiant (or near him, naming him), via `PublicRegisterEventCallback` on
`dialogue_player`, `dialogue_player_stt`, `dialogue_player_text` and `dialogue_player_telepathy`:
send `canine/perception` to the decisions model with the line. The callback runs on a SkyrimNet
worker, so no `RE::` calls there; hand the work to our own worker.

Template `prompts/decisions/canine/perception.prompt` asks one choice question:
`praise | scold | feed | play | sleep_command | other`, plus the food item named for `feed`. The
result adjusts drives (praise/scold), runs Talk-to-feed, or nudges the next behavior. It costs one
JEV call per line *addressed to Valiant*, not per line in the room.

### 5. JEV: the behavior loop (his life between conversations)

Every 45–90 real seconds (random, so it doesn't look mechanical), when it's appropriate:

1. **Skip** if Valiant is in combat, mid-conversation, already in a held behavior, or SkyrimNet is
   busy: dialogue involving him in the last ~15 s (tracked through the dialogue event callback) or
   `PublicIsActorBusy`. Follow the conversation-scheduler notes; never interrupt speech.
2. **Ask** `canine/next_behavior` with the drives (values + tiers), the scene (indoors, time, Saga's
   state: sitting/sleeping/moving, nearby people, last behavior and how long ago).
3. **Get** one choice: `idle | beg | seek_affection | nap | play | sniff | stay_close`, with
   probabilities. Act only above a confidence floor (0.5); `idle` is a valid, common answer.
4. **Do** it through the same execute code as the actions, then post a short-lived event
   (`PublicRegisterEvent`, short-lived, audience nearby) so SkyrimNet characters know: "Valiant curls
   up by the hearth and dozes off." Bystanders can react, and his own memory gets a dog-level
   record.

Rate limits: at most one behavior per loop tick, and no repeats of the same behavior within 5
minutes unless the drive is urgent.

### 6. Held states and waking

Nap, Beg and SeekAffection hold an idle. The DLL tracks the state and ends it when Saga moves more
than 800 units away, combat starts, he's activated (petted awake), or the drive clears. It ends it
with `DogSitIdleStop` / `DogIdleStop` (`IdleStop` / `Idle_Out` graph events). If Saga leaves while he
naps, he wakes and follows; Separation keeps it believable.

## Animations

### Vanilla (in `Skyrim.esm`, no download)

| Idle | FormID | Graph event | Use |
|---|---|---|---|
| `IdleDogSit` | `10F7FF` | `idleSitStart` | Beg, waiting |
| `IdleDogLay` | `10F7FE` | `idleLayStart` | Nap (sleep pose), SeekAffection |
| `DogBarkSittingTalk` / `DogBarkLayTalk` / `DogBarkStandingTalk` | `0BF209` / `0BF208` / `0BF207` | `idleDog…BarkStart` | barking in any pose |
| `DogIdleWarn` | `037F8F` | `aggroWarning2Start` | Warn (growl) |
| `DogSitIdleStop` / `DogIdleStop` | `0960E2` / `0960DF` | `Idle_Out` / `IdleStop` | end a held pose |
| `DogDeathWait` | `0EA500` | `DeathAnimation` | **test only** as a deeper sleep pose |

**Sleep:** there's no dedicated sleep animation. `IdleDogLay` is the safe sleep pose. `DogDeathWait`
is a death-pose idle; it may read as "sprawled out asleep" but could trip death-related logic in
other mods (Acheron, SkyrimNet's own death handling) and needs `DogGetup` to recover. Test it alone
before using it.

There's also a vanilla **`DogLayingDownIdleMarker`** (`10F801`), an idle marker dogs lie down at. Nap
can place one beside Saga's bed or the hearth, which is how A Dog's Life makes its dog beds work.

## Mod integrations (surveyed 2026-11-01)

**Rule (user, 2026-11-01): no dependencies on other mods.** Every core feature is built on vanilla
records, vanilla idles, SkyrimNet and our own DLL. The one allowed dependency is **SeverActions**
(very popular with SkyrimNet users and actively developed). iActions' `receive_food` is fine while
we test, but our own **AskForFood** action replaces it. Everything below is an optional bonus:
detected when present, never required.

| Feature | Our own route (core) |
|---|---|
| Feeding | **AskForFood** action: he begs (sit + whine), and the DLL opens the vanilla gift menu (`RE::GiftMenu::OpenMenuAsGifter`, filtered to food through a runtime form list), so you choose what to give. Talk-to-feed as well. Food arriving in his inventory from *any* source lowers Hunger and is eaten |
| Eating | Vanilla `IdleDogLay` + a lay-bark, then the item is removed. Optional: Animated Dog Feeding's props when installed |
| Petting | `TESActivateEvent` with empty hands; a short vanilla reaction (sit, wag via bark-sit idle). Optional: Pet The Dog's global when installed |
| Idle variety | Vanilla idles, swapped by mood through our own OAR submods (vanilla clips, plus DogAnimKit's sleep styles) |
| Naps | Vanilla `DogLayingDownIdleMarker` + `IdleDogLay` |

| Mod | What it gives us | How we use it | Permissions |
|---|---|---|---|
| **Animated Dog Feeding SE** (Nexus 130152) | Feeding dialogue + lesser power; eating animation with food props; "well fed" spell | Its gift menu moves food into the dog: our container-change sink lowers Hunger by food. Hold our behaviors while it plays (global `aaDogEatAnimBusy`, `DogEatingAnims.esp|0x811`, plus ~8 s after food arrives) | Asset use with credit; no re-upload as-is; no selling |
| ~~Casual Dog Animations (132398) + Playful Dog Animations~~ | DROPPED 2026-11-13: their authors couldn't be reached, so none of their clips are used or bundled (the head-tilt, yawn, groom, tail-wag, tail-chase and roll-over swaps are gone). Our OAR swaps use vanilla clips only; new moves are made in `tools\DogAnimKit` | — | — |
| **Pet The Dog** (47829) | Petting on activation with empty hands (perk entry). Its "animations" are vanilla player idles (kneel, search body, greybeard teach) re-pointed through DAR | Detect petting by its global `AR_DogUp` (`PetTheDog.esp|0x803`, 1 or 2 for ~3 s): poll it in the tick. A perk activation may not raise `TESActivateEvent`, so this is the reliable signal. Without the mod, fall back to `TESActivateEvent` | Needs the author's permission to bundle: soft dependency only |
| **A Dog's Life** (94642) | Its own Happiness and Hunger needs, dog beds, ball fetch, "find food/keys/ammo" commands, harnesses and dog armour | **Don't enable it alongside**: two needs systems would fight, and its follower dialogue targets vanilla dogs. Borrow ideas only: the bed (idle marker + furniture), ball fetch (Play), and "find" commands (future SkyrimNet actions such as Sniff Out Food) | Multi-author assets; skip |
| **Dog Backpacks** | — | Not surveyed yet | Mixed contributors |

Worth knowing: the vanilla dog idles are the same graph events these mods use (`IdleDogBarkStart`,
`IdleDogLay`), so anything we fire picks up their variants automatically once they're enabled.

## Settings page (`Plugin_CanineDrives`)

Target actor FormID; per-drive rise rate and input effects; tier thresholds; prompt wording table;
loop interval range and confidence floor; enable/disable each piece (prompt, actions, perception,
loop); a debug mode that logs every drive change and decision to `CanineDrives.log`.

## Build and layout

`H:\CSVO\tools\CanineDrives\`, same layout and toolchain as Pulse: CommonLibSSE-NG, vcpkg, MSVC 2022
Build Tools, Ninja. Use the static CRT (`x64-windows-static` + `MultiThreaded`) per Min's advice, and
check `dumpbin /dependents` before shipping. Runtime 1.6.1170, SKSE 2.2.8. Run content-validate on the
layer.

```
CanineDrives/
  src/            DLL (bridge.cpp holds PublicAPI.h; drives, sinks, tick, actions, loop)
  layer/nosimo.canine_drives/
    manifest.json
    prompts/submodules/user_final_instructions/0480_canine_needs.prompt
    prompts/decisions/canine/perception.prompt
    prompts/decisions/canine/next_behavior.prompt
    settings/CanineDrives.yaml
  oar/            OAR mood swaps (tools/gen_oar.py; vanilla clips only)
```

As with every SkyrimNet change here, prompt text goes to the user to paste in the dashboard rather
than being written into `overlay/` or `library/` directly. The layer ships inside the mod as
`external/`.

## Milestones

1. **Drives core:** tick, co-save, decorators, the 0480 prompt, debug log. *Test: drives move
   over game time; the prompt shows hunger after a few hours.*
2. **Inputs:** petting, travel, combat, distance, sleep; Feeding via container change if Dragon's
   Keep allows an inventory. *Test: petting drops Affection, a long walk drops Restlessness.*
3. **Actions:** the DogBehavior category with vanilla idles and held states. *Test: "lie down boy"
   triggers Nap, and he wakes when Saga leaves.*
4. **Perception decision:** praise, scold, talk-to-feed. *Test: "good boy!" lowers Affection;
   "have this sweetroll" moves the sweetroll and feeds him.*
5. **Behavior loop:** autonomous behavior with the busy check and events. *Test: left alone at the
   keep, he begs, naps, sniffs around in plausible order.*
6. **OAR polish:** drive-linked variants from our own clips (DogAnimKit); no third-party animation packs.

## Ideas for later

- **Food preferences** (user, 2026-11-01). Each food gets an affinity score, saved in the co-save.
  Seed 2–3 random favorites and one dislike from dog-plausible foods on first run. Learn from meals:
  affinity rises when he eats while very hungry, is petted or praised around the meal, or the decision
  model's mood is happy; it falls when he's fed while full or something bad follows. Favorites remove
  more hunger plus some affection need, show as a prompt fact ("Valiant adores salmon"), and sort
  first in AskForFood. Builds on the milestone 2 eating code; fits after milestone 3.

- **Favorite places** (user, 2026-11-01). Places build affinity from good moments there: meals,
  pets, naps, play. SkyrimNet's named places (`PublicGetPlaceAt`) give spots inside a location such
  as "the hearth"; otherwise the location itself. At a favorite place, Affection and Separation rise
  more slowly (he's content there, even when left alone), and the prompt can say so ("Valiant loves
  the rug by the hearth at Highreach Keep").
- **Needs while dismissed** (user, 2026-11-01). Detect when he's not following (not a player teammate,
  or SeverActions' follower state) and use a separate set of rates: at home, Hunger is capped (the
  household feeds him), Tiredness falls at night (he sleeps), Restlessness rises (cooped up), and
  Separation rises but is capped below urgent at a favorite place or home, so a reunion still matters
  without him being frantic every time you leave him. Reunion after a long absence can give a
  burst of joy (Separation reset plus an event).
- **SeverActions life events** (checked 2026-11-01). SeverActions already writes dog-only life events
  for dismissed animal companions (`sever_offscreen_life.prompt`: `isAnimal` from the race name, and
  Valiant's displays as "Dog"; animal-only activities, no consequences). Each one is posted as a
  `persistent_generic` event, "Valiant (while Saga was away): …". Integration: catch those events with
  `PublicRegisterEventCallback("persistent_generic")` and let one decision-model call map each to drive
  effects (scraps at the inn → Hunger down, a nap by the hearth → Tiredness down, petted by locals →
  Affection down, chasing rabbits → Restlessness down). SeverActions' native API also tells whether
  he's dismissed, for the dismissed rates above. Not planned: pushing drives *into* its life prompt
  (only possible by overriding its prompt or through Bio Blocks).
  Possible requests to SeverActions' author (responsive but busy, so nothing here may block us):
  an extension point in the life prompt (a submodule folder other plugins can add context to, e.g. a
  dog's current needs), and a mod event or native callback after a life event is generated, carrying
  the actor and summary as data (cleaner than parsing the persistent event's text).

- **Temperament: daily drive-rate drift** (user, 2026-11-07). Once per in-game day, one decisions call
  nudges his drive rates based on how his day went, so his temperament slowly fits his life.
  - **When:** by the game clock, not sleep (not everyone runs a needs mod). The first quiet moment
    after the day rolls over (GameDaysPassed crosses a whole day), in the same busy check as the behavior
    loop. A multi-day skip (waiting, fast travel, a long dismissal) makes one call covering those days,
    not one per day.
  - **Tally (DLL, co-save):** hours near the player / apart / dismissed, distance travelled, fights,
    meals and how hungry he was, pets, praise and scolds, naps and sleep, and the hours each drive spent
    at strong or urgent. While he's dismissed, SeverActions' life events (`persistent_generic`, "Hjalti
    (while … was away): …") go in as text lines, so his days away count too (see the life events idea
    above).
  - **Decision:** template `canine/temperament` gets the tally, the current modifiers and his bio, and
    answers raise / lower / keep per drive. No numbers from the model.
  - **Applying it:** a fixed step per answer (default ±5%), each modifier clamped (default 0.75–1.33) and
    multiplied onto the slider rates, which stay the baseline. "Keep" drifts a modifier a little back
    toward 1.0, so old habits fade.
  - **Visibility:** one log line a day ("Temperament: restless up (lots of travel, few naps), separation
    down"), and a prompt fact only when a modifier is clearly off 1.0 ("He's grown restless lately"),
    informing, not directing.
  - **Settings:** toggle, step size, limits, and a debug command to reset the modifiers or force a call.

## Open questions

- Does Dragon's Keep give the huskies an inventory screen? This decides whether container-change
  feeding works directly.
- Is `DogDeathWait` safe as a sleep pose with Acheron installed?
- Movement for Play/Sniff: SkyrimNet's package API (`RegisterPackageEx`, package contract) or
  direct `MoveTo`-style travel packages. It needs a short spike in milestone 3.
- JEV cost and latency on `typesafe/jev-1.13` for two new templates: measure in milestone 4 before
  turning on the loop.
- Should the drives also feed his diary? A dog diary built on "hungry, tired, the Queen
  scratched my ears" could reinforce the canine voice over time.

### Frost Howl (built 0.26.0, 2026-11-10)

Replaces the Ice Block idea, which was dropped: a dog that hides in ice then does it again (like ice mages in PvP) isn't fun to have along. Frost Howl is aggressive instead, a small Thu'um of his own from the dragon souls. He howls, frost bursts from him, and every foe close around him is knocked down where they stand. It buys him a second or two while they get back up, and a ring of enemies dropping is an unmistakable sign that he's in trouble, far easier to notice mid-fight than the escape notification.

- Slot: his last gift for now, a seventh step after Frost Bite III (`kTiers` goes from 6 to 7; with Souls per Rune at 2, that's the 13th soul). Perseverance stays at tier 2.
- Knockdown in place, not a throw: Papyrus `ObjectReference.PushActorAway(target, force)` called on him, with a near-zero force so they fall where they stand (a common modding trick). So there's no flight distance to balance, only how often it fires. Called per target by us, never as a spell's area effect.
- Enemies only, with the combat filter already used elsewhere: foes fighting us, never the player, companions, bystanders, anyone he's spared, or children.
- Trigger: he's swarmed (two or more foes within about 300 units), or he's badly hurt with a foe close. Not while he's in bleedout, ragdolled, midair or swimming.
- Look and sound: the wolf howl he uses to sing along with bards (NPCWolfHowl 0D8D4B), and a short frost burst on him. Pick the shader in game; FrostShortFXShader 0435A3 (Frost Step's) is the fallback.
- Cooldown about a minute or two (a setting), shorter at the quicker-cooldowns step if that's still the rule. On/off setting. No damage, so it buys time without winning fights for him.
- Quiet event only, no narration, like Frost Step. His gifts fact in the prompt gains a clause once he has it.
- To check in game: which force value drops people without moving them; whether dragons, giants and mammoths go down at all (fine if they don't); how long people stay down with the user's combat and ragdoll mods; how fair it feels.

### NPCs petting him (built 0.25.0, 2026-11-08)

SN action `PetDog`, for anyone but him. Toggle `events.npc_pet`, cooldown `events.npc_pet_cooldown`. Deliberately no travel package on other NPCs, so nothing can go wrong with their AI: it's only offered to a person already within 220 units, standing still, weapon away, not sitting or in a fight, while he's calm and not roaming or napping. They turn to him, play `IdleSearchBody` where they stand for 2.5 s, and he sits and barks. Affection drops by `petting.affection`; separation doesn't, since that's about the player. A quiet event goes out, with no narration. Children aren't excluded (the user is fine with it), but the kneel may not play on child skeletons. Not yet seen in game.

### Hunting (built 0.24.0, 2026-11-08)

Hunt and eat-from-carcass went in together, as one feature. Toggle `events.hunting`, cooldown `events.hunt_cooldown`.

- Hunt (SN action `Hunt`, behavior loop `hunt`): the nearest animal within 3000 with aggression 0 and no crime faction, not a companion. Race editor IDs containing cow, chicken, domestic, horse, dog, husky, mammoth or netch are never prey (matching by name catches modded farm animals; wild `GoatRace` stays fair game). Eligible at hunger 50 or restless 55. Setting off clears his restlessness, caught or not, and tires him a little, as Play does. Only animals are ever prey; people can only be attacked through AttackTarget. He tracks it by scent on the run package with no combat, because a fight from the start made other followers kill the prey (seen 0.24.0). Combat starts only at the pounce: within 200 units, or after 3 s within 1200 units when Frost Step is ready (he has the gift, it's off cooldown, line of sight). Hunting comes before Frost Step and Frost Bite, so the 200-unit pounce is the normal case. His first bite then kills it (Papyrus `Actor.Kill` with him as killer), at any tier, so companions get almost no time to join in. Prey that bolts anyway means a chase, with companions free to join, until it's caught or he loses it.
- Eat from a carcass (SN action `EatCarcass`, behavior loop `eat_carcass`, and automatically after a kill): he goes to the body, sniffs, lies down for 8 s (`IdleDogLay`, as with meals; vanilla dogs have no eating animation), gets a narration, then hunger drops by `feeding.carcass_meal`. No meat item, so this works with Hunterborn and the like. A body is eaten once per session.
- `feeding.carcass_meat` (default 0): 0 animals only; 1 auto, people too while the player wears the Ring of Namira (02C37B) or is a werewolf (`PlayerIsWerewolf` global 0ED06C, or `WerewolfBeastRace` 0CDD84); 2 any meat. Never children at any setting. Also never companions, ghosts, dwarven machines, undead other than vampires, dogs, horses, spiders, chaurus or netch.
- People get the werewolf's feeding effect (art `BloodSprayWereWolfEat01` 0F3A8A on the body, sound `NPCWerewolfFeeding` 101009); animals don't.
- Losing the trail: an order on an unreachable target made him pace as if casting for the scent, but that needs combat. Stalking recreates it: after 8 s with no headway he swings side to side across the trail for 10 s (a marker moved every 3 s), sniffing. Then, or at 45 s in all, the prey unloading, or 6000 from the player, he gives up with a whine and a quiet event ("loses the fox's trail").
- Seen in game on 0.24.0: chase, kill, sniff, eating and the hunger drop all work. The stalk, pounce, killing bite and casting about (0.24.1) aren't seen yet, nor the blood on people.

### Progression follows the player (0.25.2, 2026-11-08)

His growth uses the higher of his own level and the player's, because his own level stalled at 30 with the player past 45, on a record set to 1x player level, max 100, with no patch changing that. The reason it stalled isn't confirmed. His base stats still come from the game's own level; only our bonuses, size and perks follow this.

### Hjalti placed in Riverwood (2026-11-08)

`SN_Dog.esp` now has a persistent placed reference, `HjaltiRef` 000818, in Tamriel's persistent cell (000D74) at 23170.4, -44396.1, 20: about 120 units out from the Sleeping Giant's front door arrival marker, which is reliably walkable. The NPC has one package, `DefaultSandboxEditorLocation512`, so he stays within 512 units of that spot until a follow package takes over. Placeholder until his story is decided. Backup from before this: `backups/SN_Dog.esp.before-riverwood`. Masters are unchanged (Skyrim.esm, Dawnguard.esm). His bio is `hjalti_818.prompt` (SkyrimNet has no catch-all bio name: `hjalti_generic` was never picked up, seen 2026-11-09). It's in a load order slot below the plugins that win the cell and worldspace, so it doesn't change them there; for release, note that it carries vanilla copies of both records. Not yet seen in game.

### Idea: Sightings (2026-11-09)

Give him some history before the player meets him: on a new save, a handful of NPCs across Skyrim get a vague memory of having seen a big husky. Nothing makes them say anything; it comes up on its own if the talk turns to dogs.

- Tool: personal memories through `PublicAddMemory` (as Dreams uses), low importance so they don't crowd out real ones. World knowledge packs (shared rumours with conditions such as hold or faction) are an option for later.
- Who: about a dozen or two NPCs picked at random once per save, weighted toward people who'd see a dog out in the world: hunters, guards, carriage drivers, innkeepers, travellers, farmers. A co-save flag stops it running twice. Never children.
- What varies: size (small to very large, with the odd unreliable witness), runes (none, a few, all of them glowing), place, time of day, and what the dog was doing (hunting, sniffing round a camp, watching from a ridge, gone before anyone got close).
- Wording stays vague: "a big grey husky", never his name, and no facts about where he came from. That way the memories fit whatever backstory comes later, including a planned add-on with an LLM-generated backstory that's different each playthrough.
- Settings: on/off and how many. Off for an existing save unless the player turns it on, since added memories can't easily be taken back out.
- To check before building: how SkyrimNet dates and types a memory added this way, so these read as old sightings rather than something that just happened.

### Hjalti's starting stats (2026-11-10)

`HjaltiRace` (SN_Dog.esp 000806) now starts at 70 health and 70 stamina, up from the vanilla dog's 10 and 20, because at level 2 he had only 20 health and 25 stamina. The race is his alone, so no other dog changes. Auto Calc Stats still adds his level growth on top, and Canine Drives' progression bonuses start at level 5. Backup from before: `backups/SN_Dog.esp.before-race-stats`. Not yet checked in game.

Frost Howl as built (0.26.0): tier 7 (`kTiers` 7), settings `runes.frost_howl`, `runes.frost_howl_cooldown` (90 s, x0.6 from tier 5) and `runes.frost_howl_force` (2.0, 0-50). The foe filter is `Combat::Engaged` plus not the player, not a teammate, grounded, not already down, within 300. `PushActorAway` is dispatched through Papyrus on him, trying class name "Actor" then "ObjectReference"; the log says if neither took. Shader for now: FrostShortFXShader 0435A3 for 2 s. Log line: `Gifts: Frost Howl (...)`. Not yet seen in game.

### Speech and memory whitelists (2026-11-10)

`filters/hjalti_speaks.yaml` (kind actor) and `filters/hjalti_memories.yaml` (kind memory) whitelist `HjaltiRace`, on the pattern of SkyrimNet's base `paarthurnax_speaks.yaml`, so he still speaks with SkyrimNet's "only speech-capable races may speak" on, and still forms memories where animals are kept from it. SkyrimNet's base `animals_dont_speak` / `animals_no_memories` blacklist DogRace, WolfRace and ACLRaceCat, not HjaltiRace, so those never applied to him. Each file is its own toggle on the dashboard. Not yet tested with the speech-capable setting on.

### Prey grows with him (0.26.1, 2026-11-10)

What he hunts is gated by his growth level (`Progression::GrowthLevel`, the higher of his level and the player's): small game (hare, rabbit, fox) at any level, deer-sized game (deer, elk, wild goat, boar) from level 10, predators (wolf, bear, sabre cat, horker, troll, and anything aggressive) from level 30. A predator hunt also needs him at 75% health or better. Races are sorted by editor ID; a race none of those names, such as a mod's animal, is sorted by its health (20 or less small, up to 80 medium, else predator). Never prey: everything in `kNotPrey` (now also spiders, chaurus and the White Stag) and anything essential or protected. The killing bite applies only to small and medium game; a predator is a real fight that he can lose. Not yet seen in game.

Frost Step look (0.26.1, 2026-11-10): ShieldSpellFXS 056AC9 (frosty sparks round him, short) for 2 s, applied just before he moves so it carries through to where he comes out. Chosen by the user in game with `pms 56AC9 2`; first noticed on Celestine's flesh buffs. FrostShortFXShader 0435A3 stays as Frost Howl's burst.

Perseverance look (0.26.1, 2026-11-10): HealMysticFXS 04E220 on him for the 5 s heal, alongside the rune glow. Chosen by the user. The three gifts now look different: Frost Step sparks, Frost Howl frost burst, Perseverance healing glow.

Frost Step departure (0.26.3, 2026-11-10): `DLC1BoltExplosionFrost02` (Dawnguard 010D91, no damage, force 30) is placed where he stands just before he moves, so his vanishing reads as frost breaking apart rather than a teleport glitch. Chosen by the user in game with `player.placeatme 2010D91`. The sparks still carry through to where he lands.
