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
| the server's parts | `src/bf3_host/src/game_server.cpp` runs the tick over the `server_*` parts, each owning its state, the others reading it: `SlotTable` (seats: client, local or probe, peer, team, command source), `Soldiers` (spawned, state, last command, events for the next snapshot), `CommandBuffers` (jitter buffers), `MatchControl` (TDM rules, respawn policy, match messages), `Combat` (gunplay and its per-slot state), `CombatLog` (the hit/death sink), `Spawner` (spawns, placements, revives and the respawn clock), `SnapshotWriter`, `DevCommands` (debug requests, traces, spawn sweep), `SlotView` (the `ServerView`), `Outbox` |
| the client's parts | `src/bf3_host/src/client_session.cpp` owns the client thread and the ENet `ClientTransport`; `client_loop.cpp` (`ClientLoop`, driven by `iterate` on a `MonotonicClock`) runs the receive, tick and publish order over the `client_*` parts, each owning its state: `ClientLink` (transport, NetSims, snapshot acks, command packets, request queues), `ClientClock` (clock estimate, slew, buffer reports, RTT), `ClientInput` (input mailbox, command sampling), `OwnPrediction` (motor, prediction ring, reconciliation, visual offset), `RemoteTracks` (interpolation of the others), `ClientEvents` (the `PresentationEvents` feed), `ClientMatch` (match state, players, own death, restriction), `ClientGunplay` (weapon lane, received records, projectiles, shot logs) |
| TDM rules and the round flow | `src/bf3_host/src/tdm_rules.cpp`, the logic runtime in `src/bf3_sim/src/logic/` |
| bots | `src/bf3_sim/src/bots/` (perception, walking), `firefight_brain.cpp`, and `src/bf3_host/src/tdm_brain.cpp`, whose `run` senses (`sense`: shots and perception, then target choice and tracking, then the revive check), picks the activity (`choose_activity`), plans a `BotIntent` per activity, turns the view and composes the command over the `tdm_bot_*` parts, each owning its state: `TdmShared` (heat map, recent shots), `BotAwareness` (perception, being shot at), `BotTargeting` (target choice, tracking, aim error), `BotNavigation`, `BotStrafe`, `BotLook` (movement), `BotArms`, `BotWeapons` (slots, bursts, reloads), `BotRevive`, `BotGrenade`, `BotRetreat`, `BotHunt` (objectives) |
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
* The scoreboard, spawn, pre-round, round-end and kill-card screens (`src/bf3_godot/src/hud_scoreboard.cpp`,
  `spawn_screen.cpp`, `pre_round.cpp`, `round_end.cpp`, `kill_card.cpp`) are hand-built Godot
  controls that only borrow the retail fonts and atlas regions; the in-round HUD is the retail movies (`Bf3NativeHud`). The rule is the retail SWF movies
  rendered as they are (CLAUDE.md "Native Battlefield 3 UI only"); the SWF decoders in `bf3_core/ui/` give symbol
  tables, atlases, fonts, triangulated shapes, a live display tree (`SwfScene`), text layout and the AVM1 bytecode
  (`avm1.h`: every DoAction, DoInitAction and clip-action body, and the classes and `Object.registerClass` calls found
  in it; `bf3dump swf as2`), so the renderer and the screen logic are open. The scene runs only the frame scripts'
  unconditional stop/play/goto and hands every other frame script to the HUD controller that owns the clip; the movies'
  AS2 classes, `onClipEvent` handlers and conditional frame code are not executed outside the HUD controllers. `Bf3SwfView` draws no gradients, static text, Bevel or ColorMatrix filters,
  or blend modes other than normal, layer and add. The HUD does not sway with camera motion, the minimap draws at
  1.375 m per local unit, measured at MapZoom 0 (the small map InitSmall starts with); the large map's zoom 1.2 reaches the HUD's Zoom binding but not the drawn scale, and nothing toggles the map size, the Reward rows (HudMessage_04, `Widget.Image.ImageManager`) are not driven, and the engine's UI
  colour table behind `Util.ColorTinting` is read from hudmessage's kill-row name sprite (slot 0 its squad frame, 1 friendly,
  3 enemy, slot 2 none), which the footage's kill log killer and victim colours match. The engine message
  queue (`HudMessageComp`) takes queue sizes and times from `UIMessageCompData`, but its expiry, overflow and
  RemoveFlag order and the ScoreAggregation value (a sum reset after ScoreAggregateTime) are inferred, the MessageQuery DataKey to source table is measured (the key hash is unknown), and
  without squads in the model the kill feed's squad colour marks only the local player; its UpdatesPerSecond 3 is not applied.
  A kill row fades only by its timeline's `container` alpha (startAnim frames 1..4 in steps of 0.25, outAnim 10..14 in
  steps of 0.2, 0.17 s at 30 fps). In the 18.57 footage the kill feed rows fade in smoothly over about 8 frames and out
  over about 28 to about 0.4 before they vanish, 18 px apart at about 0.785 scale from y 100, just under the VU watermark
  (0..84), while the movie stacks them 26 px apart from y 0; no screen, widget or binding data, placement or AS2 scales,
  moves or fades HudMessage_02, and the kill log rows (same movie and classes) fade in the timeline's 5 frames in the same
  footage, so that kill feed look is not reproduced. The damage indicator's Scale
  (the hit's damage) and Angle (clockwise from the view, checked against the sprite's rotation) are inferred, and the
  HudAlertMessage and LogMessage widgets have no source in TDM (no combat area, squads or log lines in the model).
  The Icon3D managers (SupportIconManager, MapmarkerManager, AlertManager) run their AS2 and create, update and remove
  their tags from `HudClientState::world_tags` at stage positions; nothing in TDM fills it (no requests, markers, spotting
  or grenade alerts in the model), and the engine component's placement (UI3dIconCompData snap, fade and scale) is not done.
  The moment fixture's fading kill rows sit on the nearest timeline frame with the row still present (0.20 for 0.00 and
  0.06, 0.50 for 0.28 and 0.43 fading in).
