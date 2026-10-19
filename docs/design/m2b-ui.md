# M2b ui: the TDM HUD screens (as built)

Track **ui** of M2b wave 2, branch `m2b/ui` from the W2.0 contract (`m2b/w2` 5191059). Design: [m2b.md](m2b.md) §4,
§2.6 #16, the ui row of §11 and E9. The contract ([m2b-w2.md](m2b-w2.md)) fixed `hud_model.h`'s M2b structs,
`HudFeedInput`'s M2b fields, `fill_hud_tdm`'s declaration and `Bf3TdmHud`'s header; this track wrote their bodies.
This file records what was built, the choices the data left open, the measured gates and one command per gate.

## 1. What landed

| piece | files | commit |
|---|---|---|
| `fill_hud_tdm`, the M2b half of the HUD feed | `src/bf3_host/src/hud_feed_tdm.cpp`, the `# M2b ui` block of bf3_host's CMake | 2e78880 |
| `Bf3TdmHud` and its six screens; the strings, fonts, icons and placements they look up | `src/bf3_godot/src/hud_tdm.cpp`, `hud_tdm_screen.h`, `hud_tdm_text.h`, `hud_killfeed.{h,cpp}`, `hud_scoreboard.{h,cpp}`, `spawn_screen.{h,cpp}`, `kill_card.{h,cpp}`, `pre_round.{h,cpp}`, `round_end.{h,cpp}`; the `# M2b ui` block of bf3_godot's CMake; the named insertion `// M2b ui` (`openbf3_register_m2b_ui`) at the end of `initialize_openbf3` in register_types.cpp | 7321198 |
| `ui.hud.tdm.unit`, `ui.hud.tdm.install` | `tests/ui_hud_tdm_tests.cpp`, the `# M2b ui` block of tests' CMake | a6492d6 |
| the ui switch of `m2b.contract2` | the named insertion `kUiLanded = true` in `tests/m2b_contract2_tests.cpp` | 2118c04 |
| `Bf3TdmHudDemo` (§5); the scoreboard header and the kill feed under the scoreboard, fixed from its screenshots | `src/bf3_godot/src/hud_tdm_demo.{h,cpp}`, hud_scoreboard.cpp, hud_killfeed.cpp | bc3438b |

