# M2b wave 2 contract (W2.0), as built

The integrator's contract commits for M2b wave 2 (docs/design/m2b.md §11 "Wave 2", §11.1 step 4), on branch `m2b/w2`
from `main` 545640c (wave 1 merged: nt, au, ru, bb and the integ-w1 insertions). They declare the wave-2 headers and
insertions of m2b.md §2.6 #16-#18 and the `m2b.contract2` row. Nothing gets a body: no wave-2 symbol is called by the
tree, so no stub is needed (the W1.0 codec stubs were the one exception, and they are gone). Every change is additive:
`git diff --stat main..m2b/w2` shows insertions only, no existing line changed.

Wave 1 was read as built: m2b-w1.md, m2b-nt.md, m2b-ru.md, m2b-bb.md, m2b-au.md, the `## M2b bb` and `## M2b integ-w1`
blocks at the end of m2b.md, and the code on main. Where m2b.md and the code disagree, the code was followed; §3 lists
each difference.

## 1. What the commits contain

| commit | track | what | m2b.md |
|---|---|---|---|
| 7a94820 | ui | `hud_model.h`: `HudKillCard`, `HudEndOfRound`, `HudSpawnScreen`, `HudPreRound`, the constants `kHudTopPlayers` 3, `kHudKitCount` 4, `kHudSpawnKitItems` 5 (a block before `HudModel`), and the four `HudModel` members `kill_card`, `end_of_round`, `spawn_screen`, `pre_round` after `team` | #16 |
| 8219947 | ui | `hud_feed.h`: `HudKillCardSource`; the `HudFeedInput` M2b members (struct-member insertion at its end); the end block: `fill_hud_tdm`, `HudTdmFeedState`, `kKillFeedSeconds` 5, `kKillCardSeconds` 4, `kHudSpawnSlots {0, 1, 2, 6, 7}` and the feed rules ui's body follows | #16 |
| 32c5c68 | ui | new `src/bf3_godot/src/hud_tdm.h`: `Bf3TdmHud` (a plain class with a pimpl, like `Bf3GameCrowd`) and `TdmHudRecords` | #16, §4 |
| 003dad5 | cam | `crowd_eval.h`: `CrowdSoldierInput::revived`, `CrowdStanceSet::corpse_seconds` (default `kCrowdDeathHideSeconds`), the end block with `kCrowdCorpseSeconds` 15 and `kCrowdReviveBlendSeconds` 0.5; `game_crowd.h`: `Bf3GameCrowd::set_corpse_seconds`; new `src/bf3_godot/src/death_camera.h` (`Bf3DeathCamera`, `DeathCameraParams`, `DeathCameraStart`, `DeathCameraView`, `DeathCameraStats`) | #17 |
| 5347068 | gx | new `m2b_game.h` (`M2bOptions`, `M2bLoadInputs`, `M2bFrame`, `kM2bPlayableDefaultBots` 31, `kM2bSpawnRequestRetryTicks` 30), `m2b_check.h` (`M2bCase`, `M2bTrack`, `parse_m2b_cases`, `M2bCaseResult`, `M2bCheckOptions`, `M2bCheck`), `level_session_m2b.h` (`M2bGame`); `M2aGame::m2b` (struct-member insertion); bf3_godot's CMake `# M2b W2.0` block (the headers, the TU `m2b_w2_headers.cpp`, `OPENBF3_M2B_HAVE_UI` / `_CAM` from the files' presence) | #18 |
| ef2310f | integrator | `tests/m2b_contract2_tests.cpp` and the tests CMake `# M2b W2.0` block: the `m2b.contract2` row | §11 |
| (docs) | integrator | this file and README "M2b wave 2 contract (W2.0)" | |

Every header W2.0 adds is Godot-free (Godot types only as forward declarations), so `m2b.contract2` compiles all of
them; `m2b_w2_headers.cpp` compiles them inside the GDExtension too.

### 1.1 Who writes which body, and the insertions each track makes