* The in-round HUD does not pass the strict 1280x720 screenshot gate (the 16 footage moments against the moment
  fixture's captures); there is no gate row, because the owner deferred pixel-exact HUD work on 2026-10-24. The main
  visible differences: the kill feed rows sit larger and higher than the footage's, the minimap has no team-mate,
  squad-mate or revive icons and a flatter interior, and the footage's glowing friendly markers over soldiers are not reproduced.
* The crosshair arm is `round(100 (0.2 x spread deg + DispersionBaseOffset))` stage px; the 0.2 per degree is measured on
  the 18.57 footage at FOV 100 (`ui.hud.feed.install` footage rows) and its dependence on the field of view is not known.
  In the footage the moving minimum follows the motor's speed / walk speed as the soldier accelerates and stops (the
  `move_onset` / `move_stop` rows); the sim applies it in full above 0.1 m/s. That, the footage's bloom while firing, its
  fall after a landing without a shot and its suppressed sprint (5 deg) and jump (7 deg) arms differ from `spread_at`; the
  footage rows print the sim's values as INFO.
* Nametags are engine-drawn by `Bf3NativeHud` through `NativeTags` (`native_tags.cpp`): UIHudIcon_Player by relation, the
  squad kit icon for squad mates, names in `$Menu_medium` at the UINametagCompData sizes and tints; the name's kit icon
  (16 units in an 18-unit column) and its black glow are not measured, and the HealthBarSize bar is not drawn (other
  soldiers' health is not presented). Squads, leaders, voice and boosters are not modelled, so the SquadList, squad tints
  and squad kit icons show only in the moment fixture. Data.UINametagComp.getUIColor is native, so the SquadList takes its
  slot 0 as the squadlist txt field's authored colour (e4ff9d; only the icons' setRGB changes the look) and leaves out
  tint2Texts' shadow and glow filters, whose colours are not in the install.
* The container, EBX, payload, resource, level-soup and lag-compensation readers were written as ports of the
  earlier Rust experiment (IW4L); on 2026-10-24 the owner decided to keep them as they are, not re-derive them, and the
  install tests hold their numbers. No Rust is left in the tree.
