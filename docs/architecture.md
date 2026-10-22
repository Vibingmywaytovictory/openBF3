# openBF3 architecture, as it stands

This page describes the system in the tree today: what the libraries are, how they depend on each other, which
threads run what, and how data flows from a retail Battlefield 3 install to the screen and over the wire. It is the
page to read before any design document, and the only place where the code is explained in words: the code has
no comments, by rule ([CLAUDE.md](../CLAUDE.md), enforced by the ctest row `hygiene.rules`). The per-track
designs with their as-built notes are in [design/](design/); the milestone record with its measurements is
[history/dev-log.md](history/dev-log.md).

## Shape

openBF3 is a Godot 4.8 GDExtension written entirely in C++20. Godot provides the window, the renderer, the audio
device, input and the scene tree. Everything else is our own: the install readers, the simulation (our own Jolt
world, not Godot's physics), the network protocol (upstream ENet), the bots, the effects simulator and the audio
mixer. Only one library, `bf3_godot`, includes a Godot header. The rest are static libraries that also link into the
command-line tools and the doctest executables, which is how most of the code is tested without a window.

The retail install is read in place and never written (`InstallReader`, `PathGuard`, README "The read-as-is
rule"). Derived data goes to a content-addressed disk cache under `user://bf3cache/`.

## Libraries and their dependency order

Lower libraries never include higher ones. Arrows read "links".

```
bf3_core                       install, containers, EBX, cache, res/ant/audio asset decoders, gameplay tables,
  ^    ^                       level traversal, logic graph, UI (SWF, fonts, text, HUD model, minimap)
  |    |
  |  bf3_anim                  skeletons, poses, clips, skinning, crowd evaluator, viewmodel animation
  |    ^
  |  bf3_render                mesh/texture/material preparation, level scene, viewmodel set (uses anim)
  |
bf3_sim      -> Jolt, Recast, Detour (hidden behind *_internal.h; no engine type in a public header)
  ^                            collision and physics worlds, soldier motor, prediction, weapons, vehicles,
  |                            navigation, bot perception and walking, level logic runtime
  |  bf3_sim_harness           gate code over bf3_sim (logic harness, vehicle courses and replays, shooting range,
  |                            weapon checks and W4 tables, nav input check); tests, tools and the checks link it
  |  bf3_present               presentation PODs: PresentEvent and its ring, PresentClock, presented soldiers and
  |    ^                       weapons, effect seeds
  |    |
  |  bf3_fx bf3_audio          effects compiler and particle simulator; mixer core, DSP, patches, routing
  |                            (both link only bf3_present and bf3_core; bf3_host links bf3_present too)
bf3_net      -> ENet           bit streams, protocol v3, network simulator, loopback ping
  ^
bf3_host     -> anim, present  server: ServerHost thread, GameServer (socket-free), TDM rules, bots, lag comp, gunplay
  ^                            client: ClientSession, projectiles, HUD feed; re-exports the bf3_present types
  |
bf3_client_audio -> audio      GameAudioDriver (presentation frame to router), kit audio sources, game audio loading
  ^
bf3_godot (shared library)     the GDExtension: sessions, bridges to RenderingServer and AudioStream, HUD controls,
                               viewmodel; the headless checks and demos only with OPENBF3_BUILD_CHECKS
```

Tools: `tools/bf3dump` (install inspector, cache preparer, many `*_commands.cpp`), `tools/antdump`,
`tools/resdump`. Shared helpers live once in `bf3_core`: `bf3/core/math.h` (`Vec3`, which every module's `Vec3` is,
and `Transform`, which `res::Transform`, `fx::Transform` and `DriveTransform` are; `LevelTransform` adds a scale) and
`bf3/core/util/` (`fmt`, `seconds_since`, `finite`, ASCII text, `splitmix64`, percentiles, the key=value grammar).
Tests: one doctest executable per area in `tests/` (the install fixture `Mounted`/`open_or_skip` and `wait_until` in
`test_support.h`); `tests/CMakeLists.txt` registers the rows; `tests/godot_*.cmake` run Godot headless or windowed
through `godot_common.cmake` (the window probe, `godot_run`, `godot_require_pass`; [testing.md](testing.md)).

## Threads

| thread | owner | runs | talks to the others through |
|---|---|---|---|
| Godot main | Godot | scene tree, `_process`, all Godot API calls, HUD, the presentation of the client state | reads `PresentationFrame` copies; posts commands |
| server | `ServerHost` | `GameServer::tick` at 60 Hz, paced by a `TickClock` (steady; stepped in tests): jitter buffers, bot brains, motor steps, rules, snapshots | datagrams in and out via the ENet host; stats copied under a mutex |
| client network | `ClientSession` | receive, clock slew, prediction, interpolation, gunplay client side | `PresentationFrame` (trivially copyable, memcpy'd), `PresentationEvents` SPSC ring |
| audio | Godot's audio driver via `Bf3AudioStream::_mix` | `MixerCore::mix`: never allocates, locks or waits | three SPSC rings (commands, returns, feedback) |
| loaders and `WorkerThreadPool` | level session, cache | level decode and upload preparation, cache conversions | `std::atomic<bool>` done flags with acquire/release; results joined on the main thread |

Determinism of the server simulation rests on: a fixed 60 Hz tick, integer weapon times, Jolt built with
`CROSS_PLATFORM_DETERMINISTIC`, the soldier motor avoiding libm except `sqrt`, `std::unordered_map` used for lookup
only, and `/fp:precise` pinned in the root CMakeLists. The bot brains use libm transcendental functions, so a replay
is bit-stable per toolchain, not across them.

## Data flow

1. **Install to objects.** `Install` finds packages and CAS catalogs; `LevelMount` merges base, patch and delta
   bundles; `EbxLibrary` parses partitions through their own type descriptors and interns type tables. Every size
   read from the install is bounded against the buffer that holds it before any allocation (readers are sticky-fail
   cursors with a `WorkBudget`).
2. **Objects to tables.** `gameplay/` turns EBX into weapon, kit, soldier, vehicle and mode tables and the level
   logic graph; `level/` into spawn points, mode areas, connections and the collision soup; `ui/` into the HUD model,
   minimap and fonts. These are what the simulation and the HUD consume; nothing above `bf3_core` touches EBX.
3. **Tables to the cache.** `bf3_render`, `bf3_fx` and `bf3_audio` convert meshes, textures, effects and sound banks
   into prepared, Godot-shaped blobs keyed by content digest (`cached_convert`). `bf3dump prepare` fills the cache
   ahead of time; the game fills it lazily otherwise.
4. **Simulation.** `GameServer` holds the slots, steps `SoldierMotor` (a pure `step(state, command)` with a full reset
   each tick), runs `Gunplay`, `TdmRules` and the bot brains, and encodes `Snapshot`s. `ClientSession` predicts its own
   soldier with the same motor and interpolates the others. The server tick, in order: commands (one per slot from
   its jitter buffer by `Command::tick`; a missing one is the last command repeated with edges cleared; a newer
   sequence for an already buffered tick replaces it, older copies are dropped), command sources (bots and scripted
   walkers write theirs), motor steps for every live soldier in slot order, rules (projectiles, damage, deaths,
   `TdmRules::step`, spawns settled and stamped at this tick with full protection, dev Teleport and SpawnAt applied
   here so the motor's tick precondition holds), lag-compensation history, then every `snapshot_divisor` ticks one
   `Snapshot` per client with the nearest others first and the rest in a `SnapshotOthers` of the same seq. A slot
   is the ghost id of its soldier; `SoldierState::events` is ORed per receiving client until its next snapshot.
5. **Presentation.** The client publishes a `PresentationFrame` and `PresentEvent`s; `bf3_godot` turns them into
   transforms, viewmodel poses, HUD text, effect triggers (`bf3_fx::Triggers`) and audio commands (`bf3_audio`).
   Level geometry goes straight to `RenderingServer` (`level_upload.cpp`); soldiers through skeleton uploads
   (`skin_upload.cpp`); particles through `Bf3FxRenderer`; sound through `Bf3AudioStream`.

## Configuration and tuning

Runtime options are Godot user args (`-- --bf3-install=...`, `--quit-after=`, `--screenshot=`, the check switches),
read through `godot_util.h`'s `user_arg_value`. Tuning lives in `bf3-opensource/config/*.cfg` as key=value files
with ranges checked at load: motor, vehicle, mechanics, TDM rules, bot tuning and the netsim profile are `KeyDef`
tables over one grammar (`bf3/core/util/key_values.h`, byte and line caps on every file), each with its own messages.

`OPENBF3_BUILD_CHECKS` (ON in the `dev` preset, OFF in `release`) compiles the `*_check`, `*_demo`, `s0_selftest` and
`fx_ref` sources into `bf3_godot` and registers their classes; without it a check switch is reported and ignored.

## Where things live

| you are looking for | start at |
|---|---|
| how a file in the install is found and read | `src/bf3_core/src/install*.cpp`, `superbundle_mount.cpp`, `payload.cpp` |
| an EBX field | `src/bf3_core/src/ebx.cpp`, `gameplay/ebx_fields.cpp`, then the table in `gameplay/` |
| the player's movement | `src/bf3_sim/src/soldier_motor.cpp`, `prediction.cpp` |
| shooting, damage, hit registration | `src/bf3_sim/src/weapons/`, `src/bf3_host/src/gunplay.cpp`, `lag_comp.cpp` |
| the wire format | `src/bf3_net/src/protocol.cpp`: one `serialize` per message, run by the writer, the reader and the `BitCounter` that sizes the layout (`bit_archive.h`) |
| the server tick order | "Data flow" step 4 above, then `game_server.cpp` `run_tick` |
| TDM rules and the round flow | `src/bf3_host/src/tdm_rules.cpp`, the logic runtime in `src/bf3_sim/src/logic/` |
| bots | `src/bf3_sim/src/bots/` (perception, walking), `src/bf3_host/src/tdm_brain.cpp`, `firefight_brain.cpp` |
| what the player sees each frame | `src/bf3_godot/src/level_session*.cpp`, `render_bridge.cpp`, `viewmodel.cpp`, `hud_*.cpp` |
| effects | `src/bf3_fx/src/effect_compiler.cpp`, `simulator.cpp`, `triggers.cpp`, `src/bf3_godot/src/fx_renderer.cpp` |
| sound | `src/bf3_audio/src/mixer_core.cpp`, `patch_*.cpp`, `weapon_audio.cpp`, `src/bf3_godot/src/audio_stream.cpp` |
| a SWF shape's triangles | `src/bf3_core/src/ui/swf_shape.cpp` (edges to fill0/fill1 contours, curves flattened to 1/40 px), `swf_geometry.cpp` (nonzero sweep) |
| a HUD movie's live display tree | `src/bf3_core/src/ui/swf_scene.cpp` (placements across imports, frame stepping, frame-script stop/play/goto, attachMovie, change lists; script overrides outlive seeks; ids are slot plus generation), `swf_actions.cpp` |
| SWF text layout | `src/bf3_core/src/ui/swf_text.cpp` (HTML subset, DefineFont3 advances and kerning, 2 px gutter, autoSize, GFx shrink/fit) |
| a HUD movie on screen | `src/bf3_godot/src/swf_view.cpp` (`Bf3SwfView`: a `SwfScene` as RenderingServer canvas items fed by its change list; filters, clip-depth masks and blend layers are offscreen viewports; `_xrotation`/`_yrotation` tilt is a homography; engine-drawn targets by renderer string) |
| the in-round HUD | `src/bf3_godot/src/native_hud.cpp` (`Bf3NativeHud`: hudmpscreen, hudtdmscreen and crosshairdefault as three `Bf3SwfView`s at window size, stepped by `HudScreen` from `HudModel`; paints the engine-drawn iBgDraw, EKG and minimap targets), `src/bf3_core/src/ui/hud_screen.cpp`, `hud_widgets.cpp` (one controller per AS2 class) |
| a ctest row | `tests/CMakeLists.txt`, the matching `tests/*_tests.cpp` or `tests/godot_*.cmake` |

## Known debts

These are structural, known, and listed so nobody mistakes them for design intent.

* `bf3_host` holds both the server and the client. A `bf3_client` split would make the dependency picture honest.
* Gate code still sits in three libraries: `bf3_audio` (`audio_check`, `game_audio_check`, which `game_audio` and
  `bf3_client_audio` call), `bf3_fx` (`fx_harness`) and `bf3_host` (`rest_check`).
* Milestone names (M1, M2a, M2b, S0, S3, PT1) remain in the gated checks, demos and selftest (`m1_check`,
  `m2a_check`, `m2b_check`, their scenes, user args and ctest rows), the test files, `HudWave` and `--look=m1`.
* A bounds-checked byte reader, the small vector operators (`add`, `dot`, `cross`, `rotate`) and two
  copies of `thread_cpu_seconds` (`scale_bench.cpp`, `bf3dump/audio_commands.cpp`) are still defined per
  translation unit instead of once in `bf3_core`.
* `bf3_core` is several libraries in one directory (containers, assets, gameplay tables, level, UI).
* The build is Windows and MSVC only by policy; Win32 use is confined to about twenty files.
* The kill feed, scoreboard, spawn, pre-round, round-end and kill-card screens (`src/bf3_godot/src/hud_killfeed.cpp`,
  `hud_scoreboard.cpp`, `spawn_screen.cpp`, `pre_round.cpp`, `round_end.cpp`, `kill_card.cpp`) are hand-built Godot
  controls that only borrow the retail fonts and atlas regions; the in-round HUD is the retail movies (`Bf3NativeHud`). The rule is the retail SWF movies
  rendered as they are (CLAUDE.md "Native Battlefield 3 UI only"); the SWF decoders in `bf3_core/ui/` give symbol
  tables, atlases, fonts, triangulated shapes, a live display tree (`SwfScene`), text layout and the AVM1 bytecode
  (`avm1.h`: every DoAction, DoInitAction and clip-action body, and the classes and `Object.registerClass` calls found
  in it; `bf3dump swf as2`), so the renderer and the screen logic are open. The scene runs only the frame scripts'
  unconditional stop/play/goto; the movies' AS2 classes, `onClipEvent` handlers and conditional frame code are not
  executed outside the HUD controllers. `Bf3SwfView` draws no gradients, static text, Bevel or ColorMatrix filters,
  or blend modes other than normal, layer and add. The HUD does not sway with camera motion, the minimap zoom is a
  fixed 1.375 m per local unit, and the KillFeed, SquadList, ScorePopup and KillLog widgets are not driven.
* The container, EBX, payload, resource, level-soup and lag-compensation readers were written as ports of the
  earlier Rust experiment (IW4L) rather than from the format facts alone. The rule now forbids that
  (CLAUDE.md "No code from other projects"); those units are to be re-derived from `research/` and the install
  measurements, with the existing install tests as the gate that nothing changes.
