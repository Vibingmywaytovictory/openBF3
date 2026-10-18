# M2a gx: Godot integration, the playable scene (as built)

Track `gx` of M2a wave 3 (docs/design/m2a.md §15 row gx, gates E1, E3, E5, E10, E11, E19 through the game; E9,
E14 and E16 are the integrator's timed rows), on branch `m2a/gx` from the W3.0 contract `m2a/w3` 3b3b991. Phase 1:
wave 2's gunplay on, S1's walker bots (armed, not firing) drawn as M1 capsules, a human player who shoots, kills,
dies and respawns. Bot AI (bt), 3p soldiers (tp), game audio (au3) and game effects (fx3) are not built here: their
cases are skipped with "waiting for <track>", and the code that drives them is in place behind compile switches.
The end-to-end step (§9) merged `m2a/w3` (bt, tp, au3, fx3) into `m2a/gx` (0040e33) and wired all four parts in:
the playable scene now has bt's firefight bots that fight the player and each other, tp's 3p soldiers with their
weapons, au3's game audio and fx3's effects, and the audio, fx and firefight cases run their bodies.

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
(bt's firefight bots since §9, S1's walkers before; default 24, at most 32: sv's `kMaxLagSlots`, m2a-w3.md §4). A bad value fails the load with a message.

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
  firefight case only when `firefight_brain.cpp` no longer carries the W3.0 stub (`OPENBF3_M2A_HAVE_BT`). Since the
  end-to-end step (§9) all four are 1.

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
| audio | E7 | au3's G7 script in the game (§9): the own soldier fires, heard in 3p through a listener on the aim axis; recorded and analysed by au3's `GameAudioCheck` |
| fx | E8 | 1,000 M16A3 shots at Metro concrete and brick, the client-local oilfire, a soldier hit, the cue counts (§9) |
| firefight | E9, E13, E14 | `--m2a-firefight`: 64 bt bots in pairs, 2560 x 1440, FX on / off windows (§9); the integrator's timed row |

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
| `godot.m2a` | `godot_m2a.cmake` (cases range, reload, hud, ads, audio, fx) | 350 | branch, by hand, window preset |
| `godot.m2a.kits` | `godot_m2a.cmake -DCASES=kits` | 80 | branch, by hand |
| `godot.m2a.play` | `godot_m2a.cmake -DCASES=play -DNO_ERRORS=1 -DARGS=--respawn=3` | 330 | branch, by hand |
| `godot.m2a_firefight` | `godot_m2a_firefight.cmake` (E9, E13 / E14 with bt; exit 77 off a 2560 x 1440 primary screen) | 30 | the integrator's merge pass |
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

In `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\gx\` (gitignored, outside the worktree), 1152 x 648. The
end-to-end step's play run replaced them (§9.4); the phase 1 run's are described here:

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

* (phase 1; closed by §9) audio (E7), fx (E8), firefight (E9, E13, E14) and the crowd's capsule hiding waited for au3,
  fx3, bt and tp. The end-to-end step's open items are in §9.5.
* The viewmodel thread cannot be cancelled: a quit during `open_viewmodel_sources` waits for it (m2a_shutdown).
* `remote_shots` is 0 because S1's walkers are armed but do not fire; damage to the player in the play run comes
  from the falls (the range case proves damage dealt; bt's bots will prove damage taken from bots).
* E16 (`godot.m2a_load`), E9 and E14 (`godot.m2a_firefight`) are the integrator's merge-pass rows. The one
  indicative branch run of `godot.m2a_load` read warm 5.42 s (threshold 5 s; cold 4.34 s): the warm SharedLevel was
  ready at +4.14 s after `_ready`, with the gunplay tables (nothing cached) and the viewmodel sets (3.39 s on their
  own thread, joined in `m2a_load_assets`) on the load. If the quiet-lock run also misses, the fallback is to join the
  viewmodel thread after Done (the viewmodel appears a few frames later) or to cache the gunplay tables.

## 9. The end-to-end step (as built)

### 9.1 What changed

* Merge `m2a/w3` into `m2a/gx` (0040e33, `--no-ff`): the shared files (README.md, src/bf3_godot/CMakeLists.txt,
  tests/CMakeLists.txt) keep both sides, w3's blocks first and then gx's block.
* The session (`level_session_m2a.cpp`, 3f1ce54):
  * `m2a_load_assets` loads `Bf3GameFx`, `Bf3GameAudio` (fx's impact sound names; the auditions' superbundles only for
    the audio case) and `Bf3GameCrowd` on the loader thread, each timed. A part that fails is dropped with an
    `[m2a] ERROR` line, and the game runs on without it.
  * `m2a_server_config`: `--bots=n` are bt's firefight bots (`FirefightConfig`, Free mode, walking). S1's bot level
    stays for walking (`BotFillConfig` count 0). The playable scene defaults to 24 bots, the play case to 16, and the
    cap is 32 (sv's `kMaxLagSlots`). `--m2a-firefight` uses 64 bots in Pairs mode, with the observer in slot 64.
  * Every frame, in m2a_game.h's order:
    1. crowd step;
    2. muzzles: the own 1p muzzle from `Bf3GameFx::own_muzzle` (a stand-in sample when the viewmodel has no muzzle
       joint), then the crowd's 3p muzzles;
    3. FX step, with the 1p projection kept equal to the viewmodel's FOV;
    4. audio step, which also takes the FX step's impact and effect sound cues.
  * The capsules hide once the crowd is built. Deaths by an enemy are counted.
* The check (`m2a_check.cpp`): the audio, fx and firefight case bodies (3f1ce54, 5b26f05, 9a28eb7, 486f003); the
  skips stay only for a build without the track (`OPENBF3_M2A_HAVE_* = 0`).
* Named replacements in other tracks' files, each in its own commit:
  * fx3's `game_fx.cpp` `resolve()` (b7652bc): a partition that holds an `EmitterDocument` counts as one. MP_012's
    oilfire partition has `EmitterTemplateData` as its primary instance, so `start_effect` refused it before.
  * au3's `tests/audio_router_tests.cpp` (589a3b0): `m2a_fake::impact_sound_names` deleted for the real
    `bf3::fx::impact_sound_names`.
* tests/CMakeLists.txt gx block: `godot.m2a` COST 80 -> 350 (audio and fx now run).

### 9.2 The cases

* audio (E7): au3's G7 script (`game_check_script()`, 17 steps, 10 rounds per burst) in the game.
  * Setup: on a new life (the earlier cases of a run spend the bag), the own soldier stands at the range lane with the
    M16A3 on automatic, aimed 60 degrees up. Its rounds leave Metro, so no impact sound reaches the recording.
  * A probe round's OwnShot position is the emitter. Per step, the listener (`M2aGame::listener_override`) sits on
    the aim axis at the step's distance.
  * The own soldier is heard in 3p: `M2aFrame::free_camera` is true while the override is on. This is a deviation from
    au's headless G7 shooter, which was a remote soldier. The free camera sends a neutral command, so the own soldier
    could not fire in it.
  * The auditions (M98B, RPG-7, M1A2) play at the emitter.
  * au3's `GameAudioCheck` records the mix, and `analyse_game_check` gives the rows. The recording goes to
    `b\shots\gx\gx_e7.wav`.
* fx (E8):
  * The two surfaces are found by a ray scan from every spawn: the first triangle 6-30 m out, facing the shooter, of
    PhysicsMaterialIndex 8 (concrete) and of 2 (brick).
  * 500 M16A3 shots at each surface, automatic, with reloads. When the bag runs dry, a new life at the same spawn
    refills it. The SpawnAt waits 1 s after the last shot (§9.5).
  * The oilfire starts 20 m ahead through `Bf3GameFx::start_effect`, client-locally, and its live particles are read
    after 15 s.
  * Three single rounds go at the target bot's chest at 8 m (the fx case now has the target bot).
  * The check accounts every frame from the scan to the rows. Impact starts are matched to their Impact event over
    frames (the start waits for its timeline) and by position (a penetrating round leaves several impacts).
  * Impact sounds are counted against the static Impact events whose cell names a sound. Cells without one play none:
    Metro's table has such cells. Soldier-cell sounds are told apart by position.
* firefight (E9, E13, E14), the integrator's timed row:
  * The window is 2560 x 1440 borderless with vsync off, and render time is measured. The case skips when the primary
    screen is not 2560 x 1440.
  * The observer stands at the range lane. A 10 s warm-up comes first, then 12 alternating windows of 5 s, FX on and
    off. In an off window the FX step does not run and `Bf3FxRenderer::hide_all()` hides the instances.
  * Rows: 64 bots, bursts, remote shots, E9's lights / particles / FX main-thread p99 <= 1.0 ms / FX GPU <= 2.0 ms, and
    E14's frame p99 <= 16.7 ms / M2a main p99 <= 3.0 ms / crowd step p99 <= 2.0 ms / 0 audio underruns / 0 event
    overflows.
  * Not run on the branch (TIMING rule).
* play: new rows:
  * remote shots > 0 (bt's bots fire);
  * deaths by an enemy, and the bot count (reported);
  * the crowd built, with its frames;
  * FX built, with its muzzle and impact counts;
  * audio built, with seconds mixed;
  * late audio commands, gated with `OPENBF3_AU3_GATE_LATE=1` (§9.5).

### 9.3 Results

Measured 2026-10-18 on the branch: windowed, one row at a time, warm cache. Shared machine, unlocked, so wall times
are indicative only.

| row | result |
|---|---|
| `godot.m2a` | PASS, 6 / 6 cases (range 27.7 s, reload 12.8 s, hud 7.7 s, ads 4.3 s, audio 124.5 s, fx 163.0 s); 0 ERROR lines; 348.8 s |
| `godot.m2a.kits` | PASS, 67 / 67 rows, 61.2 s case, 69.5 s row |
| `godot.m2a.play` | PASS, every row, 0 ERROR lines, 300.4 s scripted, 310.5 s row |
| branch pass `tools\build.cmd dev --strict` | exit 0: 137 rows, 135 passed, 2 skipped (`core.cache_data`, `core.res`), 0 failed, 0 STRICT violations; 194 s. The install's layout.toc, cas.cat and .sb / .toc files unchanged |

* audio (E7):
  * windows: every step's windows as scripted;
  * onsets: 10 per burst at every distance, spacing within 96 frames (mean gap 3600.3-3606.6 frames, expected 3600);
  * levels: near - mid 45.3 dB, mid - far 13.4 dB;
  * high-pass filter: mix 16.2 dB, noise tap 18.5 dB, noise prediction off by 0.75 dB;
  * Fire gate at 320 m: -144 dB;
  * auditions inside their bands: M98B at 325 m 0.5 (-93.0 dBFS), RPG-7 at 67.5 m 0.5 (-47.9 dBFS), M1A2 at 32.5 m 0.5
    (-67.4 dBFS) and at 90 m 0.5 (-98.8 dBFS); 0 or 1 outside them;
  * kit items: 12 (10 drivers, 2 one-shots), 44 programs, 0 failures;
  * every count 0: short / late blocks, late commands, mixer refusals, skipped steps, dropped frames, ring overflows.
* fx (E8):
  * 72 blueprints preloaded, 0 failures; atlas 52 / 52, 0 fallbacks;
  * 1,003 shots with a 1p muzzle start, the `_1p_flash` in 0.759 of them (761);
  * drawn instances equal to `FxWorld::particles()` on all 29,250 frames (0 mismatches);
  * oilfire: 80 live particles after 15 s;
  * concrete 500 / 500 impacts started `fx_impact_concrete_01_s_xp2` with `decal_concrete_s_01` (500 / 500);
  * brick 500 / 500 `fx_impact_concrete_01_s` with `decal_brick_s_01` (500 / 500);
  * soldier 3 / 3 `fx_impact_soldier_body_s`, 0 decals;
  * Metro has no water, so the Caspian row (`fx.install` "cells") covers it;
  * tracers 1,003 = shots flagged 1,003; shells 1,003 = firearm shots;
  * impact sounds 1,000 = expected 1,000 (1,000 Impact events, 0 culled);
  * 0 double starts.
* play: in 300 s:
  * 16 firefight bots, 9,178 remote shots, 123 team kills (US 68 : RU 55);
  * the player: 355 own shots, 5 kills, 6 deaths (3 by an enemy, the rest falls), 6 respawns, the team switch at
    120 s;
  * crowd: 50,791 crowd steps (0 frozen, 0 unknown weapons);
  * FX: 19,062 muzzle starts (0 remote shots without a crowd muzzle), 13,035 impact cells started, 11,275 decals,
    5 double starts (reported, §9.5);
  * audio: 300.8 s mixed, 96 voices at peak, 12,404 impact sounds, 384 reloads, 5 late commands (reported, §9.5);
  * 0 event overflows, 0 bad gunplay events, 0 weapon reconciliations.
* A 40 s scripted smoke run with 16 bots (before the rows) loaded:
  * fx: 72 blueprints, 0 failures;
  * audio: 44 programs;
  * crowd: 4 pose lanes;
  * the firefight: 16 Free bots, max slots 64.

  The bots fought each other (US 11 : RU 6), and the player was damaged and killed by bots.

### 9.4 Screenshots (the end-to-end play run)

In `b\shots\gx\`, 1152 x 648:

* `gx_hip.png`: the M16A3 at the hip in a room by a doorway. FX3's impact smoke and decals are on the wall left of the
  crosshair. HUD `26 / 155`, `US 1 : RU 1`: the bots are already scoring.
* `gx_ads.png`: the M9 aimed down its sights (the play script switches to the sidearm), the crosshair hidden, HUD
  `15 / 64`.
* `gx_reload.png`: the M9 tilted in its reload clip, HUD `12 / 64`. An orange-brown strip runs from the pistol to the
  lower right edge of the frame (§9.5).
* `gx_firefight.png`: the M16A3 in the park in front of the hotel fence. A dark 3p soldier stands at the fence in the
  centre. Score `US 25 : RU 20`, `kills 1 deaths 2`.
* `gx_death.png`: `KILLED BY slot 9 (RU)` with `respawn in 1.5 s`. A 3p soldier with helmet, gear and rifle (tp) fills
  the lower left, and the viewmodel and the weapon HUD are hidden.
* `gx_e7.wav`: E7's recording (the audio case).

### 9.5 Open items

* Shot ids are reused across a respawn (cl / fx3). `next_shot_index` is kept across lives. A shot predicted after the
  server's respawn tick never happens on the server, and the new life's first real shot gets the same id. fx3's
  triggers then drop that shot's effects as a repeat and count a double start: 5 in the play run, 4 in the fx case
  before its 1 s wait.
  * The fx case still gates 0 (E8 (7)). The play row reports the count.
  * A fix belongs to fx3 (forget the own slot's shot keys on its Spawn event) or cl.
* Late audio commands in the play run: 5 (8 in an earlier run). Each is a game-thread stall over the 21.3 ms
  scheduling lead. This is wall clock, so the row reports it on the branch, and `OPENBF3_AU3_GATE_LATE=1` gates it
  (as with E7's late blocks).
* The M9 reload clip shows an orange-brown strip stretched from the pistol to the frame edge (`gx_reload.png`). It
  looks like a viewmodel skinning or part artefact of the M9's reload; not investigated.
* The own 1p muzzle used the stand-in sample on 3,641 frames of 47,746, where the viewmodel had no muzzle joint (knife,
  grenade, switching). It is reported, not gated.
* `godot.m2a_firefight` (E9, E13, E14) and `godot.m2a_load` (E16) were not run on the branch (TIMING rule). They are
  the integrator's merge-pass rows.
* The wave-2 fakes in `vm_script.{h,cpp}` (`m2a_fake::present_weapon`), `tests/anim_viewmodel_tests.cpp` and
  `tests/m2a_contract_tests.cpp` stay. They are the reference predicates of those tests, and the real parts do not
  replace them one for one.

## 10. The verify fix (as built)

The verifier's blocking findings on the end-to-end step, each fixed in `m2a_check.cpp` (gx), with
`level_session_m2a.{h,cpp}` (gx) for the server-side records. Branch results: unlocked, shared machine, so the
timings are indicative only.

### 10.1 What changed

* **E19 (ads)**:
  * The FOV time is gated at ± 1 display frame (the run's worst frame dt). It is measured from the first frame that
    presents the new ADS level.
  * The press-to-presented latency is its own row, gated ≤ 0.1 s. A frame-exact gate cannot include one predicted
    tick plus the presentation delay. m2a.md E19 now says this (rule-fix commit b4a3899).
  * New rows:
    * the viewmodel FOV equals `RenderFov` 55 at hip;
    * the camera recoil equals the presented recoil every frame (camera − view − kick against `weapon.recoil`,
      ± 1e-4°);
    * the springs settle within the longest configured settle time (+ 1 frame) of the last shot cue;
    * ADS is refused while deploying: no presented zoom, `ads_allowed` false, the FOV stays at hip, and the held
      ADS zooms once the deploy ends;
    * ADS is refused while sprinting.
* **E11 (kits)**: in every loadout, slots 1, 7, 6, 0.
  * Every item deploys (its Deploy clip or a stand-in). The vm clip / stand-in list is printed per item.
  * Every item fires (a Fire / FireSingle / Knife / Throw clip). The automatic rifles have no fire clip, so for them
    the Shot cue on vm's procedural kick stand-in counts (`viewmodel_coverage` entry 1 is `ProceduralStandIn`).
  * Every firearm reloads (its Reload clip).
  * Server events equal predicted events: shots field for field, plus the sorted timed (time, event, slot) tuples.
  * The knife hits the target bot. The damage must be applied only when the bot is an enemy. For the RU loadouts
    the bot is a teammate (friendly fire off).
  * The M67 fuse is exactly 3.6 s and equals `TimeToLive`. The fuse comes from the server's own `DetonationEvent`
    (new `M2aServerRecorder::projectiles`), not from its wire copy. The client's presented Detonation has its own
    row: the wire quantizes it to 1/65536 tick, measured at −3 units.
* **E3 / E4 (range)**:
  * E3 adds head at 60 m (3 hits) and legs at 5 m (5 hits, 22.75 each). A missing lane now fails its row.
  * E4 adds the MK11 (recon): a head shot at 5 m kills in 1 (117.5), at 60 m in 2.
* **E5 (reload)**: the M16A3 and M9, tactical and empty.
  * Per display frame, against the predicted `kReloadStarted` / `kAmmoCommitted` / `kReloadFinished`: the reload
    clip start, the HUD ammo change and the ready frame.
  * The gate is late ≤ 1 frame and early ≤ 1 tick. Early is allowed because a presented weapon's discrete fields
    come from the newer predicted tick.
  * The M16A3 with the trigger held through the reload fires its first shot exactly at `kReloadFinished`, and its
    Shot cue comes within 1 frame.
  * The clip lengths are printed per kit weapon.
* **E8 (5) (fx)**: Metro's collision tags are searched for Water (18). The search found 0 Water tags among 1,818
  tags. Concrete and brick resolve through the same table, so the row passes on the search, not on a constant. If
  Metro had reachable water, the case would shoot it and gate the effect with no decal.
* **E1 / E10**:
  * E1: one fx3 1p muzzle start per predicted shot.
  * E1 gaps for every kit firearm:
    * held automatic: equal to the interval;
    * single fire: tapped every 2 ticks so each tap is a new press, with every gap ≥ the interval and the smallest
      within the tap period.
  * E1: MK11 taps give one shot per tap and one per held trigger.
  * E10 checks what the widgets drew (`Bf3HudDrawRecord`, a frame later):
    * the crosshair gap against (h/2)·tan(spread)/tan(vfov/2), ± 0.5 px;
    * the ammo at hudmpscreen's `Ammo_01`, x 1194 ± 2 stage px, in `$HUD_medium`;
    * screenshots `hud_hip`, `hud_reload` and `hud_ads`.
* **Ownership**: m2a.md §15 lists gx's two named replacements:
  * R6: b7652bc, in fx3's `game_fx.cpp`;
  * R7: 589a3b0, in au3's `tests/audio_router_tests.cpp`.

  Commit 0009ec3, which adds them, says the rule proved too narrow.
* **Nonblocking**:
  * `play.killed_by_enemy` ≥ 1 is gated in a build with bt.
  * `godot.m2a` and `godot.m2a.kits` take `-DNO_ERRORS=1`.
  * The status line reads "firefight bots N (bt, free|pairs), others in the snapshot K".
  * The firefight and ADS shots need a bot within 45 m, centred, with five rays (head, chest, hips, chest ± 0.3 m
    across) clear of every collision set. A nearer framed bot re-takes the shot, and the ADS shot is taken between
    bursts.

### 10.2 Results (2026-10-18, branch, unlocked, shared machine: timings indicative only)

| row | command | result |
|---|---|---|
| `godot.m2a` | `ctest --preset dev-window -R "^godot\.m2a$"` (with `OPENBF3_GODOT`, `OPENBF3_GODOT_WINDOWED=1`) | PASS, 6 / 6 cases, 252 rows, 0 failed, 0 ERROR lines; 393 s (range 52.6, reload 22.1, hud 7.7, ads 13.0, audio 124.6, fx 164.0) |
| `godot.m2a.kits` | `ctest --preset dev-window -R "^godot\.m2a\.kits$"` | PASS, 276 rows, 0 failed, 0 ERROR lines; 161 s |
| `godot.m2a.play` | `ctest --preset dev-window -R "^godot\.m2a\.play$"` | PASS, 0 ERROR lines, `killed_by_enemy` 4 (gated ≥ 1); 311 s |
| branch pass | `tools\build.cmd dev --strict` (`OPENBF3_GODOT` set) | exit 0: 137 rows, 135 passed, 2 skipped (`core.cache_data`, `core.res`), 0 failed, 0 STRICT violations; 187 s |

* **E19**:
  * fov_in 0.1167 s (target 0.120 ± 0.0056 s); fov_out 0.150 s (target 0.150);
  * press latency 0.011 s;
  * recoil worst 1.34e-5° over 398 frames (largest recoil 0.563°);
  * springs at rest after 0.489 s (limit 1.0 s);
  * deploy refusal: 96 frames, 0 zoomed, lowest FOV 55.00;
  * sprint refusal: 145 frames;
  * viewmodel FOV 55.00 (weapon and arms).
* **E11**:
  * 8 / 8 loadouts.
  * The M67 fuse is 216,000,000 units (3.6 s), equal to `TimeToLive`, in every loadout. Release to detonation
    3.77 s; the presented Detonation is −3 units off.
  * The knife hit is applied on the US side; on the RU side it is 1 hit, not applied (a teammate).
  * `events_equal` passes for every item.
* **E1 gaps** (held / tapped; units):
  * M16A3 / M4A1: 4.5 M;
  * M27: 4.8 M;
  * AK-74M: 5,538,462;
  * M9 / MP443: min 10 M against a 9 M interval;
  * MK11 / SVD: min 14 M against 13,846,154;
  * the server fired the same shots.
* **E3 / E4**:
  * M16A3: chest at 5 m 4 hits (25), head at 5 m 2 (50), legs at 5 m 5 (22.75), chest at 60 m 6, head at 60 m 3.
  * MK11: head at 5 m 1 hit (117.5), head at 60 m 2.
  * Every kill was counted, with the hit indicator once per server hit.
  * E1: 31 1p muzzle starts for 31 shots.
* **E5**:
  * M16A3: tactical 1.8 s with the commit at 1.53 s; empty 2.37 s with the commit at 2.0145 s.
  * M9: tactical 1.35 s, commit 1.0125 s; empty 1.6 s, commit 1.2 s.
  * The clip starts 0 frames late, and ready is 0 frames late.
  * The HUD ammo changes up to 2 frames early (0.0111 s at most, under a tick).
  * The held trigger fires exactly at the finish.
* **E5 clip lengths** (reload clip / empty clip in s, against `ReloadTimeBulletsLeft` / `ReloadTime`):

  | weapon | reload / empty clip | `ReloadTimeBulletsLeft` / `ReloadTime` |
  |---|---|---|
  | M16A3 | 2.333 / 2.900 | 1.80 / 2.37 |
  | M9 | 1.900 / 2.167 | 1.35 / 1.60 |
  | M4A1 | 2.267 / 2.867 | 1.85 / 2.48 |
  | M27 | 2.333 / 2.900 | 1.90 / 2.50 |
  | MK11 | 2.267 / 2.867 | 1.85 / 2.40 |
  | AK-74M | 2.667 / 3.600 | 2.10 / 3.10 |
  | MP443 | 1.900 / 2.167 | 1.35 / 1.60 |
  | AKS-74u | 2.333 / 3.333 | 1.90 / 3.00 |
  | RPK-74M | 3.600 / 4.467 | 3.00 / 4.00 |
  | SVD | 2.967 / 3.867 | 2.15 / 3.30 |

  The authored clips are longer than the sim times. The sim times gate; the clip is presentation.
* **E10**:
  * crosshair gap error 0 px over 1,096 frames;
  * ammo at x 1194.00 over 1,096 frames, 0 in the wrong font;
  * 1 stale widget record (limit 10).
* **The BF3 install is unchanged**:
  * a read-only listing: 1,918 files, none written since 2026-10-17 (newest 2026-04-25);
  * the suites' `install.unchanged` cases pass in the branch pass.

### 10.3 Screenshots

In `b\shots\gx\`, 1152 x 648:

* `hud_hip.png`: the M16A3 at the hip by a bus shelter, with the crosshair and `26 / 118`.
* `hud_reload.png`: the rifle tilted in its reload clip, `23 / 118`.
* `hud_ads.png`: the iron sights, the crosshair hidden, `31 / 110`.
* `gx_hip.png`: the M16A3 at the hip at a doorway, with impact smoke on the wall and `26 / 155`. The status line ends
  "firefight bots 16 (bt, free), others in the snapshot 16".
* `gx_reload.png`: the M16A3 tilted in its reload, `30 / 125`.
* `gx_death.png`: `KILLED BY slot 9 (RU)` and `respawn in 1.5 s`, with a 3p soldier in the lower left.
* `gx_firefight.png` (the bot at 21.5 m): a street with a red van. The bot is in a shadowed doorway at the crosshair:
  small and dark.
* `gx_ads.png` (the bot at 24.9 m): the rear sight ring. The bot is a dark shape in the ring, partly behind the front
  post, and hard to make out.

### 10.4 Open items

* The firefight / ADS framing sees only the check's collision world, not foliage, other render-only occluders,
  lighting or the iron sight's own post.
  * Earlier runs framed a bot behind a tree and one behind a wall corner. The five rays fixed the wall corner.
  * The kept shots show the bot, but small.
  * A test on the rendered frame (the crowd's own pixels) would close this. It is not built.
* `audio_late_commands` (8 in the last play run) and the double starts from reused shot ids (6) remain as §9.5 says.
