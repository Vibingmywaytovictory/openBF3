# openBF3 architecture, as it stands

This page describes the system in the tree today: what the libraries are, how they depend on each other, which
threads run what, and how data flows from a retail Battlefield 3 install to the screen and over the wire. It is the
page to read before any design document. The per-track designs with their as-built notes are in
[design/](design/); the milestone record with its measurements is [history/dev-log.md](history/dev-log.md).

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
bf3_net      -> ENet           bit streams, protocol v3, network simulator, loopback ping
  ^
bf3_host     -> anim           server: ServerHost thread, GameServer (socket-free), TDM rules, bots, lag comp, gunplay
  ^    ^                       client: ClientSession, projectiles, presentation events, HUD feed
  |    |
bf3_fx bf3_audio               effects compiler and particle simulator; mixer core, DSP, patches, routing
  ^      ^                     (both link bf3_host today only for the presentation event types; see "Known debts")
  |      |
bf3_godot (shared library)     the GDExtension: sessions, bridges to RenderingServer and AudioStream, HUD controls,
                               viewmodel, plus the headless checks and demos
```

Tools: `tools/bf3dump` (install inspector, cache preparer, many `*_commands.cpp`), `tools/antdump`,
`tools/resdump`. Tests: `tests/` builds one doctest executable per area; `tests/CMakeLists.txt` registers the ctest
rows and `tests/godot_*.cmake` run the Godot project headless or windowed ([testing.md](testing.md)).

## Threads

| thread | owner | runs | talks to the others through |
|---|---|---|---|
| Godot main | Godot | scene tree, `_process`, all Godot API calls, HUD, the presentation of the client state | reads `PresentationFrame` copies; posts commands |
| server | `ServerHost` | `GameServer::tick` at 60 Hz: jitter buffers, bot brains, motor steps, rules, snapshots | datagrams in and out via the ENet host; stats copied under a mutex |
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
   soldier with the same motor and interpolates the others.
5. **Presentation.** The client publishes a `PresentationFrame` and `PresentEvent`s; `bf3_godot` turns them into
   transforms, viewmodel poses, HUD text, effect triggers (`bf3_fx::Triggers`) and audio commands (`bf3_audio`).
   Level geometry goes straight to `RenderingServer` (`level_upload.cpp`); soldiers through skeleton uploads
   (`skin_upload.cpp`); particles through `Bf3FxRenderer`; sound through `Bf3AudioStream`.

## Configuration and tuning

Runtime options are Godot user args (`-- --bf3-install=...`, `--quit-after=`, `--screenshot=`, the check switches),
read through `godot_util.h`'s `user_arg_value`. Tuning lives in `bf3-opensource/config/*.cfg` as key=value files
with ranges checked at load (`motor_tuning.cpp`, `vehicle_tuning.cpp`, `mechanics_tuning.cpp`, `tdm_rules.cpp`,
`tdm_brain.cpp`, `netsim.cpp` each carry a parser today).

## Where things live

| you are looking for | start at |
|---|---|
| how a file in the install is found and read | `src/bf3_core/src/install*.cpp`, `superbundle_mount.cpp`, `payload.cpp` |
| an EBX field | `src/bf3_core/src/ebx.cpp`, `gameplay/ebx_fields.cpp`, then the table in `gameplay/` |
| the player's movement | `src/bf3_sim/src/soldier_motor.cpp`, `prediction.cpp` |
| shooting, damage, hit registration | `src/bf3_sim/src/weapons/`, `src/bf3_host/src/gunplay.cpp`, `lag_comp.cpp` |
| the wire format | `src/bf3_net/src/protocol.cpp` (read/write pairs are adjacent) |
| the server tick order | `src/bf3_host/include/bf3/host/game_server.h` header comment, `game_server.cpp` `run_tick` |
| TDM rules and the round flow | `src/bf3_host/src/tdm_rules.cpp`, the logic runtime in `src/bf3_sim/src/logic/` |
| bots | `src/bf3_sim/src/bots/` (perception, walking), `src/bf3_host/src/tdm_brain.cpp`, `firefight_brain.cpp` |
| what the player sees each frame | `src/bf3_godot/src/level_session*.cpp`, `render_bridge.cpp`, `viewmodel.cpp`, `hud_*.cpp` |
| effects | `src/bf3_fx/src/effect_compiler.cpp`, `simulator.cpp`, `triggers.cpp`, `src/bf3_godot/src/fx_renderer.cpp` |
| sound | `src/bf3_audio/src/mixer_core.cpp`, `patch_*.cpp`, `weapon_audio.cpp`, `src/bf3_godot/src/audio_stream.cpp` |
| a ctest row | `tests/CMakeLists.txt`, the matching `tests/*_tests.cpp` or `tests/godot_*.cmake` |

## Known debts

These are structural, known, and listed so nobody mistakes them for design intent.

* `bf3_fx` and `bf3_audio` depend on `bf3_host` (and so on sim, net and anim) for `PresentEvent`, `PresentClock`,
  `PresentedSoldier` and two seed hashes. Those are PODs that belong in a lower `bf3_present` library; the two audio
  glue files that call real host logic (`game_audio_frame.cpp`, `kit_audio.cpp`) belong above the audio runtime.
* `bf3_host` holds both the server and the client. A `bf3_client` split would make the dependency picture honest.
* `bf3_godot` and `bf3_sim` compile their check, demo and harness code into the shipped extension; it should sit
  behind a build option.
* Milestone names (M1, M2a, M2b, S0, PT1) survive in file names, class names and thousands of comment tags; the
  dev log is where that history belongs.
* The same small helpers (`fmt`, `seconds_since`, `finite3`, `trim`, `lower`, a key=value parser, a bounds-checked
  reader) are defined in many translation units instead of once in `bf3_core`.
* `bf3_core` is several libraries in one directory (containers, assets, gameplay tables, level, UI).
* The build is Windows and MSVC only by policy; Win32 use is confined to about twenty files.
