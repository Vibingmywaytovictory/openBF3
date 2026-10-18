# M2a au3: game audio (as built)

Track `au3` of M2a wave 3, on branch `m2a/au3` from `m2a/w3` 3b3b991. Design: [m2a.md](m2a.md) §12.3, §8.3, §4.4,
§16 Q9 and gate E7; the contract is [m2a-w3.md](m2a-w3.md) §3 "au3". au's mixer, patch instances, sampler and check
([m2a-audio.md](m2a-audio.md)) are used through their public headers only. No au file was edited.

## 1. What landed

| file | what |
|---|---|
| `src/bf3_audio/include/bf3/audio/weapon_audio.h`, `src/weapon_audio.cpp` | `SimMixerClock`, `load_weapon_audio`, `OneShotPool`, `WeaponAudioRouter` (voice limits, both timelines, listener re-send) |
| `impact_audio.h`, `src/impact_audio.cpp` | `ImpactSoundCache` (fx3's impact and effect sound names, strict then lenient compile), `ImpactAudio` |
| `game_audio_check.h`, `src/game_audio_check.cpp` | `game_check_script`, `game_audition_items`, `GameAudioCheck` (MixTap + router observer + TapRecorders), `analyse_game_check` (au's `analyse_check` plus the in-game rows) |
| `kit_audio.h`, `src/kit_audio.cpp` (au3 file) | `kit_audio_sources(GunplayLevel)` (the kit items' sounds, fire modes and intervals) and `kit_audio_report` (E7 item 1) |
| `game_audio_frame.h`, `src/game_audio_frame.cpp` (au3 file) | `GameAudioDriver` (the per-frame order), `load_game_audio_assets`, `start_audition`, `compile_patch_lenient`: the Godot-free halves of `Bf3GameAudio`, so the offline rows run the game's code |
| `src/bf3_godot/src/game_audio.{h,cpp}` | `Bf3GameAudio`: the 96-instance game mixer, its `Bf3AudioStream` and `AudioStreamPlayer` "Bf3GameMixer", the latency-corrected clock, the load, the step and the auditions |
| `tests/audio_router_tests.cpp` | the `audio-router` unit suite and the `audio-game` install suite |
| `tests/m2a_contract3_tests.cpp` | `kAu3Landed = true` (its own commit) |

Shared files carry one `# M2a au3` block at their end: `src/bf3_audio/CMakeLists.txt`, `src/bf3_godot/CMakeLists.txt`
and `tests/CMakeLists.txt`.

## 2. SimMixerClock

`frame_of(t, timeline)` = `clock()` − latency + (t − now(timeline)) × rate / `kTimeUnitsPerSecond` + bias. It is clamped
to `earliest_frame()`, and every clamp is counted (`late()`, `max_late_frames()`). The delta is computed in doubles,
so hostile times (`SimTime` min / max) clamp rather than overflow. `now` comes from `host::present_clock`, so
Predicted is (tick − 1 + `present_own_fraction`) × U and Render is `present_render_ticks` × U.

* **Latency.** `Bf3GameAudio::step` reads `AudioServer::get_output_latency()` every frame. Values that are not finite,
  or outside (0, 2) s, count as 0. The result goes to `set_output_latency_frames`.
* **Bias.** Latency + `schedule_lead_blocks` × 512 + `kBiasMarginFrames` (2 × 512 + 800). That covers the mixer's lead,
  Godot's mix chunks running ahead between two display frames, and one display frame of event age. An event "now" then
  lands after `earliest_frame()` instead of being clamped.
* **Resolution.** `MixerCore::clock()` moves in 512-frame blocks. Two separate instances (single fire, one-shots)
  therefore carry up to one block (10.7 ms) of the display frame's phase between them. A burst's rounds are placed on
  the burst's own grid (first frame + k × interval), so they carry none: the E7 rehearsal's `spacing` rows measure
  0-70 frames, and every onset is exact on the sampler's Step.

## 3. WeaponAudioRouter

### Own soldier (Predicted timeline)

* **Bursts.** One `PatchInstance` per trigger hold, IsFirstPerson 1 (0 in the free camera), at the listener, seeded
  `sound_seed(own_slot, counter)`.
* **Rounds.** OwnShot events place the rounds by shot index: frame = first + (id − first_id) × interval.
* **End.** The burst ends on the first of:
  - `kTriggerDown` clearing in `own_weapons_curr.timers[0]`;
  - the predicted clock running 1.5 intervals past the last round;
  - an index gap;
  - a def change.

  The stop event goes half an interval after the last round (never before `earliest_frame`), then a Stop 2 blocks
  later.
* **Single fire.** One instance per shot, released at once (EnableStep 0).
* **Reloads.** OwnTimed `kReloadStarted` starts a one-shot on the def's patch with the Reload event, when the patch
  takes it.

### Others (Render timeline)

* **RemoteShots.** A RemoteShot arrives about the interpolation delay early. The first one of a burst starts the
  instance on its exact frame (`frame_of(t, Render)`). The seed is `sound_seed(slot, (record burst_seq + 1 + ahead) & 15)`.
  Later RemoteShots place their rounds by shot index when they are on the burst's grid (|t − grid| ≤ interval / 2) and
  inside the hold. An index gap of at most 64 means lost datagrams: those rounds are filled on their grid frames and
  counted as `state_rounds`.
* **Records.** `host::present_other_weapons` gives the firing bit, `burst_seq` and `reload_seq`.
  - They extend a burst's evidence.
  - They synthesize the rounds a lost RemoteShot left out, up to round(evidence − first) / interval.
  - They start a burst only when every RemoteShot of it was lost.
  - A reload_seq change starts the Reload one-shot.
* **Newest records.** `set_others_newest` takes the records at the frame's `newest_tick`, with the horizon at the end
  of that tick, so the burst's end is decided about the render delay early. Its stop then lands on time: half an
  interval after the last round. A burst with RemoteShots ends when horizon − last > interval + 2 ticks. Without the
  newest records it ends when render − last > interval + 3 ticks; the stop is late then, clamped and counted.
* **Ghost restarts.** Once a burst ends, records up to its last shot + 2 ticks belong to it (`ended_evidence`).
* **Single fire (fixed on this branch).** A single-fire RemoteShot instance ends at once. Its firing records used to
  start a second instance from state, so every other player's pistol or DMR shot sounded twice. The 64-shooter run
  found it: 3966 state rounds with no datagram lost. The same `ended_evidence` rule now covers the shot. The
  `audio.router.unit` row pins it: 5 single shots give 5 instances and 0 state rounds.

### Voice limit (§16 Q9)

* **Drivers.** At most 32. A new burst that finds them full kills the farthest released tail (`stolen`); with no tail
  to take, it is culled (`culled`). A running burst is never stolen.
* **Distance.** Beyond `max_weapon_distance_m` (1000 m) a burst is culled.
* **One-shots.** At most 64, the oldest stolen. Each gets a Stop after 3 s (patches with a stop event never finish on
  their own) and is killed and forgotten after 20 s, so a lost finished feedback cannot leak a slot.
* **The mixer.** 96 instances (32 + 64), so the mixer's own cap never refuses: `mixer_refusals` = 0 in every row.

### Listener

After every SetListener, the router and the one-shot pool re-send SetEmitter for each live instance: the mixer
applies a listener only on the next SetEmitter.

### Knife and M67

They are OneShot defs. The knife posts Melee (`kHashMelee` 210357473). The M67 posts Fire on RemoteShot / OwnShot,
and its explosion plays on Detonation when the def has an explosion patch. On Metro the M67's `explosion_sound` is
empty: the explosion sound comes from fx3's effect sounds.

## 4. ImpactAudio

* **ImpactSoundCache::build.** Compiles each of fx3's impact sound names once: strict first, then lenient. At most
  4096 names; failures are kept with their error.
* **play_impact.** Takes cues within 400 m with finite geometry, at `frame_of(time, timeline)`.
* **on_effect_sound.** SoundStart starts at `earliest_frame()`, seeded splitmix64(effect ^ component << 48), with at
  most 128 tracked. SoundStop stops by key (effect, component).
* **Unknown patches.** Counted (`unknown_patches`) and silent. Impacts and effect sounds share the router's
  `OneShotPool`.

## 5. Bf3GameAudio

The class is in `game_audio.{h,cpp}`. gx calls it.

* **`load`** (loader thread) calls `load_game_audio_assets`:
  1. `kit_audio_sources` over `SharedLevel::gunplay`, then `load_weapon_audio` (strict);
  2. `ImpactSoundCache::build` over `p_impact_sounds` (Bf3GameFx's `impact_sound_names`);
  3. the E7 audition patches, compiled leniently. A patch the level's mount lacks (on Metro: the M1A2) is compiled
     from a lazily mounted `EbxLibrary` over `GameAudioLoadOptions::audition_superbundles` (default
     `kAuditionSuperbundles` = `Win32/Levels/MP_001/MP_001`, which holds the patch and its waves [verified:
     `bf3dump find @all-mp m1a2_abrams`]) and gets a `WaveBank` of its own from that superbundle's mount. One that
     loads from neither is listed in `audition_failures`, and its check steps fail;
  4. one `WaveBank` over the level programs' waves, using the loader's threads.

  `load_report()` lists each kit item's form, the auditions (with their source) and the bank line. On Metro the
  M1A2 adds 14 resident waves (5.0 MiB) and 0.09-0.36 s on the loader thread (branch, unlocked, shared machine:
  indicative only). `set_load_options` before `load` changes the source; an empty `audition_superbundles` skips it
  (item 4 then fails).
* **`build`** (main thread):
  - reads `res://config/audio_tuning.cfg`;
  - creates the mixer: 96 instances, 8192 commands, the lead from the tuning;
  - installs a `TapForwarder` as the mixer's only MixTap;
  - creates the pool, clock, router and impacts;
  - creates the stream and player "Bf3GameMixer" under the parent node.
* **`step(frame, impact cues[, effect sounds])`** (every display frame):
  - measures the latency and sets the bias;
  - calls `set_now(present_clock)`;
  - sets the listener to the active camera;
  - takes the others' muzzles from fx's `MuzzleSample` (3p; +Y of the effect frame is the barrel);
  - converts the impact cues;
  - calls `GameAudioDriver::step`.

  The driver's order is own slot, first person, trigger → listeners → `set_others_newest` (present_other_weapons at
  `newest_tick`) → `on_events` → `on_others` → impacts → effect sounds → `update`.
* **`audition(patch, position)`** (E7 item 4) calls `start_audition`: a client-local one-shot facing the listener, in
  third person, at `earliest_frame()`. When a check is in a step, its `on_audition` runs in `before_start`.
* **`set_check`** points the router's observer at the check and swaps the TapForwarder's target under the
  AudioServer lock. The check is an observer and a MixTap.
* **`stats`** returns MixerStats, router, impacts, pool, clock, latency and bias. **`shutdown`** is idempotent.

## 6. E7: the in-game check and its offline rehearsal

* **The script** (`game_check_script`) has 13 burst windows and 10 auditions:
  - M16A3 bursts of 10 rounds, 2 per distance, at 5, 20, 100, 150, 280 and 320 m;
  - one 150 m `far_pinned` burst, with both distance HPFs pinned to 0 Hz (au's reference for the HPF rows);
  - auditions of m98b at {280, 325, 400} m (band 300-350), rpg7 at {30, 67.5, 120} m (band 45-90) and m1a2 at {10,
    32.5, 90, 120} m (bands 15-50 and 80-100).

  Steps take 1 s to settle, bursts start at +0.25 s and come 4 s apart. The timeout is 120 s.
* **GameAudioCheck.**
  - It records the full mix through its MixTap.
  - It taps the shooter's driver instances (au's core and noise voice taps) through the router observer, before
    their Start.
  - It opens one window per burst (first round frame to stop frame) or per audition (1 s).
  - It probes each window: a fresh `PatchInstance` of the same program and seed, evaluated at the step's distance
    (au's G5 method). It reads the distant HPF v46, the pitch v28 after the Fire, the 0-20 m close group, the
    250-300 m far groups, and each audition band's group amplitude together with the band it fades in over.
* **analyse_game_check** maps the steps of 300 m or less onto au's `CheckScript`: d5 → near, d100 → mid, d150 → far,
  pinned → far_pinned, each other step a segment of its own. It then calls au's `analyse_check` (onsets, spacing,
  levels, HPF rows). It adds these rows:
  - `windows.<step>`;
  - `gate.d320`: the core tap ≤ −100 dB, the 300-301 m Fire gate;
  - `distance.*`: within 0.25 m;
  - `probe.hpf`: 310 × clamp((d − 17) / 183) within 1 Hz;
  - `probe.pitch` in [1.21, 1.262];
  - `probe.close` and `probe.far`: cosine fades within 1e-4;
  - `audition.band`: partial inside the band or the band it fades in over, exactly 0 or 1 outside both (au G5.4);
  - `audition.audible`: the loudest 20 ms of the window ≥ −110 dBFS, full mix at unity master;
  - `skipped.<step>`: always a failure. A step the driver cannot run (an audition whose patch did not load, or that
    `Bf3GameAudio::audition` refused) is marked with `GameAudioCheck::skip_step`: `step_done` turns true so the run
    moves on, and both `skipped.<step>` and `windows.<step>` fail. A skipped item never reads as a pass;
  - the counters: mixer short_blocks / late_blocks / late_commands, router mixer_refusals, record dropped frames.

  Two rules changed on this branch after the first rehearsal:
  - the band rule now follows au's fade-in reading: the RPG-7's 45-90 group is 0.933 at 30 m inside its 5-35 fade-in;
  - the audible rule was a 1 s RMS at −90 dBFS. The M98B at 325 m measures −102.7 dB as a 1 s RMS, while its loudest
    20 ms is −93.0 dBFS: present, and as quiet as its distance law makes it.
* **The offline rehearsal** (`audio.game`, case "E7 rehearsal") drives the game's code on recorded presentation events, without Godot:
  - **The data.** The fixture loads Metro TDM's gunplay tables and the assets, exactly as `Bf3GameAudio::load` does.
    The audio tuning comes from the project's `audio_tuning.cfg`.
  - **The server side** follows sv's rules: burst_count per tick-with-shots after a tick without, and firing for a
    shot in (s − 2, s]. Records go out on the even ticks.
  - **The client side.** A snapshot arrives 2.5 ticks after its tick, with its records and the RemoteShots of its
    interval. The rehearsal publishes a real `PresentationFrame` per 60 Hz display frame: render = received − 4 ticks,
    the newest sample at or before render plus up to two newer, and `newest_tick`. host's own `present_clock`,
    `present_others` and `present_other_weapons` read it.
  - **The audio side.** `GameAudioDriver::step` runs on a 96-instance MixerCore with the game's bias, with
    `GameAudioCheck` as tap and observer. The listener holds on the shooter's axis, facing it.
  - **The auditions** go through `start_audition` with each item's own bank. The rehearsal runs every step of
    `game_check_script` (none is removed) and requires 0 skips; the M1A2 comes from MP_001 (§5).

## 7. Rows

| row | what | label |
|---|---|---|
| `audio.router.unit` | 12 data-free cases (synthetic programs): the clock and both timelines, own bursts / single fire, others' RemoteShot bursts (lost datagrams, no newest records, all lost, two bursts, single fire, death), distance and cull, the listener re-send, reloads and one-shots, the voice limit and stealing, impacts and effect sounds, pool aging, determinism (bit-identical mix and counters), hostile input, the check's bookkeeping | none |
| `audio.game` | one process, one load, three cases: "kit items" (E7 item 1 on Metro TDM: 12 kit items, 10 Driver and 2 OneShot, 12 compiled, 0 failed; the impact cache; the bank with 0 missing waves), "E7 rehearsal" (the offline E7 rehearsal), "a scene for the ear" (gate 4 offline: own M16A3 bursts and a reload, the M16A3 at 15 / 80 / 250 m, an SVD at 40 m, five impacts); `e7_rehearsal.wav` / `.txt` and `scene.wav` / `.txt` to `<build>/shots/audio.game` | none (install, COST 8) |
| `audio.game.crowd` | 63 others + the own soldier for 60 s plus 6 s of tails, impacts every 3rd frame, reloads; functional counters | none (install, COST 16) |
| `audio.game.underruns` | the same render with `OPENBF3_AU3_GATE_LATE=1`: also gates `late_blocks` = 0 (a wall-clock measure: a block that took longer than its 10.67 ms) | `timing` (main only, RUN_SERIAL) |

Every install row exits 77 without an install and checks install.unchanged (snapshot diff). `OPENBF3_AU3_SHOTS`
redirects the WAVs to one flat directory.

## 8. Measured results

Times are from the branch, unlocked, on a shared machine: **indicative only**. No timing row ran on the branch.

* **Gate 1: `audio.router.unit`.** 12 cases, all passed in 0.09-0.16 s. The coverage:
  - own shots on the Predicted grid, exact;
  - others' bursts by distance;
  - impacts and reloads;
  - the voice limit and stealing;
  - both timelines;
  - bit-identical determinism.
* **Gate 2: the E7 rehearsal.** All 124 rows passed, 0 skips. The run covered 113.4 s of game time, 13 burst windows
  and 10 audition windows. The rows:
  - **Bursts.** 10 onsets in each of the 11 sub-301 m windows. Spacing within 4-70 frames of 3600 (tolerance 96).
  - **Levels.** near − mid 45.4 dB, mid − far 13.6 dB.
  - **HPF.** `hpf.mix` 17.0 dB and `hpf.noise_tap` 18.5 dB; `hpf.noise_prediction` 0.74 dB (≤ 1.5).
  - **Fire gate.** `gate.d320` −144 dB, twice.
  - **Probes.** Every `probe.*` row exact.
  - **Auditions.**
    - m98b: 1.0 at 280 m, 0.5 at 325 m (loudest 20 ms −93.0 dBFS), 0.0 at 400 m.
    - rpg7: 0.933 at 30 m (in the fade-in), 0.5 at 67.5 m (−47.9 dBFS), 0.0 at 120 m.
    - m1a2 (from MP_001): band 15-50 reads 1.0 at 10 m, 0.5 at 32.5 m (−67.4 dBFS), 0.0 at 90 and 120 m; band
      80-100 reads 1.0 at 10 and 32.5 m, 0.5 at 90 m (−96.7 dBFS), 0.0 at 120 m.
  - **Counters.** short_blocks 0, late_commands 0, mixer_refusals 0, dropped frames 0. late_blocks read 0, not gated.

  The windowed run is gx's m2a_check case "audio" (§10).
* **Gate 3: 60 s with 64 shooters, Godot-free.** 0 short blocks, 0 late commands, 0 ring overflows, 0 starved voices,
  0 decode errors, 0 feedback drops, 0 mixer refusals, peak 94 of 96 instances. The router saw:
  - 6123 bursts, 167 culled and 6072 tails stolen;
  - 24051 event rounds and 45 state rounds (culled bursts resuming from the records);
  - 35 reloads.

  The pool peaked at 64 voices, 248 stolen and 0 failed. There were 1200 impacts and 0 unknown patches. The mix read
  −24.8 dBFS RMS, peak 0.558, with 0 non-finite samples.

  **Underruns, indicative:** late_blocks 0 (blocks over their real-time budget). The worst block took 6.6 ms of
  10.67 ms; 66 s rendered in 15.9 s wall time (4.2× real time). The gating row is `audio.game.underruns` (timing,
  main only).
* **Gate 4: in the playable scene.**
  - **Offline (`audio.game`, case "a scene for the ear").** The loudest 20 ms of each sound, at unity master:

    | sound | dBFS |
    |---|---|
    | own M16A3, first person | −18.2 |
    | M16A3 at 15 m | −31.4 |
    | M16A3 at 80 m | −66.3 |
    | M16A3 at 250 m | −93.6 |
    | SVD at 40 m | −45.8 |
    | impacts | −35.0 |

    The row checks own > 80 m and 15 m > 80 m > 250 m. It also checks 2 own bursts, 1 reload, 8 bursts in all and
    5 impacts, with 0 state rounds.
  - **In the windowed scene.** This needs gx's level-session wiring (m2a_game.h hooks), which is not on this branch:
    see §10.

## 9. Gates, one command each

From the worktree, after `tools\build.cmd dev --no-test`.

| gate | command |
|---|---|
| 1 audio.router.unit | `ctest --preset dev -R "^audio\.router\.unit$"` |
| 2 E7 rehearsal (offline) | `ctest --preset dev -R "^audio\.game$"` (case "E7 rehearsal"; WAV: `b\dev\shots\audio.game\e7_rehearsal.wav`) |
| 2 E7 in game (windowed) | gx's `godot.m2a_check` case "audio" (`OPENBF3_GODOT_WINDOWED=1`), once gx lands |
| 3 64 shooters, functional | `ctest --preset dev -R "^audio\.game\.crowd$"` |
| 3 64 shooters, underruns | `ctest --preset dev-timing -R "^audio\.game\.underruns$"` (main only, rule 7) |
| 4 heard, offline | `ctest --preset dev -R "^audio\.game$"` (case "a scene for the ear"; WAV: `b\dev\shots\audio.game\scene.wav`) |
| E7 item 1 | `ctest --preset dev -R "^audio\.game$"` (case "kit items") |
| contract | `ctest --preset dev -R "^m2a\.contract3$"` |
| asan | `tools\build.cmd asan --no-test`, then `ctest --preset asan -R "audio\.router|audio\.game|m2a\.contract3" -LE timing` (rule 7: the timing row stays on main) |

## 10. Open issues and hand-offs

* **gx.**
  - **Lifecycle.** Construct `Bf3GameAudio` in the level session. Call `load(M2aLoadInputs, Bf3GameFx::impact_sound_names())`
    on the loader thread after `Bf3GameFx::load`, then `build(parent)`. Then call `step(frame, Bf3GameFx::sound_cues()[,
    effect sounds])` every frame after `Bf3GameFx::step`.
  - **The check.** For case "audio", `set_check(&check)` before the first step. Then `begin_step` when the camera is
    in place, `audition` per audition step (a refusal skips the step through `skip_step`, so `step_done` still turns
    true and the step fails), `step_done` polling, and `analyse_game_check` with
    `stats().short_blocks / late_blocks / late_commands / router.mixer_refusals`.
  - **The shooter.** The check needs a bot firing M16A3 bursts of 10 on the camera's axis.
  - **`m2a_check`'s shooter** is `set_shooter(slot)`.
* **fx3.** The effect-sound accessor in `game_fx.h` is still missing. `Bf3GameAudio::step` already has the overload
  taking `span<const EffectSoundRequest>`. `fx::impact_sound_names` is declared but has no body on this branch: the
  install rows use a test-local `m2a_fake::impact_sound_names` with the same rule, over `ImpactTable::cell`.
* **burst_seq.** sv increments `burst_count` per tick with shots after a tick without. For automatic weapons whose
  interval spans more than one tick, that is once per shot, not once per burst. The others' seeds use
  (record burst_seq + 1 + RemoteShots ahead) & 15, which every client computes alike from the same records. But
  "a change = a new burst" does not hold, so the router never uses a burst_seq change to start a burst.
* **The M1A2 audition** (fixed after the verifier's finding). The patch is not on Metro's mount; it now loads from
  `kAuditionSuperbundles` (§5) and its 4 steps pass. Any audition that cannot run fails E7 (`skipped.<step>`).
* **The M67 explosion.** Metro's M67 def has no `explosion_sound`. The detonation sound has to come from fx3's effect
  sounds.
* **far_pinned seed.** In game, `far_pinned` is a later burst, so its seed differs from far.0's (au's G7 reuses
  far.0's seed). The rehearsal's `hpf.noise_prediction` still passes at 0.74 dB.
* **short_blocks** is never incremented by `MixerCore` (au). The underrun measure is `late_blocks`, which is a
  wall-clock measure.
* **Single-instance phase.** Separate instances carry up to one 512-frame block of phase (§2). This is not
  noticeable for single fire at 0.15 s or more. A smoothed clock would remove it.
