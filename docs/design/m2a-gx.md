# M2a gx: Godot integration, the playable scene (as built)

Track `gx` of M2a wave 3 (docs/design/m2a.md §15 row gx, gates E1, E3, E5, E10, E11, E19 through the game; E9,
E14 and E16 are the integrator's timed rows), on branch `m2a/gx` from the W3.0 contract `m2a/w3` 3b3b991. Phase 1:
wave 2's gunplay on, S1's walker bots (armed, not firing) drawn as M1 capsules, a human player who shoots, kills,
dies and respawns. Bot AI (bt), 3p soldiers (tp), game audio (au3) and game effects (fx3) are not built here: their
cases are skipped with "waiting for <track>", and the code that drives them is in place behind compile switches.

## 1. What the track adds

| part | files | owner |
|---|---|---|
| the playable scene and its launcher | `bf3-opensource/scenes/m2a_metro.tscn`, `tools/play.cmd` | gx |
| the M2a session (the wave-3 hook bodies) | `src/bf3_godot/src/level_session_m2a.{h,cpp}` | gx (W3.0 declared the hooks) |
| the in-game check | `src/bf3_godot/src/m2a_check.{h,cpp}` | gx (W3.0 declared the header) |
| the weapon input | `src/bf3_godot/src/input_mapper.{h,cpp}`: the `M2a gx [mapper]` member block (named insertion d34078f), the two hook calls (named insertions 413e5c5) and the end block (eb5bda2) | gx |
| test scripts | `tests/godot_m2a.cmake`, `tests/godot_m2a_firefight.cmake` | gx |
| E16 on the M2a scene | `tests/godot_m1_load.cmake`: named replacement `M2a gx [E16]` (01ee8ea), `-DSCENE` and `-DUSER_ARGS` | gx |
| shared files, one `M2a gx` block at the end | `src/bf3_godot/CMakeLists.txt`, `tests/CMakeLists.txt`, `README.md` | shared |
| named insertion in another track's file | `src/bf3_godot/src/level_session.cpp`: `mapper_.m2a_frame(*frame_, now_ns); // M2a gx [view]` before `set_input` (ff22559) | gx |

m2a.md is unchanged.

## 2. Playing

```
tools\play.cmd                          (= <godot> --path bf3-opensource res://scenes/m2a_metro.tscn -- --bots=24)
tools\play.cmd --kit=3 --team=2 --respawn=3 --bots=16
```

`OPENBF3_GODOT` names the Godot executable (default `C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe`).
User args (after `--`): `--kit=<0..3>` (assault, engineer, support, recon; default 0), `--team=<1|2>` (US, RU; default
1), `--respawn=<s>` (default 5 s in the scene, 1 s for the checks; `GameServerConfig::respawn_ticks`), `--bots=<n>`
(S1's walkers; default 24, at most 32: sv's `kMaxLagSlots`, m2a-w3.md §4). A bad value fails the load with a message.

| input | action |
|---|---|
| mouse, WASD, Shift, Space, Ctrl, Z | M1's look, move, sprint, jump, crouch, prone |
| left mouse | fire (held level + press counter: semi weapons fire once per press) |
| right mouse | ADS (held; refused while sprinting by sv's rule; mouse sensitivity follows the look scale) |
| R | reload (a tap is held as a level for one client tick at least) |
| 1, 2, G, F, wheel | primary, sidearm, grenade (slot 6), knife (slot 7), cycle the filled slots |
| B | fire mode |
| K / T | the next spawn's kit / team (a KitSelect, sent only once spawned) |
| F6 | M1's free camera (hides the viewmodel and the HUD; the audio listener follows it once au3 lands) |
| Esc | releases the mouse and every held button; the click that captures it again never fires |

On screen: the M1 status lines, ui2's minimap, the HUD (`Bf3Hud`: ammo, crosshair hidden in ADS, hit indicator, weapon
name and icon, health), the viewmodel of the own team (`Bf3Viewmodel`, both team sets loaded), the score line
(`kills k deaths d   US a : RU b`) and the kit line (`US assault   next spawn: ...`). Bots are M1's capsules tinted
grey (team unknown), blue (friend) or red (enemy) from W3.0's team field; they are hidden once
`Bf3GameCrowd::built()` (tp). On death the viewmodel and the weapon HUD hide and the centre shows
`KILLED BY slot n (US|RU)` or `YOU DIED (fall)` with the respawn countdown.

## 3. The session (level_session_m2a.cpp)

* `m2a_parse_args` (main): the session is M2a when the session node carries the metadata `openbf3_m2a`
  (m2a_metro.tscn) or the command line has `--m2a-check` / `--m2a-firefight`; every hook returns at once otherwise,
  so M1, S1 and S3 run as before. Turns the mapper's weapon input on, reads `viewmodel_tuning.cfg`, and creates
  `Bf3Localisation` and `Bf3UiAssets` (with `Bf3Hud::required_atlas_movies()`) for a windowed run.
* `m2a_load_options` (loader): `SharedLoadOptions::gunplay = true`; starts the viewmodel thread (its own cache
  handle): `open_viewmodel_sources`, `read_viewmodel_kits`, the US and RU `load_viewmodel_set`.
* `m2a_load_assets` (loader): joins the viewmodel thread, so the kit viewmodels are part of the load that E16 times
  (Done is the first controllable frame with them loaded); `make_client_gunplay_inputs` (an error fails the load),
  `make_hud_static`; for a check run a `CollisionWorld` (the range lane) and the server recorder (`GunplayObserver`,
  every list bounded at 65,536 records, the excess counted).
* `m2a_server_config` / `m2a_client_config`: respawn ticks, the first spawn's kit and team, the check's observer and
  target bot; the client's gunplay inputs, `send_kit_select` with Ready, `record_predicted` for the checks.
* `m2a_presented` (every frame from the session's start): drains the presentation events in chunks of 256 (the ring
  never overflows during the load), takes the predicted records, scores Death events (`kOwnShooter` / `kOwnVictim`,
  the killer's team from W3.0's team field; a fall is killer == victim), detects death and respawn from the own
  state, gates the mapper's weapon bits on Done, alive, Spawned and a valid presented weapon, and sends K / T as a
  KitSelect only after Spawned.
* `m2a_frame` (after Done): builds the UI and the two viewmodels on the first frame after the load, maps
  `PresentedWeapon::def` to the team set's item through the kit unlocks (case-insensitive), presents the viewmodel
  (hidden when dead, in the free camera or without an item), fills the HUD, tints or hides the capsules, steps the
  wave-3 parts, draws the labels and runs the check. It returns true only for a check run, so the playable scene
  keeps M1's `--m1-load-check` and shots (E16 uses them).
* The wave-3 parts (`Bf3GameFx`, `Bf3GameAudio`, `Bf3GameCrowd`) are created and stepped in m2a_game.h's order only
  when the CMake `M2a gx` block finds their track's `.cpp` (`OPENBF3_M2A_HAVE_FX / _AUDIO / _CROWD`), and the
  firefight case only when `firefight_brain.cpp` no longer carries the W3.0 stub (`OPENBF3_M2A_HAVE_BT`). On this
  branch all four are 0: that code is not compiled here and must be built and run after the merges.

## 4. The check (m2a_check.cpp)

`--m2a-check=<cases>` runs the cases in order through the input mapper (scripted levels and presses, the same path
as a human's input) and the dev requests (Teleport, SpawnAt, SpawnTarget, TargetInfo); every row prints
`[m2a] PASS|FAIL <case>.<row> <value> (<rule>)`, the run ends with `M2A CHECK: PASS|FAIL (...)` and exit 0 / 1. A
case has a step timeout and a case timeout (180 s; kits 420 s; play its length + 120 s).

| case | gate | what it does |
|---|---|---|
| range | E1, E3 | M16A3 from a full magazine held: 31 first predictions against the server recorder field for field, the 75 ms gaps, the first shot at its tick's start, one OwnShot event and one viewmodel Shot cue per shot, the HUD's ammo; M9 5 taps = 5 shots, held = 1; E3 on a range lane (`find_range_lane`) with the target bot: chest and head at 5 m, chest at 60 m, hits to kill, damage per hit, the HUD hit indicator per server hit, the kill counter |
| reload | E5 | tactical and empty reload: duration and commit point against the def, the first prediction equal to the server, the viewmodel's reload clip and the HUD's `is_reloading` on every reloading frame, a switch 0.25 s in cancels with the magazine kept |
| kits | E11 | the 8 loadouts through KitSelect + SpawnAt: the presented kit and team, the slot defs, the viewmodel item of slots 0, 1, 6, 7 from the kit unlocks, the primary fires and reloads (ReloadStarted .. Committed or Finished, magazine full); the knife hits the target bot; the M67's fuse |
| hud | E10 | the localised name and `_small` icon of every kit item; every frame the HUD model against the presented weapon |
| ads | E19 | world FOV 55 -> 40 -> 55 within the aiming controller's FOV times + 0.1 s, the look scale, the mapper's mouse scale, ADS refused while sprinting |
| play | the play gate | the scripted 5-minute run (§6) |
| audio, fx | E7, E8 | skipped: "waiting for au3" / "waiting for fx3" |
| firefight | E13, E14 | `--m2a-firefight`; skipped: "waiting for bt" |

The play case: a 30 s movement pattern (walk, sprint, strafe, back, crouch, jumps) with the view sweeping; every 8 s
2.5 s of bursts at the nearest enemy within 80 m (every second engagement aimed down the sights); a reload every
30 s, primary / sidearm switches, a grenade and a knife every 60 s; K / T at 120 s (the next life spawns on the other
team with the next kit); a fall to death every 70 s from 50 s (Teleport 60 m up; no input for 8 s; a drop that lands
alive on a roof drops again from there, up to 3 times). Rows: 3 falls, deaths >= falls, 3 respawns, >= 50 OwnShot
events, the team switch, both viewmodels built, 0 frames with an unmapped def, the HUD loaded, 0 event overflows,
0 bad gunplay events, >= 1 KitSelect, 5 screenshots; kills, team kills, remote shots and reconciliations reported.
The ctest script also fails it on any line containing `ERROR` (`-DNO_ERRORS=1`).

## 5. Test rows

All in the `# M2a gx` block of tests/CMakeLists.txt, registered with `openbf3_godot_row(... WINDOW ...)` (label
`window`, RUN_SERIAL, the project lock, exit 77 = skipped without `OPENBF3_GODOT`, a window, an install, or while
`OPENBF3_USER_ACTIVE_FLAG` exists). The three godot.m2a* check rows share one warm cache root
(`b/<preset>/godot-cache/godot.m2a`); none of them measures a load.

| row | script | COST | runs |
|---|---|---|---|
| `godot.m2a` | `godot_m2a.cmake` (cases range, reload, hud, ads, audio, fx) | 80 | branch, by hand, window preset |
| `godot.m2a.kits` | `godot_m2a.cmake -DCASES=kits` | 80 | branch, by hand |
| `godot.m2a.play` | `godot_m2a.cmake -DCASES=play -DNO_ERRORS=1 -DARGS=--respawn=3` | 330 | branch, by hand |
| `godot.m2a_firefight` | `godot_m2a_firefight.cmake` (E9, E13 / E14 with bt; exit 77 while the case is skipped) | 30 | the integrator's merge pass |
| `godot.m2a_load` | `godot_m1_load.cmake -DSCENE=res://scenes/m2a_metro.tscn -DUSER_ARGS=--bots=0` (E16) | 50 | the integrator's merge pass |

`godot.m2a.play` is longer than test_budget's 120 s window budget by design (its gate is 5 minutes); test_budget
prints a warning for it, not a violation. The integrator may add it to `gate_length_rows`.

## 6. Gates and results

Measured 2026-10-18 on the branch (windowed rows one at a time, warm cache; shared machine, so wall times are
indicative only).

| gate | command | threshold | measured | met |
|---|---|---|---|---|
| godot.m2a (E1, E3, E5, E10, E19) | `set OPENBF3_GODOT=<exe>` `set OPENBF3_GODOT_WINDOWED=1` `ctest --preset dev-window -R "^godot\.m2a$"` | `M2A CHECK: PASS`, every functional case PASS, audio / fx SKIP with their track | 4 pass, 0 fail, 2 skip; 58.0 s | yes |
| godot.m2a.kits (E11) | `ctest --preset dev-window -R "^godot\.m2a\.kits$"` (same environment) | `M2A CHECK: PASS` | 67 / 67 rows pass; 67.4 s | yes |
| play, 5 minutes, 0 errors | `ctest --preset dev-window -R "^godot\.m2a\.play$"` (same environment) | `M2A CHECK: PASS`, 0 `ERROR` lines | PASS, 0 ERROR lines, 300.4 s scripted, 312.3 s row | yes |
| screenshots | the play row (`OPENBF3_GX_SHOTS` overrides the directory) | hip, ADS, reload, firefight, death in `b\shots\gx` | 5 / 5 (§7) | yes |
| branch pass | `tools\build.cmd dev --strict` | every row passes, no budget violation | 128 rows: 126 passed, 2 skipped (`core.cache_data`, `core.res`), 0 failed, in two runs (556 s and 627 s on a busy machine). `--strict` exited 1 both times, on rows of other tracks that have no COST and went over 30 s under the load (run 1: `render.s3` 31.9 s; run 2: `anim`, `anim.s3`, `render.s3`, `sim.terrain.unit`, `fx.unit`, `anim.server_pose`). test_budget STRICT over the five gx rows (window preset): 0 violations | rows: yes; STRICT: no, on other tracks' rows |
| E16 | `ctest --preset dev-window -R "^godot\.m2a_load$"` | cold <= 15 s, warm <= 5 s | the integrator's; one branch run (unlocked, shared machine, indicative only): cold 4.34 s, warm 5.42 s; install unchanged (256 files) | indicative: warm over |
| E9, E13 / E14 | `ctest --preset dev-window -R "^godot\.m2a_firefight$"` | m2a.md E9, E14 | the integrator's (skipped until bt) | - |

Results per case (godot.m2a, godot.m2a.kits, godot.m2a.play):

* range: 31 predicted = 31 server shots, every field equal (worst vector difference 0); every gap 4,500,000 units
  (75 ms); first shot at its tick's start; 31 OwnShot events, 31 viewmodel Shot cues; the HUD's ammo equal after the
  burst; M9 5 taps -> 5 shots (server 5), held -> 1. E3: chest at 5 m 4 hits of 25.000, head at 5 m 2 hits of 50.000,
  chest at 60 m 6 hits; the hit indicator rose once per server hit and the kill counter counted each kill.
* reload: tactical 1.8 s, commit 1.53 s; empty 2.37 s, commit 2.0145 s, all equal to the def; the first prediction
  equal to the server; the reload clip on >= 0.9 of the 324 / 426 reloading frames; the HUD reloading every frame; a
  switch cancels with 27 rounds kept.
* hud: names 12 / 12, icons 12 / 12, 1,253 frames compared, 0 mismatches.
* ads: FOV 40 at 0.128 s (limit 0.12 + 0.1), 55 at 0.156 s (0.15 + 0.1), look scale 0.5, the mapper's mouse scale
  0.5, 145 sprinting frames with the FOV at 55.
* kits: 8 / 8 loadouts spawn as asked; every slot def and viewmodel item (US: M16A4, M4A1, M27 IAR, MK11, M9; RU:
  AK-74M, AKS-74U, RPK-74M, SVD, MP443; both: M67, knife); every primary fires and reloads to a full magazine; the
  knife hits; the M67 detonates 3.6 s after it left the hand.
* play: 3 falls (5 drops), 3 deaths, 3 respawns, 403 OwnShot events, 4 kills (US 2 : RU 2), the team switch at
  120 s, 1 KitSelect, both viewmodels built, 0 unmapped frames, 0 event overflows, 0 bad gunplay events, 1,045
  events drained, 0 remote shots (S1's walkers do not fire), 0 weapon reconciliations.
* Load (indicative, not E16): viewmodel sets 1.5-1.9 s warm (US 7 items, RU 7, 7 + 7 defs mapped); HUD 12 rows, 203
  icons.
* The BF3 install is unchanged (layout.toc, cas.cat, the .sb / .toc files: the `install.unchanged` rows of the
  branch pass).

## 7. Screenshots

In `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\gx\` (gitignored, outside the worktree), 1152 x 648, from
the last godot.m2a.play run:

* `gx_hip.png`: crouched beside the shuttered shop fronts of the park, the M16A3 viewmodel at the hip with the
  hands, the four-tick crosshair at the centre, HUD `25 / 155`, M16A3 name and icon, health 100 %, the minimap at the
  bottom left, `kills 0 deaths 0  US 0 : RU 0` and `US assault  next spawn: assault US` at the top right.
* `gx_ads.png`: the M16A3 aimed down the sights (the rear aperture centred on the front post), the crosshair hidden,
  HUD `27 / 131`.
* `gx_reload.png`: the M16A3 rolled to the side in its reload clip in front of the hotel fence, the magazine count
  `7` in red (low ammo), `/ 155`.
* `gx_firefight.png`: aimed down the sights under the shop awning at a red (enemy) capsule down the walkway (the shot waits for an enemy within 45 m in the line of sight), the
  magazine `0` in red after the burst, `kills 1 deaths 0  US 1 : RU 0`.
* `gx_death.png`: on a roof above the park after a fall drop, `YOU DIED (fall)` and `respawn in 1.5 s` at the
  centre, health 0, no viewmodel and no weapon HUD, `kills 1 deaths 1  US 1 : RU 0`, the minimap still drawn.

`gx_range.png` (an earlier range run) shows the M16A3 on the range lane with HUD `25 / 118` and kills 3.

## 8. Open items

* audio (E7), fx (E8), firefight (E9, E13, E14) and the crowd's capsule hiding wait for au3, fx3, bt and tp. The
  `#if OPENBF3_M2A_HAVE_*` code that builds and steps those parts is not compiled on this branch.
* The viewmodel thread cannot be cancelled: a quit during `open_viewmodel_sources` waits for it (m2a_shutdown).
* `remote_shots` is 0 because S1's walkers are armed but do not fire; damage to the player in the play run comes
  from the falls (the range case proves damage dealt; bt's bots will prove damage taken from bots).
* E16 (`godot.m2a_load`), E9 and E14 (`godot.m2a_firefight`) are the integrator's merge-pass rows. The one
  indicative branch run of `godot.m2a_load` read warm 5.42 s (threshold 5 s; cold 4.34 s): the warm SharedLevel was
  ready at +4.14 s after `_ready`, with the gunplay tables (nothing cached) and the viewmodel sets (3.39 s on their
  own thread, joined in `m2a_load_assets`) on the load. If the quiet-lock run also misses, the fallback is to join the
  viewmodel thread after Done (the viewmodel appears a few frames later) or to cache the gunplay tables.