| owner | bodies (new files) | named insertions and replacements in other code (each line marked `// M2b <key> [tag]`, in their own commit) |
|---|---|---|
| ui | `src/bf3_host/src/hud_feed_tdm.cpp` (`fill_hud_tdm`, added to bf3_host in a `# M2b ui` block); `src/bf3_godot/src/hud_tdm.cpp` (`Bf3TdmHud`) and the six screens `hud_killfeed`, `hud_scoreboard`, `spawn_screen`, `kill_card`, `round_end`, `pre_round` `.{h,cpp}` (a `# M2b ui` block in bf3_godot's CMake, their classes in a `// M2b ui` block of `register_types.cpp`); `tests/ui_hud_tdm_tests.cpp` (`ui.hud.tdm.unit`, `ui.hud.tdm.install`) | none needed; sets `kUiLanded` in `m2b_contract2_tests.cpp` |
| cam | `src/bf3_godot/src/death_camera.cpp` (Godot-free: m2b.contract2 compiles it in when the file exists); `set_corpse_seconds` and the revive input in a `// M2b cam` block of `game_crowd.cpp`; a "revive" case in `anim.s3.unit` | `[corpse]` crowd_eval.cpp:809: the hide test `dead_age >= kCrowdDeathHideSeconds` reads the stance set's `corpse_seconds` (a named replacement); `[revive]` crowd_eval.cpp: the stand-up blend when `dead` falls with `revived`; `[corpse]` game_crowd.cpp:299: the corpse input's `anim::kCrowdDeathHideSeconds` becomes the configured seconds (a named replacement); `[revive]` game_crowd.cpp `step`: a `PresentKind::Revive` event (aux = the revived slot) sets `revived` for that slot's next life; `[defib]` the viewmodel set's slot-2 unlock when the defibrillator's 1p mesh resolves (viewmodel_set.h:274-281 resolves slots 0, 1, 6, 7; otherwise the defib is drawn without a viewmodel, accepted, m2b.md §4); sets `kCamLanded` |
| gx | `level_session_m2b.cpp` (the M2b session: the hook functions it declares itself), `m2b_check.cpp`, `scenes/m2b_metro.tscn`, `tests/godot_m2b.cmake` with the rows `godot.m2b.play` (E9, `window`) and `godot.m2b_load` (E12, `window`), README "M2b" | the tagged lines `// M2b gx [args]`, `[load]`, `[server]`, `[client]`, `[presented]`, `[frame]`, `[input]`, `[shutdown]` in the M2a hooks of `level_session_m2a.cpp`, one per hook (the M2b session is the M2a session with the rules on, m2b_game.h); additive blocks in `input_mapper.{h,cpp}` (Tab, Enter, the kit keys, KEY_3 -> slot 2 and slot 2 in the wheel order at input_mapper.cpp:279); **R13** `src/bf3_fx/src/triggers.cpp:1236` `FxTriggers::sane`: `<= PresentKind::Spawn` -> `<= PresentKind::Revive` (the integ-w1 open item: a Revive event is counted as rejected today, m2b-nt.md §7); `tools/play.cmd` (new defaults: the M2b scene, 31 bots; `--m2a` for the old scene; the `--tdm` preview stays) |

Bodies gx calls and the order of the M2b frame are in m2b_game.h's comment. gx builds ui's and cam's parts only
under `OPENBF3_M2B_HAVE_UI` / `OPENBF3_M2B_HAVE_CAM` (defined by bf3_godot's CMake when `hud_tdm.cpp` /
`death_camera.cpp` exist), so all three branches link alone and pick each other up after the merges without an edit.
The M2b check skips a row whose part has not landed with "waiting for ui" / "waiting for cam", never passes it.

## 2. The feed's rules (pinned in the header)

hud_feed.h's M2b block states what `fill_hud_tdm` writes for each screen, so ui's body, gx's check and the contract
rows agree on one reading. The ones that decide behaviour:

* **Players by slot.** `HudModel::players[slot]` (kHudMaxPlayers == net::kMaxPlayerRows == 64, pinned), so every
  `HudKill`, kill card, top-player and team-list index is a server slot and needs no mapping.
* **The spawn screen** is visible while the own soldier is not alive (or unspawned), no kill card is up and the phase
  is not RoundOver (the round end shows then). At join (`!spawned_this_round`) it shows at once, without the timer and
  the revive line. `can_deploy` = the server tick reached `own_respawn_tick` and the phase is PreRound or InRound.
* **The kill card** starts on the drained own Death event (kOwnVictim) and is visible for kKillCardSeconds of the
  presentation clock; the spawn screen follows it, which is the order of m2b.md §1.1 step 4.
* **Hostile inputs** (a row or event slot >= 64, a phase past RoundOver, ticks in the past) are skipped or clamped and
  counted in `HudTdmFeedState::bad_inputs`, never used as indices.

## 3. Differences from m2b.md (the code was followed)

* **`HudFeedInput` gets more than `match`, `players` and `kill_card_source`** (#16): also `m2b`, `match_valid`,
  `own_slot`, `own_team`, `server_tick`, `own_respawn_tick`, `own_revive_until`, `own_unspawned`,
  `spawned_this_round`, `spawn_kit`, `scoreboard_held` and `kits`. The feed is pure and Godot-free; the timers, the
  local player, Tab and the kit bar have no other way in. `fill_hud` ignores all of them, so the M2a HUD rows are
  unchanged.
* **`fill_hud_tdm` is a second function, not a change of `fill_hud`.** m2b.md says "an additive block in
  hud_feed.{h,cpp}". A separate function keeps `fill_hud`'s body untouched, and its body lives in the new file
  `hud_feed_tdm.cpp` so gx can tell from the tree whether ui landed (the M2a gx pattern of file presence).
* **One façade for the screens, `Bf3TdmHud`.** m2b.md lists ui's six screen files but no type gx builds. A plain
  class with a pimpl (as `Bf3GameCrowd`) lets gx build, feed and query the screens (records for E9, the spawn screen's
  clicks, whether the mouse is needed) without depending on ui's Control classes, which stay ui's to shape.
* **`DeathEvent` carries no position**, and neither does the Death presentation event (presentation_events.h: slot
  = killer, aux = victim, def, value = killer_health, extra = respawn_tick, seed = revive_until). The kill card's
  distance comes from the game: `HudKillCardSource` (the killer's presented position in the frame that drains the own
  death).
* **Deploy uses the server's clock, and gx resends.** ru refuses an early SpawnRequest silently (counted on the
  server only, m2b.md §3.4), and the client's own tick runs ahead of the server's. So `can_deploy` compares
  `server_tick` (PresentationFrame::newest_tick) with `own_respawn_tick`, and gx sends the request again after
  `kM2bSpawnRequestRetryTicks` (30) without a Spawned [inferred].
* **Where the 15 s corpse is chosen** (#17 names only `kCrowdCorpseSeconds`): `CrowdStanceSet::corpse_seconds`
  (default `kCrowdDeathHideSeconds`, so M2a and S3 are unchanged) and `Bf3GameCrowd::set_corpse_seconds`, which gx
  calls before `build()` in the M2b scene. The data has `SoldierDef::time_for_corpse` (soldier_def.h:112); gx may pass
  it instead of the constant when it is > 0.
* **A corpse outlives its slot only if cam builds for it.** The crowd draws one 3p soldier per slot. A bot respawns
  5 s after its death (up to 8 s while it waits for a revive), so its corpse ends then, before 15 s, unless cam keeps
  a separate corpse instance. The 15 s hold applies as is to slots that stay dead (a human on the spawn screen). This
  is cam's call; either way it is accepted for M2b.
* **`ReviveEvent`'s id is the revive tick** (nt, m2b-nt.md §6), not the revived life; the crowd keys the stand-up on
  the event's `aux` (the revived slot) and that slot's next life.
* **`kHudModelVersion` stays 1.** The M2a fields keep their offsets (pinned: `team` ends at 8,860, M2a's size was
  8,864); only `ui2.contract` reads the constant, as "> 0".
* **gx's hook functions are not declared.** Only gx calls them (from its tagged lines in level_session_m2a.cpp), so
  they are gx's to declare, as bb declared its `m2b_bb_tdm_*` functions. W2.0 declares `M2bGame` and the
  `M2aGame::m2b` member that holds it.
* **The integ-w1 open item** (`FxTriggers::sane` rejects `PresentKind::Revive`) is gx's named replacement R13 in
  wave 2. It is one comparison in bf3_fx; gx owns the integration, and the M2b scene is the first that drains Revive
  events into the FX.

## 4. m2b.contract2

`tests/m2b_contract2_tests.cpp`, suite `m2b-contract2`, functional, parallel-safe, no install, no Godot. It links
bf3_host, bf3_anim, bf3_fx and bf3_render and has `src/bf3_godot/src` on its include path.

* **Shapes, at compile time.** `HudModel` is 9,024 B (M2a: 8,864): the new members follow `team` (which ends at
  8,860) in the order kill_card, end_of_round, spawn_screen, pre_round. `HudKillCard` is 32 B, `HudEndOfRound` 24,
  `HudSpawnScreen` 96, `HudPreRound` 12, with the offsets of their fields. Every new struct is trivially copyable,
  and `kHudMaxPlayers == net::kMaxPlayerRows`. The signatures of `fill_hud_tdm`, `Bf3DeathCamera::step` and
  `Bf3GameCrowd::set_corpse_seconds` are pinned, and `Bf3TdmHud` and `M2bCheck` are not copyable.
* **Constants and enums.** The W2.0 constants. `kHudSpawnSlots` = kKitSlots with kKitGadgetSlot.
  `kCrowdDeathHideSeconds` stays 3. `M2bCase` and `M2bTrack` values; the names and `present_flags` bits the feed
  reads.

The cases:

1. **The defaults of every W2.0 field keep M2a**: the hidden screens, `kHudNoPlayer`, `HudFeedInput{}.m2b == false`,
   `revived == false`, `corpse_seconds == kCrowdDeathHideSeconds`, the death camera's defaults (4 s = kKillCardSeconds),
   `M2bOptions` (deploy screen, 31 bots), an empty `M2bGame`.
2. **The M2b check's parser** refuses hostile command lines: "", "Play", a repeat, an empty item, "all" (M2a's word),
   an unknown case, 4,096 bytes.
3. **ui's `fill_hud_tdm`**: pending until `kUiLanded`. It is a recorded own death by slot 5 with a headshot. It
   checks the kill feed, the kill card (killer, kit, killer health, distance), the players by slot and the kill
   counter. The spawn screen comes after the card: not deployable before the respawn tick, deployable at it, revive
   possible. The round end on RoundOver, and every screen hidden with `m2b` off.
4. **cam's `Bf3DeathCamera`**: pending until `kCamLanded`. It is active after `begin`, faces the killer (cos > 0.9)
   after the turn and is finished at 4.1 s. A non-finite corpse ends the view and is counted.

Both pending rows compile today (`if constexpr` in a non-template still type-checks the branch), so a header change
that breaks them breaks the build.

## 5. Gates, one command each

* `tools\build.cmd dev`: the branch pass (no timing, realtime or window rows).
* `ctest --test-dir b\dev -R "^m2b\.contract2$" --output-on-failure`.

Measured results: README "M2b wave 2 contract (W2.0)".

## 6. Track worktrees

From `m2b/w2` at the last W2.0 commit:

* ui, cam and gx, each on branch `m2b/<key>` at `C:\Users\olku\BF3 reimplementation\wt\m2b-<key>`;
* submodules initialised with `--reference` to the main checkout's modules;
* `bf3-opensource/.godot/extension_list.cfg` written as in the other worktrees.

m2b.md §11.1 step 4 puts ui and cam in parallel and gx after them. gx's branch links alone through the
`OPENBF3_M2B_HAVE_*` switches, so gx can start at once: its scene, input and check need neither body. It merges last,
after ui and cam are on `m2b/w2`.