`hud_model.h` (#16) needed no change: the contract's structs held everything the screens draw.

## 2. The feed: `fill_hud_tdm`

`host::fill_hud_tdm(input, static, kits, state, events, model)` runs after `fill_hud` with the same drained
`PresentEvent`s and does not bump `HudModel::serial`. It is pure, Godot-free, and reads only `HudFeedInput`'s M2b
fields (`match`, `players`, `kill_card_source`, `spawn_kit`, `now_s`), the static weapon rows and the TDM kits.
Without `input.m2b` it hides the four M2b screens and returns, leaving the M2a part of the model byte for byte as
`fill_hud` wrote it (a unit case checks this).

**Range checks.** Every slot, team, kit and phase is checked before it is used. A bad value is clamped or skipped and
counted once in `HudTdmFeedState::bad_inputs`, and it is never used as an index:

* a phase past `RoundOver` reads as `Loading`;
* an own slot >= 64 (other than `0xFF`) reads as no player, and an own team > 2 reads as team 0. The friendly side
  defaults to team 1 while the own team is unknown;
* a player row with team > 2, kit >= 4 or `name_bytes` > 16 is clamped;
* a Death whose victim slot is >= 64 is skipped, and a killer slot >= 64 reads as no player;
* a killer health that is NaN or outside 0..100 is clamped;
* a NaN `now_s` reads as 0, sanitised before the events are read;
* a tick more than one hour ahead is clamped (`kMaxAheadTicks`).

**Players.** Rows are filled by server slot (`players[slot]`, `kHudMaxPlayers` 64), and `player_count` is the
highest slot + 1.

* `sorted_players`, `team1_players` / `team2_players` and `friendly_team_players` / `enemy_team1_players` are sorted
  by score, then kills, then the lower slot.
* The team names are `text_hash("ID_M_US")` and `text_hash("ID_M_RU")` (strings "US" and "RU").
* `top_player1..3_name`, the winner flags and `scoreboard_enabled` (`input.scoreboard`, Tab) are filled from the
  same rows.
* The kill counter fields follow from `MatchState`, with negative values clamped to 0.

**Kill feed.** Each Death event pushes one `HudKill` into the ring of 8:

* slot is the killer, aux the victim;
* the item comes from the def's static row. A def other than `kNoWeapon` with no row is counted in `unknown_defs`
  and leaves `item_id` 0;
* headshot comes from the event flags;
* `suicide` is set when the killer is the victim or no player;
* `team_kill` is set when killer and victim share a team.

The serial grows by one per push, with a wrap guard. An own kill that is not a suicide also goes to
`local_kill_message`. The screen fades a line `kKillFeedSeconds` (5 s) after it first drew it, so the feed itself
keeps no clock.

**Kill card.** A Death with `kOwnVictim` starts a card and sets these fields:

* serial + 1, visible until `now_s + kKillCardSeconds` (4 s);
* the killer and the killer's kit (from the row);
* headshot, suicide, item and the weapon's name hash;
* `killer_health` from the event's value;
* the distance from the own corpse to the killer's position at the death (`kill_card_source`), or -1 when the killer
  was not seen or the value is not finite.

The event's `extra` and `seed` hold the death's `respawn_tick` and `revive_until`; the feed keeps them in the state
for the spawn screen. The own Spawn event ends the card early.

**Spawn screen.**

* `visible`: an M2b match, the own soldier dead, no kill card up, and not `RoundOver`.
* `at_join`: no own life yet this round.
* `can_deploy`: visible, the phase is PreRound or InRound, and the server tick has reached the death's respawn tick.
* `seconds_to_deploy` and `seconds_revive_left` come from ticks (clamped).
* `revive_possible`: dead, not at join, and before `revive_until`.
* `selected_kit` is `input.spawn_kit`, the game's choice.
* `kit_items[k]` holds the own team's loadout for kit k, slots `kHudSpawnSlots` {0, 1, 2, 6, 7}, as each def's
  `item_id` (assault's slot 2 is the defibrillator).

The soldier's `show_medic_revive` / `time_until_revive` and the spawn logic's timers follow the same ticks.

**Pre-round and round end.**

* `pre_round`: visible in PreRound. It counts down while the countdown tick is set (otherwise it shows "waiting for
  players"). `input_restricted` holds until the restrict tick.
* `end_of_round`: visible in RoundOver, with the winner and both teams' kills. `local_won` is set only when the own
  team is known. It also carries the kill limit, the top 3 slots by score, and the seconds to `restart_tick`.

## 3. The screens: `Bf3TdmHud`

`Bf3TdmHud::build` adds a full-window `Control` "Bf3TdmHud" (mouse ignore) under the game's parent, holding six
`Bf3TdmScreen` children in this draw order: KillFeed, PreRound, SpawnScreen, KillCard, RoundEnd, Scoreboard. A
screen is a `Control`, drawn on the 1280x720 stage and mapped to the window by `host::hud_stage_to_window` /
`hud_scale`, so it follows the window size.

* `set_model` copies the model and calls each screen's `update_screen`. That shows or hides the screen and queues a
  redraw.
* The drawing happens in `NOTIFICATION_DRAW` through a C++ virtual (`draw_screen`). Godot 4.8 dev7 does not dispatch
  a registered `_draw` on a second-level extension class (the M2a HUD's pattern).
* `wants_mouse()` is true while the spawn screen or the round end is up.
* `take_kit_click()` / `take_deploy_click()` hand the game the spawn screen's clicks.

**Lookups** (`TdmHudContext`, resolved once each, after `Bf3Localisation` / `Bf3UiAssets` have loaded; until then
the screens draw with the fallback font). Every lookup that fails is counted once in `TdmHudRecords::missing_fonts` /
`missing_icons` / `missing_strings`, warned once, and drawn with a fallback:

| kind | source | used |
|---|---|---|
| fonts | `Bf3UiAssets::get_font_file` | `$HUD_medium` (numbers, Venice-Medium on en), `$Menu_medium` (text, Purista EA Medium), `$Menu_bold` (titles, Purista EA Semibold) |
| strings | `Bf3Localisation::database()->find_sid` | 24 SIDs (`hud_tdm_text.h`): the four kit names, ID_M_SPAWN_DEPLOY_AS / _DEPLOY / _TIME_TO_DEPLOY, ID_H_KILLEDBY, ID_H_KILLCAMSUICIDE, ID_H_HEALTH, ID_SCORE_HEADSHOT (its `{0:s}` dropped), ID_H_PREROUND_COUNTDOWN, ID_H_PREROUND_WAITING_FOR_PLAYERS_TITLE, ID_M_EOR_TEAMWON / _TEAMLOST, ID_H_SCOREBOARD_WINNER, ID_M_EOR_NEXT_ROUND_STARTS_IN, ID_M_US, ID_M_RU, ID_M_SB_NAME, ID_H_SCOREBOARD_KILLS / _DEATH / _SCORE, ID_M_SB_CONNECTION; and the weapon names by `weapon_name_sid_hash` |
| icons | `Bf3UiAssets::get_region("ui/assets/iconsingame", …)` | `mc_iconsIngameSmall/KitAssault`, `KitEngineer`, `KitSupport`, `KitRecon`, `mc_iconsIngameSmall/dead` |
| weapon icons | the M2a `Bf3Hud::icon(item_id)` (`_small` DxTextures) | the kill feed, the kill card, the kit bar |
| placements | `Bf3UiAssets::hud_placements` | hudmpscreen `HudMessage_02` (the kill feed), spawnscreenpc `KitBar_01` and `TicketCounterSpawnScreen_01` |

* **The SIDs.** They were found in the SWF layouts' text fields and checked against the en database: every one
  resolves to the English recorded in `hud_tdm_text.h`, and `ui.hud.tdm.install` asserts that.
* **Own texts.** Three texts have no SID in the data the HUD reads, so they are drawn as own texts (not lookups):
  "REVIVE POSSIBLE", "BOT" and "M" (metres).
* **Weapon icons.** They come from the M2a HUD so the 203 `_small` icons are not loaded twice. The context finds the
  `Bf3Hud` with `find_children("*", "Bf3Hud")`, searching again every 120 frames until one is found. Without one, the
  lines draw without icons, and nothing is counted until the HUD is there.
* **The headshot mark.** No headshot icon exists in the atlases the HUD reads, so the mark is drawn: a ring and a dot.

### 3.1 Kill feed (`Bf3TdmKillFeed`)

The lines are drawn newest first, right-aligned under hudmpscreen's `HudMessage_02` (top right): killer, the weapon's
`_small` icon at 64x16, the headshot mark, then the victim. Names take the team colour, and a line the local player
is in gets a tint. A line fades over the last 0.5 s of `kKillFeedSeconds`, counted from its first draw.

* **Records.** It records `kill_feed_lines` (lines drawn), `newest_serial` and `lines_started` (distinct serials
  ever seen; E9 compares this with the deaths received).
* **Under the scoreboard.** While the scoreboard is up the feed draws nothing, but it still counts and ages its
  lines. This changed after the screenshots showed the feed through the scoreboard panel.
* **Team counter.** The screen also draws the team counter at the top centre: own team and count, the limit, the
  enemy count and team. It shows while a kill limit is set and no full-window screen or scoreboard is up.

### 3.2 Scoreboard (`Bf3TdmScoreboard`)

The scoreboard is visible while `scoreboard_enabled` (Tab is held). It is a panel at stage (120, 70), 1040x580, with
two columns: the friendly team on the left, the enemy on the right.

* **Header.** The team names sit at the outer edges, the two counts towards the middle, and "/ limit" between them.
* **Rows.** The columns are rank, NAME (with a small "BOT" tag), K, D, SCORE, PING (a bot shows "-"). A dead player
  gets the `dead` icon, the local row is highlighted, and rows shrink to fit (14 to 26 stage px).
* **Records.** It records `scoreboard_rows` and `scoreboard_team_kills`.

### 3.3 Kill card (`Bf3TdmKillCard`)

The card is a bottom-centre panel at (430, 470), 420x150, shown while `kill_card.visible`. It carries "KILLED BY",
the killer's name in the team colour, the killer's kit icon and name, the weapon's icon at 128x32 with its localised
name, "HEADSHOT", the killer's "HEALTH", and the distance "37 M" when known. For a suicide or a fall the card reads
"YOU COMMITTED SUICIDE".

**Records.** It records the serial, the killer name drawn, the item and `kill_card_weapon_named`.

### 3.4 Pre-round (`Bf3TdmPreRound`)

The box sits at the top centre (stage top 132, 360x92). It shows "ROUND WILL START IN" and the seconds rounded up, or
"WAITING FOR PLAYERS". Its edge is orange while the input freeze is on. It is hidden while the round end shows.

### 3.5 Round end (`Bf3TdmRoundEnd`)

The screen dims the world and shows a panel at (340, 150), 600x400. The title is "YOUR TEAM WON!" / "YOUR TEAM
LOST!", or "WINNER" and the team name when the own team is unknown. Below it are both teams' counts with "WINNER"
under the winner and "/ limit" between them, the top 3 players with their scores, and "NEXT ROUND STARTS IN: N".

**Records.** It counts its rising edges in `round_end_shown`.

### 3.6 Spawn screen (`Bf3TdmSpawnScreen`)

The screen dims the world. "DEPLOY AS" sits over four kit cards (190x140, gap 10) at spawnscreenpc's `KitBar_01`
(bottom left). Each card shows the kit's iconsingame icon and name and the `_small` icons of its `kit_items`, and the
selected card is outlined.

* **Team counts.** These sit at `TicketCounterSpawnScreen_01`.
* **Deploy.** At the bottom right (stage 914, 622; 270x56) is the Deploy button. It reads "DEPLOY", or, while it is
  disabled, "TIME TO DEPLOY  N".
* **Revive line.** After a death, while a medic can still revive, "REVIVE POSSIBLE  N" shows in green above Deploy.
* **Buttons.** The cards and Deploy are Godot `Button`s with focus mode none, so Enter stays the game's key. They are
  styled with `StyleBoxFlat`, and the title font is set on Deploy. Their `pressed` signals go through `callable_mp`.
* **Clicks.** A click is kept until `take_kit_click` / `take_deploy_click` takes it. Clicks count only while the
  screen is visible, and Deploy only while `can_deploy`. Both are cleared when the screen becomes visible again.
* **Selection.** The drawn selection is the model's `selected_kit`: the game owns the choice and feeds it back
  through `HudFeedInput::spawn_kit`.
* **No point choice.** TDM spawns are random, so there is no spawn point to choose.
* **Records.** It records `spawn_screen_shown` (rising edges), `deploy_enabled` and `selected_kit`.

## 4. Tests

`test_ui_hud_tdm` (`tests/ui_hud_tdm_tests.cpp`) prints its findings as `TDM ROW <name> PASS|FAIL <detail>`.

**`ui.hud.tdm.unit`** (suite `ui-hud-tdm-unit`) is data-free. It runs 7 cases:

1. **A recorded round.** 255 display frames run through `fill_hud` + `fill_hud_tdm`: pre-round with freeze, deploy,
   kills (own and others, headshot, suicide), the own death with its kill card, the spawn screen's timer and revive
   window, Spawn ending the card, the round end, and the restart. The case checks these invariants on every frame,
   counting the frames that miss one:
   * the serial is `fill_hud`'s;
   * the counts and the limit equal `MatchState`;
   * the feed's newest serial equals the deaths sent;
   * pre-round and round-end visibility follow the phase;
   * the spawn screen never shows with the card or the round end, and only while dead or unspawned.
2. **Players by slot and the sorted lists**, including the shrink to the highest slot.
3. **The kill-feed ring**: 20 deaths, the newest 8 kept, 2 unknown defs counted, 4 own kills in
   `local_kill_message`.
4. **The kit bar**: the own team's loadouts, the defibrillator in assault slot 2, team 2, and a def without a row
   reading as 0.
5. **Hostile inputs**: out-of-range slots, teams, kits, phases, name lengths and ticks, NaN health and NaN `now_s`.
   At least 10 bad inputs are counted, and nothing indexes out of range.
6. **m2b off**: the M2a part of the model is byte-equal to `fill_hud`'s.
7. **The lookup lists' shape**: unique keys, upper-case SIDs, the font, icon and placement lists.

**`ui.hud.tdm.install`** (suite `ui-hud-tdm-install`; exits 77 without an install) resolves every lookup the screens
make, through the same readers Godot uses:

* the 24 strings against the en database, each equal to its recorded English;
* the three fonts through the en font mapping and `resolve_font_name`;
* the five icons through `SwfLibrary::load` + `build_atlas_map`;
* the three placements through `read_hud_placements`;
* the 13 TDM kit items of both teams: catalog → `ItemIconIndex` → the name's text → `load_item_icon`;
* install.unchanged.

Both rows are functional, parallel-safe and carry no scheduling label.

## 5. Run-time check and screenshots: `Bf3TdmHudDemo`

The game wiring (`m2b_game`, `level_session_m2b`, E9's `m2b_check`) is gx's. On this branch nothing in the game
builds `Bf3TdmHud` yet. So the track adds `Bf3TdmHudDemo`, a node that drives the screens with no game:

* It builds `Bf3TdmHud` beside a `Bf3Localisation`, a `Bf3UiAssets` and a hidden M2a `Bf3Hud` (the weapon icons'
  source).
* It holds one hand-made `HudModel` per stage for 20 drawn frames: pre-round, spawn screen at join, kill feed, kill
  card, spawn screen after a death, scoreboard, round end. It uses 24 players and real weapon item ids from the
  M2a HUD's item index.
* After each stage it checks `TdmHudRecords`. At the spawn screen it emits the kit and Deploy buttons' `pressed`
  signals and checks that the game would take them: kit 2 and Deploy at join, and an early Deploy refused after a
  death.
* With `-- --tdm-shots=<dir>` it saves one PNG per stage.
* It quits with exit code 0 when every row passed.

The repo has no scene for it (the demo scene was cut from the track's file list). The scene is a scratch file with
three nodes: Localisation (`Bf3Localisation`), UiAssets (`Bf3UiAssets`) and Demo (`Bf3TdmHudDemo`).

```
[gd_scene format=3]
[node name="TdmHudDemo" type="Node"]
[node name="Localisation" type="Bf3Localisation" parent="."]
[node name="UiAssets" type="Bf3UiAssets" parent="."]
[node name="Demo" type="Bf3TdmHudDemo" parent="."]
```

**Results.**

* **Headless**: 9/9 rows, 2.2 s wall.
* **Windowed** (1280x720, the user away): 16/16 rows with 7 screenshots in
  `openBF3\b\shots\ui\tdm_*.png` (gitignored), 2.5 s.
* **Lookups**: 0 missing fonts, icons or strings, and no ERROR or WARNING lines.

**What the screenshots show** (for the user's judgement of the look; the background is a flat grey-green, not the
game):

* `tdm_pre_round`: the team counter at the top centre ("US 0 100 0 RU") over a box "ROUND WILL START IN" with a
  yellow 8, edged in orange (the freeze).
* `tdm_spawn_join`: "DEPLOY AS" over four dark kit cards. ASSAULT is outlined in yellow; each card has its green
  iconsingame kit icon, its name and four white weapon silhouettes. The team counts are at the top right of centre
  ("US 0 / 100 0 RU"), and a teal DEPLOY button is at the bottom right.
* `tdm_kill_feed`: six feed lines at the top right with blue and orange names and white weapon silhouettes. Two lines
  have the drawn headshot ring, and the local player's lines are tinted (orange for the own death, yellow for the own
  kill). The counter "US 23 100 19 RU" is at the top centre.
* `tdm_kill_card`: a bottom-centre card with an orange left edge: "KILLED BY", "Bot 05" in orange, the engineer kit
  icon and name, an M4A1 silhouette with "M4A1", a yellow "HEADSHOT", "HEALTH 64" and "37 M".
* `tdm_spawn_dead`: the spawn screen with ENGINEER selected, "REVIVE POSSIBLE  7" in green, and a greyed "TIME TO
  DEPLOY  5" button. The kill feed stays visible at the top right.
* `tdm_scoreboard`: the two-column scoreboard, "US" and "RU" at the outer edges, "23 / 100 19" in the middle,
  12 rows per side sorted by score, the "BOT" tags, and the local row "openBF3" highlighted in yellow. The kill feed
  is hidden.
* `tdm_round_end`: "YOUR TEAM WON!" in blue, "US 100" with "WINNER" under it, "/ 100", "RU 87", the top 3 (Bot 11
  1247, Bot 09 1163, Bot 22 1104) and "NEXT ROUND STARTS IN: 18".

**The run fixed two defects**: the kill feed showing through the scoreboard, and the scoreboard's limit running into
the "RU" title. Both screenshots above are after the fix.

## 6. Gates (measured 2026-10-19, dev preset, m2b/ui)

One command per gate, from the worktree root (the dev build directory `b\dev`):

| gate | threshold | measured | met | command |
|---|---|---|---|---|
| ui.hud.tdm.unit | data-free, the HUD model from recorded frames, <= 2 s | 7 cases pass, 0.05 s; recorded round: 255 frames, 7 deaths = 7 feed lines, 2 kill cards (239 frames for 4 s), 0 mismatching frames | yes | `ctest --test-dir b\dev -R "^ui\.hud\.tdm\.unit$"` |
| ui.hud.tdm.install | every string, font and icon lookup resolved against ui1 / ui2 on the install, <= 15 s, install.unchanged | 0.55 s; 24/24 strings equal their English, 3/3 fonts (Venice-Medium, Purista EA Medium, Purista EA Semibold), 5/5 icons, 3/3 placements, 13/13 kit items named with a `_small` icon (the defibrillator `DEFIBRILLATOR`, 64x16); install.unchanged clean (256 files) | yes | `ctest --test-dir b\dev -R "^ui\.hud\.tdm\.install$"` |
| m2b.contract2 (ui case) | the contract's ui case runs with `kUiLanded = true` | 4 cases, 113 assertions pass, 0.09 s; the ui case is active, the cam case still PENDING (cam) | yes | `ctest --test-dir b\dev -R "^m2b\.contract2$"` |
| run time (no gate in m2b.md; the track's own check) | the screens build, draw and take clicks in Godot; 0 missing lookups | headless 9/9 rows (2.2 s); windowed 16/16 rows, 7 PNGs; 0 missing fonts, icons and strings | yes | `"C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe" --path bf3-opensource --resolution 1280x720 <scratch>\tdm_hud_demo.tscn -- --tdm-shots="C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui"` |
| E9 | the client screens fed from the game, 90 s scripted play | not run: E9 is `godot.m2b.play` (gx's row, `window` phase, the wave-2 merge pass) | n/a (gx) | `ctest --preset dev-window -R "^godot\.m2b\.play$"` (merge pass only) |
| branch pass | every functional row passes | 149 rows: 147 passed, 2 skipped (`core.cache_data`, `core.res`, as on m2b/w2), 0 failed; 386.6 s ctest wall (400 s with the build, OPENBF3_GODOT set); in the parallel pass ui.hud.tdm.unit 0.11 s, ui.hud.tdm.install 1.49 s, m2b.contract2 0.17 s; budget warnings only, none on this track's rows | yes | `tools\build.cmd dev` |

The branch pass's budget figures are indicative only (one run, on a branch).

## 7. Open issues and handoff

**For gx:**

* `fill_hud_tdm` must run after `fill_hud` with the same drained events.
* Pass the selected kit in `HudFeedInput::spawn_kit`.
* Call `Bf3TdmHud::set_model` every display frame.
* Set the mouse mode from `wants_mouse()`, and take `take_kit_click()` / `take_deploy_click()` once per frame.
* Build `Bf3TdmHud` under a full-window `Control` or a `CanvasLayer` above the M2a HUD.
* The weapon icons need the M2a `Bf3Hud` somewhere in the tree. The context finds it by class and searches again
  every 120 frames if it is missing.

**Other open issues:**

* **E9 is not measured here.** It is gx's row in the merge pass. The screens' run time was checked only by the demo
  with hand-made models (§5), not with a live server's frames.
* **Inferred values.** These are `[inferred]`: the layouts not taken from a placement (the panels' sizes and
  positions, the kill card, the round end, Deploy's position), the kill feed's line metrics, the 0.5 s fade, and
  `kKillCardSeconds` 4 s / `kKillFeedSeconds` 5 s (the contract's).
* **Own texts.** "REVIVE POSSIBLE", "BOT" and "M" have no SID in the data read, so they stay English on other
  locales.
* **The headshot mark** is drawn, not BF3 art.
* **asan.** `fill_hud_tdm` is not a parser, and the new rows ran on the dev preset only. They are asan-ready (the
  CMake block sets the asan PATH) for the acceptance step's asan pass.
