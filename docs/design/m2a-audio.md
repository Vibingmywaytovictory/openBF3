# M2a audio: A1 (XAS voices, wave bank, mixer) and A2 (weapon patch interpreter)

Track design for the audio part of milestone M2a ("Infantry gunplay", research/PORTING-RESEARCH.md §6), slices
**A1** and **A2** of gap audio §11.6. Branch `m2a/audio`. Status: **implemented** (A1 and A2). Every gate G0–G8 passes; §1.1 has the measured values,
the load each timing was taken under, and one command per gate. §16 lists where the implementation departs from
the interface-stage text and why; the sections below are updated to match.

Sources this design relies on, and the short names used below:
* **plan**: research/PORTING-RESEARCH.md. §3.10 audio, D15 (decision row), §2.3 threads, §2.4 cache, §6 M2a row,
  §7.7 audio oracles.
* **gap audio**: research/topics/gap-r1-audio-system.md. §1 census, §2 the M16A3, §6 mixer, §9 waves and decode cost,
  §10 Godot, §11 recommendations, §12 oracles, §14 open items.
* **IW4L**: `C:\Users\olku\iw4L\crates\resource_bf3\src\sound.rs`. This is the user's own Apache-2.0 XAS decoder, and
  A1 ports it with attribution in the file header.
* Tags: **[verified]** means checked on this tree against the retail install on 2026-10-15 (§4 says how). **[research]**
  means taken from plan or gap audio and not re-measured. **[inferred]** means a modelling choice the data does not
  state; every inferred value is a tuning key (§6.6) or an open question (§15).

Clean room: none of the forbidden sources (Rime, fun-bots, IceBloc, AssetBankPlugin, FrostyToolsuite, VU-Docs schema,
probe-ant) was opened for this design. Field names come from the partitions' own type tables, through our EBX reader.

Naming in this document: patch nodes are named by type and by the registers they read or write (`v46` is register
46 of the patch's register file), voices by their `VoiceIndex`. "Reading R" is the MultiCrossfader model of §6.3.

---

## 1. Goal and exit gates

**Goal.** BF3's own weapon sound patches play from the retail install through our mixer in Godot:
* XAS is decoded per voice from resident chunk bytes.
* The control graph of each `SoundPatchAsset` is evaluated per event (distance, fire, LFOs, seeded random) and drives
  the patch's compiled voices (gain, pitch, filters).
* The result is summed into one Godot `AudioStreamPlayback`.
* There is no HDR (A3), no reverb (A4), and AreaType is fixed at 0.

Every gate is run by one command, either a ctest or the command it wraps, and has one pass threshold. Install tests
exit 77 when no install is found. Every install test also checks `install.unchanged`: the `Install::snapshot` of
`layout.toc`, `cas.cat` and every `.sb`/`.toc` is identical before and after the run. Timing gates (G3, G6's load
time, G8) run `RUN_SERIAL`. This machine is shared with other workflows, so every timing in this document says the
load it was taken under; acceptance runs of the timing gates need a quiet machine (in this workflow the runner checks
the shared quiet lock before starting them and waits while it exists; the gates themselves neither take nor need it).

| id | gate (ctest) | threshold | how measured |
|---|---|---|---|
| G0 | `audio.contract` | every audio header compiles together; type table sets; seam hashes and FourCCs; `kStoredConnectionNone` = 0; the M16A3 check constants; tuning parser and `audio_tuning.cfg` (16 keys) | doctest, no data. **Passes now** |
| G1 | `audio.waves` | Metro TDM (`load_level MP_Subway TeamDeathMatch0`): 909 `SoundWaveAsset`, every one read without error; every segment stream of every playable variation (English range for localised waves) decoded; **decoded frames == header frames: 0 mismatches**; header frames vs `SegmentLength × rate`: 0 off by more than 1 frame; 0 errors | `bf3dump audio waves MP_Subway TeamDeathMatch0 --decode`; the test parses its summary line |
| G2 | `audio.loc` | `Win32/Loc/en` chunk toc (base + patch): 4,290 chunks; every stream in every chunk scanned and decoded; 15,890 streams, **0 mismatches**, 0 errors; 7 chunks with no stream (non-sound) are reported and are not errors | `bf3dump audio loc en --decode` |
| G3 | `audio.decode.perf` | best of 3 single-thread decodes of all G1 streams, **thread CPU time ≤ 0.2 ms per channel-second** (wall time printed beside it) | `bf3dump audio waves MP_Subway TeamDeathMatch0 --bench 3`: CPU time of the decoding thread from `GetThreadTimes` over `decode_stream` only, chunks already in memory; RUN_SERIAL |
| G4 | `audio.census` | over `@all-mp`: 834 `SoundPatchAsset` (833 under `sound/`) + 1 `SoundMasterPatchAsset`; 32,117 node instances of 46 types in patches; 32,221 of 58 types with master and mixers; 7,323 voices; 1,879 `SoundWaveAsset`; **the 164 handheld weapon patches use exactly the 32 A2 types and all compile strictly**; all 835 graphs compile with `allow_unimplemented`; 0 unknown types; **in every graph the non-zero `ConnectionIndex` values are exactly 1..N once each** (9,807 connections: `Sen0` 9,784, `Rou0` 23) | `bf3dump audio census @all-mp` |
| G5 | `audio.oracles` | plan §7.7 items 1–4, the Cone curve, the M16A3 routing and audible sets, all evaluated by the interpreter on compiled programs. Item list and tolerances in §10.2 | doctest `audio.oracles` (install) |
| G6 | `audio.bank` | the Metro TDM wave bank loads with 629 resident waves = **265.4 MiB ≤ 384 MiB** budget; 280 localised waves skipped; the mount's **161 handheld weapon patches** compile strictly and name 318 distinct waves, **all resident (0 starved)**; **`WaveBank::load` ≤ 500 ms** (warm, second load in the process) | `bf3dump audio bank MP_Subway TeamDeathMatch0 --loads 2` (ctest `audio.bank` through `tests/audio_gates.cmake`); RUN_SERIAL |
| G7 | `audio.m16.offline` | the M16A3 check script (§10.4) rendered offline through `MixerCore`: `analyse_check` rows (a)–(d) PASS (core-tap onsets 10 per burst at 3600 ± 96 frames, §10.4; full-mix levels near > mid > far by ≥ 3 dB; HPF ≥ 10 dB on the full mix and on the voice-14 tap, the tap within 1.5 dB of the analytic prediction); plus the offline rows (e)–(h): the sampler trace follows the model, a 0 Hz high-pass at 17 m is bit-identical to none, same seed bit-identical and a different seed not, 0 starved voices and 0 decode errors; WAVs written to `b/shots/audio/` | `bf3dump audio check --g7 --out-dir b/shots/audio` (ctest `audio.m16.offline` through `tests/audio_gates.cmake`) |
| G8 | `godot.audio` | windowed Godot run of `res://scenes/audio_m16.tscn` with the same check script: **rows (a)–(d) PASS on the recorded WAVs**; the recording covers ≥ 30.0 s; **`short_blocks` = `late_blocks` = `late_commands` = `ring_overflows` = `starved_voices` = 0**; **the recorded mix equals an offline replay of the same script sample for sample**; `max_block_ns` and `max_call_gap_ns` printed; install unchanged; exit 77 without a window or `OPENBF3_GODOT` | `tests/godot_audio.cmake` (the `godot_m1_load.cmake` pattern: probe run, then `-- --audio-check --audio-record=<b/shots/audio/godot_m16.wav> --quit-after=90`), RUN_SERIAL, RESOURCE_LOCK `openbf3_godot_project` |

Unit gates that need no data: `audio.xas.unit`, `audio.patch.unit`, `audio.mixer.unit`, `audio.dsp.unit`. The hostile
input gate is `audio.fuzz`, which runs under the `asan` preset (§11).

The plan's M2a row says "908 Metro waves". This tree's `load_level` scope finds 909 (§4.1), and G1 pins 909. The
difference is a scope difference, not a reader difference (open question 7).

G8 measures the mixer's compute time and ring health, not device xruns: Godot does not expose those to a
GDExtension. `late_blocks` is wall time per block, so preemption on a busy machine can trip it; the row prints the
worst block time and the worst gap between `_mix` calls so a flake is visible as one.

### 1.1 Results (implementation wave, 2026-10-16)

One command per gate; each ctest wraps exactly that command (`tests/audio_gates.cmake` for G1–G4, G6, G7;
`tests/godot_audio.cmake` for G8). Commands run from the repo root after `tools\build.cmd dev`; `bf3dump` is
`b\dev\tools\bf3dump\bf3dump.exe`. **Timings were taken on a machine shared with other workflows** (the quiet lock was
honoured before every run, but other work may still have been running): they are noisy, and the margins below are
what makes them meaningful, not their exact values.

| id | command | threshold | measured |
|---|---|---|---|
| G0 | `ctest --preset dev -R audio.contract` | headers, type table, hashes, FourCCs, 16 tuning keys | pass (dev and asan) |
| G1 | `bf3dump audio waves MP_Subway TeamDeathMatch0 --decode` | 909 waves, 0 frame / 0 length mismatches, 0 errors | 909 waves, 12,955 segments, 0 / 0 / 0; install unchanged |
| G2 | `bf3dump audio loc en --decode` | 4,290 chunks, 15,890 streams, 0 mismatches, 0 errors, 7 without streams | 4,290 / 15,890 / 0 / 0 / 7 |
| G3 | `bf3dump audio waves MP_Subway TeamDeathMatch0 --bench 3` (ctest `audio.decode.perf`, RUN_SERIAL) | best thread CPU ≤ 0.2 ms per channel-second | G3_MEASURED |
| G4 | `bf3dump audio census @all-mp` | the §1 counts | 834 (833 under `sound/`) + 1; 32,117 / 46; 32,221 / 58; 7,323 voices; 1,879 waves; 835 compiled, 0 unknown; 9,807 connections (9,784 + 23); 164 handheld strict on 32 types; 0 blob failures; 0 feedback edges |
| G5 | `b\dev\tests\test_audio_install.exe --test-suite=audio-oracles` (ctest `audio.oracles`) | §10.2 items 2–7 | G5_MEASURED |
| G6 | `bf3dump audio bank MP_Subway TeamDeathMatch0 --loads 2` (ctest `audio.bank`, RUN_SERIAL) | 629 resident, 265.4 MiB ≤ 384, 280 skipped, 161/161 strict, 318 waves, 0 starved, warm load ≤ 500 ms | G6_MEASURED |
| G7 | `bf3dump audio check --g7 --out-dir <openBF3>\b\shots\audio` (ctest `audio.m16.offline`) | rows (a)–(h) PASS | G7_MEASURED |
| G8 | `"%OPENBF3_GODOT%" --path bf3-opensource res://scenes/audio_m16.tscn -- --audio-check --audio-record=<openBF3>\b\shots\audio\godot_m16.wav --quit-after=90` (ctest `godot.audio`, RUN_SERIAL) | rows (a)–(d) PASS, ≥ 30.0 s, all mixer counters 0, replay equal | G8_MEASURED |

The ctests write their WAVs to `${PROJECT_SOURCE_DIR}/b/shots/audio` (gitignored) or to `OPENBF3_AUDIO_SHOTS` when it
is set at configure time; in a worktree set `OPENBF3_AUDIO_SHOTS=C:\Users\olku\BF3 reimplementation\openBF3\b\shots\audio` to collect
them in the main checkout's shots directory. Under the `asan` preset the bf3dump gates run with `REPORT_ONLY` (counts
and timings reported, not gated), but a non-zero exit (an ASan abort) or a missing `install.unchanged yes` still
fails the test.

---

## 2. Module ownership

| module | owns | Godot? | M2a audio files |
|---|---|---|---|
| `bf3_core` `audio/` | reading the data: XAS stream codec, `SoundWaveAsset` reader, `WaveBank` (resident chunk bytes), the node type table, the `SoundPatchAsset` compiler (control program + audio plan), the program blob and its cache entries | no | `include/bf3/core/audio/*.h`, `src/audio/*.cpp` |
| `bf3_audio` (new library) | the runtime: `PatchInstance` (the A2 interpreter), `MixerCore`, DSP, WAV I/O, analysis, tuning, the weapon seam, the shared M16A3 check (`audio_check.h`) | no | `src/bf3_audio/**` |
| `bf3_godot` | `Bf3AudioStream`/`Bf3AudioPlayback` (the `AudioStreamPlayback` subclass), the demo node and its checks, scene `audio_m16.tscn` | yes | `src/bf3_godot/src/audio_*.{h,cpp}` |
| `tools/bf3dump` | `bf3dump audio …` subcommands (G1–G4, G6 and G7 drivers, offline render) | no | `tools/bf3dump/audio_commands.{h,cpp}` (waves, loc, dispatch), `audio_patch_commands.cpp` (census, bank, check, render, wave) + an additive dispatch line |

Plan §3.10 and gap audio §11.1 put "`bf3_audio` in `bf3_core`". **Decision:** the data side (codec, readers, compiler)
goes in `bf3_core`. That is where every other reader lives, and the dedicated server and `bf3dump` need it without a
mixer. The runtime (interpreter state, mixer, DSP) is a separate Godot-free library `bf3_audio`, client only. The
dedicated server links neither the runtime nor any audio thread; it runs dummy audio (plan §3.10, gap audio §11.3).
The headless split the plan asks for still holds, because every piece is testable without Godot (open question 6).

---

## 3. Threads

| thread | does | never |
|---|---|---|
| level loader (existing loader thread; plan §2.3) | reads the level's `SoundWaveAsset`s (`read_sound_wave`, charged to a `WorkBudget`); compiles or takes from the cache every `SoundPatchAsset` the level needs (`parallel_map` over patches); reads all resident chunks in parallel (`parallel_for`) into a `WaveBank`; publishes `shared_ptr<const WaveBank>` and the programs | touch the mixer |
| game thread (Godot main) | `PatchInstance::create` (all allocation happens here) and the pre-Start setup (`WeaponSoundDriver::prepare`, test pins, taps); per tick pushes POD `MixerCommand`s (start, stop, parameters, events stamped with a mix frame, emitter/listener); `reclaim()` destroys returned instances and retired banks; `poll_feedback()` reads haptics; owns and finally destroys the `MixerCore` | block on the audio thread |
| audio thread (Godot's AudioServer thread: WASAPI, or the Dummy driver's thread when headless) | `Bf3AudioPlayback::_mix` → `MixerCore::mix`: drains commands, evaluates and renders each instance per sub-block on the core's 512-frame grid, sums buses, folds to stereo, serves the requested frames, feeds the `MixTap` and voice taps | allocate, free, lock, log, read files, wait |
| test / offline render | calls `MixerCore::mix` directly in a loop (G7, unit tests, `bf3dump audio render`, G8's in-process replay) | — |
| WAV writer | `WavRecorder::save` on the game thread after playback stopped (PathGuard checked) | write from the audio thread |

Handoff rules (§7): three SPSC rings (commands in; finished instances out; feedback out). Instances move by pointer;
a refused Start leaves the instance with the caller. Banks are retired by epoch. The audio side never frees memory:
the session releases the `MixerCore` on the game thread after stopping the player and waiting one AudioServer
`lock()`/`unlock()`, so the playback's `shared_ptr` is never the last reference (`mixer_core.h`).

---

## 4. Data sources (verified)

All counts below were taken on this tree's `bf3_core` (`load_level`, `EbxLibrary`, `Install::chunk_toc`) with a
scratch tool linked against `b\dev\src\bf3_core\bf3_core.lib`, plus `bf3dump dump` of the named partitions.
Outputs stayed in the scratch directory. A1 moves the scratch decode and census logic into `bf3_core` and `bf3dump
audio`, and the gates re-measure everything there.

### 4.1 Waves on Metro TDM [verified]

`level::load_level(install, "MP_Subway", "TeamDeathMatch0")` mounts 12,315 partitions. gap audio §9 counted 11,514
from the main + TDM bundles. This mount finds:
* **909 `SoundWaveAsset`.** Of these, 280 are localised (VO, with `Localization[]`) and 301 have a `StreamPool` (the
  280 VO waves + 21 others).
* `ChannelCount` 1: 764, 2: 122, 4: 22, 6: 1.
* Chunk bytes: the 608 non-VO waves without a pool hold 249.4 MiB; the 21 with a pool hold 16.0 MiB. VO lists all
  languages and comes to 2,506.4 MiB if every listed chunk counted, but only English is installed.
* The 629 non-VO waves hold 1,628 distinct chunks; reading them all through `LevelMount::read_chunk` on one thread
  took 79–100 ms (three passes, the CPU 90 % busy with other workflows).
* Research figures for comparison (gap audio §9): 908 waves, 606 + 21 + 281, 245.7 MiB. The differences come from the
  mount scope.
* With the English range only: 11,973 variations, 12,955 segments. Every stream is codec 4 (EA-XAS v1).
* Rates: 11,025 Hz ×6, 16,000 Hz ×1, 44,100 Hz ×206. At 48 kHz: 11,747 mono, 648 stereo, 346 quad, 1 six-channel.
* The 280 VO waves' chunks are absent from the level mount. Every one of them is found in `Win32/Loc/en` through
  `ui::read_toc_chunk(install, "Win32/Loc/en", id)` (text_db.h).
* **0 read errors. 0 decoded-vs-header frame mismatches. 0 header-vs-`SegmentLength × rate` mismatches above 1 frame.**
* Maxima: 69 chunks per wave, 416 variations per wave (all languages; 102 in English), 29 segments per variation.
* Decode speed, one thread, wall time: 0.092–0.115 ms per channel-second when the machine was lightly loaded. It rose
  to 0.35 and 0.85 while other workflows held the CPU at 100 % (the 0.85 run covered 27,993 channel-seconds); hence
  G3 gates on thread CPU time. gap audio §9 [research]: 0.117–0.120.
* The mount holds 329 `SoundPatchAsset`, of which **161 are handheld weapon patches** (`sound/weapons/handheld/**`;
  @all-mp has 164). Their `Sampler.Wave` and `WaveSwitcher.Waves[]` references (7,336) name **318 distinct waves, all
  present in the mount, none localised**, 38.1 MiB of chunks.

`SoundWaveAsset` fields as the partitions' own type table names them. Checked by dumping
`sound/weapons/shared_content/hifi_lmg/hifi_lmg_m4_wave` and `sound/vo/common/ru/autotriggers/vo_ru_mp_alert_hmg`:
* `ChannelCount`
* `Chunks[] {ChunkId, ChunkSize}`
* `Variations[] → SoundWaveVariation {ChunkIndex, FirstLoopSegmentIndex, LastLoopSegmentIndex, SeekTablesSize,
  Segments[] {SamplesOffset, SegmentLength, SeekTableOffset}}`
* `Localization[] {Language → AudioLanguage, FirstVariationIndex, VariationCount}`
* `StreamPool`, `Seekable`, `Selection`, `VoicePriority`, `PersistentVariationCount`, `SubtitleStringIds[]`

The M16A3's waves (each segment is its own XAS stream at its `SamplesOffset`; segment lengths at the wave's rate):

| wave | segments | intro / loop / tail |
|---|---|---|
| `hifi_lmg_m4_wave` | 14 | 0 / 1–12 (94.7 ms each) / 13 (0.377 s) |
| `corebassclose_lmg_08_wave` | 29 | 0 / 1–28 (94.7 ms) / none |
| `corebassdistant_lmg_04_wave` | 25 | 0 / 1–23 (94.7 ms, one 93.4) / 24 (0.310 s) |
| `noise_close_openfield_wave` | 16 | 0 / 1–14 (94.7 ms) / 15 (1.23 s) |
| `noise_distant_openfield_wave` | 13 | 0 / 1–11 (94.7 ms) / 12 (0.730 s) |
| `noise_far_openfield_wave` | 16 | 0 / 1–14 (94.7 ms) / 15 (0.790 s) |
| `lfe_wave` | 1 | loop 0–0 (94.7 ms) |
| `hifi_rattle_composite_wave` | 1 | loop 0–0 (2.59 s) |
| `openfield_reflections_far_wave`, `reflections_close_openfield` | 1 each, 3 variations | 2.8–3.7 s |

At the burst pitch of 1.16–1.262 (§4.5) a 94.7 ms segment plays for 75.0–81.5 ms: the data's loops are tuned to the
M16A3's 75 ms interval.

### 4.2 Stream format [verified on every Metro and Loc/en stream]

The format is documented in `sound_stream.h`:
* A stream is a sequence of blocks with tag bytes 0x48 (header), 0x44 (data) and 0x45 (end), each with a big-endian
  24-bit size.
* The header gives codec = `h[0] & 0xf`, channels = `(h[1] >> 2) + 1`, an 18-bit rate, and total frames
  `& 0x1fffffff`.
* EA-XAS v1 data comes in 76-byte frames per channel holding 128 samples, in 4 groups of 32. The coefficient pairs are
  {0, 0}, {240, 0}, {460, −208} and {392, −220}.
* The census found only codec 4 in retail. `Pcm16Be` (codec 2) is supported because the format defines it.

### 4.3 Loc/en [verified]

`Install::chunk_toc` of `Win32/Loc/en` (base + patch):
* 4,290 chunks, 1,159.7 MiB. 4,283 of them hold streams and 7 do not. gap audio §9 counted 4,285 (base only).
* 15,890 streams (research: 15,806). Every stream is codec 4. Up to 8 channels: one 8-channel stream at 44,100 Hz and
  one at 48,000 Hz. 1,223 streams are 16 kHz mono (radio).
* 43,090 channel-seconds. **0 errors, 0 mismatches.** 0.0991 ms per channel-second.

### 4.4 Patches and the graph [verified]

`EbxLibrary` over `@all-mp` (50,486 partitions):
* 834 `SoundPatchAsset` + 1 `SoundMasterPatchAsset`. The extra patch beyond the research count is
  `levels/mp_012/sound/sfx_poppertarget_raise`; the research dumped `sound/` only, which holds 833.
* 462 patch configurations; 1,879 `SoundWaveAsset`.
* Patches hold 32,117 node instances of 46 types. With the master patch and the 19 mixers: 32,221 of 58 types.
* Max 242 nodes per patch; max `ValueCount` 266; 7,323 voices in total.
* Every patch partition comes from a level superbundle.
* Research gave 833 / 32,113 / 7,322; the differences come from the extra level patch. Per-type counts differ
  slightly from gap audio §1.2 for the same scope reason (e.g. SimpleTransform 7,618 vs 7,639).
* The plugin FourCC census matches gap audio §1.3: `Sen0` 9,784, `Gai0` 5,649, `SnP1`/`Pau0` 5,018, `Rsp0` 4,874,
  `Sub0` 2,305, `LPB0`/`VuM0` 1,610, `Pn21` 1,413, `HI20` 723, `PI20` 400, …

The **164 handheld weapon patches** (`sound/weapons/handheld/**`) use exactly the 32 types of `sound_types.h` (`a2`).
Every field of every instance of those types was tabulated (scratch `fields.txt`), and it fixes the port/float/enum
layout in `sound_types.cpp`. Enum usage in weapons:
* Loop: `LtNone` 1,013, `LtRelease` 708, `LtForward` 22, `LtInstantRelease` 22.
* FadeType: Cosine 1,310, Linear 350.
* Logical: Or 116, And 2.
* SimpleTransform: Multiply 2,526, Add 492, Sine 9, Power 1, Subtract 1.
* Condition: Equal 8, Less 4, Greater 4.

Enumerator orders (SimpleTransformOperation, ConditionType, …) were read from the partitions' enum tables. The
compiler maps enumerators by name.

The graph structure is summarised in `sound_program.h`:
* `SoundGraphData {Nodes, ValueCount, InputEvents, InputParameters, Public*, Info}`
* `SoundGraphInfo {Voices[] {ProcessingStage, Plugins[] {Id, OutputChannelCount, ConnectionIndex,
  ConstructParamsIndex/Count, EnableAttributeReadMask}}, Connections[] {ConnectionType, VoiceIndex, Bus, Parameters},
  ConstructParams[], LinkedPluginAttributes[] {AttributeIndex, Node, Plugin, Port, AllowExtremeValues}}`
* Ports are `AudioGraphNodePort {UnconnectedValue, ValueIndex, IsConnected}`.
* **Linked-attribute registers can exceed `ValueCount`.** The M16A3 uses 131–134 with `ValueCount` 130, so the
  register file is sized max(ValueCount, highest index + 1).
* **`ConnectionIndex` is 1-based; 0 means none.** In all 835 graphs the non-zero values are exactly 1..N once each,
  N = `Connections.size()`: 9,807 connections, named by `Sen0` (9,784) and `Rou0` (23, in 6 non-weapon graphs:
  `sound/master`, three `mp_music` patches, `destruction_bigtower`, `vehicle_paradrop_start_xp5`). No `Sen0` carries
  0. Read 1-based, the M16A3's LFE voice 4 sends to the LFE bus and voice 14 to voices 1 and 5; read 0-based, index N
  is out of range. The compiler stores index − 1 and rejects a value above N, a duplicate or an unnamed connection;
  G4 checks the rule on every graph.

The voice graph has three kinds of voice:
* Sampler voices (stage 0: `SnP1`, `Rsp0`, `Pau0`, `Gai0`, `Sen0` → voice) send into submix voices (`Sub0` …).
* Submix voices (stages 1–2) feed output voices (stage 3, or stage 1 for a direct output).
* Output voices: `Sub0`, `LPB0`, `VuM0`, `Pn21`, `Sen0` → bus. `Pn21` construct params are 45, 135 and 2.0. Each
  output node (Cone/Flat/Direct) names its voice through its `LowPassPlugin`, `PanPlugin`, `MainSendPlugin`,
  `ReverbSendPlugin` and `VuPlugin` refs.

Master buses come from the `BusNodeData.BusName` values of `sound/master` [verified]:

| bus | hash | channels |
|---|---|---|
| GameMixMain | b735bfcd | 4 |
| Reverb | d72517d3 | 2 |
| GameMixAlt | 9384b910 | 4 |
| DirectOut | 84f7080e | 6 |
| Dialog | 14706325 | 1 |
| LFE | fcef9739 | 1 |
| Music | 8df9c7ba | 4 |
| Movies | dda9bec8 | 6 |
| LoadingMusic | 49bdaefa | 2 |
| NarrVoice | 84003c1f | 4 |

`AudioSystemAsset` (`sound/audiosystem`): `SampleRate` 48,000, `DopplerFactor` 0.5, `SoundSpeed` 331.5,
`DefaultLanguage` English.

### 4.5 The M16A3 patch [verified, `sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02`]

* `WeaponDef::sound` of the M16A3 names this partition. For the RPG-7 it names
  `sound/weapons/handheld/rpg-7/weapon_at_rpg7`.
* `Loudness` 110, `Radius` 0.8, `IsLooping` true. 107 nodes, 30 voices, 152 plugins, 38 connections, `ValueCount`
  130.
* **Events:** `Fire` (start, v96), `#1920007028` (stop = `DefaultStopEvent`, v99), `Reload`, `#2484178491`,
  `#1968287961` (the Sampler `Step`, v111) and `#3520090073`.
* **Parameters:** `IsFirstPerson` (v13, v58, v65, v108), `#45329532` (the WaveSwitcher index for long/short reload),
  `#3650803780`, `#1114728387`, `#1365968171` and `#4198762218` (`EnableStep`, v110).
* Name hashes are djb2-xor (`ebx_name_hash`): Fire 2088671869, IsFirstPerson 824639024, Reload 3294210644,
  AreaType 3772263626. `audio.contract` pins these.

**Control.** Physics writes the distance to v22. The gate crossfader (300–301 m, linear, `LockControlValue`) turns
Fire (v96) into the gated Fire v97 and the stop (v99) into v98.

| crossfader (Start/Stop) | groups (band, fade, amplitude → started samplers) |
|---|---|
| gate (v96/v99) | 300–301 linear → v97/v98 |
| core bass (v97/v100) | 5–35 cosine, amp v69 → corebassclose; 250–300 cosine, amp v70 → corebassdistant |
| noise (v97/v100) | 1–5 linear, amp v79 → noise_close; 20–50 cosine, amp v80 → noise_distant; 250–300 cosine, amp v87 → noise_far |
| hifi (v97/v100) | 0–20 cosine, amp v68 → hifi_lmg |
| reflections (Start v100) | 3–15 cosine, amp v88 → close reflections; 250–300 cosine, amp v89 → far reflections |
| handling | 1–15 (Start v93), 1–17 (Start v100: shell casings), 1–19 (Start v95: reload), single groups |

* **Fire mode.** v118 = `#1365968171` + EnableStep; v119 = SimpleValueSelector(v118) over [0, 1.9, 1.9, 1.9, 1.9];
  DeMux(v97, v119) routes the gated Fire to v101 when v119 is 0; v120 = ParameterFilterLinear(v119, attack 10,000/s,
  release 0.3/s); DeMux(v98, v120) routes the stop to v102 when v120 ≥ 1; v100 = Or(v101, v102) is the release
  register. So **EnableStep 0 = single shot** (Fire triggers and releases in one evaluation), **1 = automatic** (the
  stop releases).
* **Pitch:** NumberGenerator U[1.21, 1.262] → v28, triggered by v97, so **drawn once per burst**. Lfo (0.96–1.0,
  4 Hz) → v29 with amplitude v75 = the Adsr on v97/v98 (A 0.7 s, S 1). Pitch v30 = v28 × v29 drives every sampler except
  the reload, handling and shell-casing ones and the rattle.
* **HPFs.** `ScaleClamp` v22 17..200 m → 0..310 Hz = v46 → `HighPassIir2` on voice 14 plugin 2 (the noise submix).
  v49 = ScaleClamp(v22 30..100 → 0..80) + ScaleClamp(v22 0.5..30 → v57..150), v57 = ScaleClamp(IsFirstPerson 0..1 →
  80..0) → `HighPassIir2` on voice 6 plugin 1 (the core submix). Third person: v49 = 80 Hz up to 0.5 m, 119.2 Hz at
  17 m, 230 Hz from 100 m; v46 = 225.3 Hz at 150 m.
* **Delays.** hifi_lmg: U[22, 26] ms per burst. noise_close/distant/far: U[0, 45] ms per burst in third person, 0 in
  first person (ValueSelector on IsFirstPerson). The corebass samplers have no Delay port.
* **Samplers** (voices; stepped = Step v111 connected):

  | sampler | voices → submix | mode | trigger |
  |---|---|---|---|
  | corebassclose | 28–29 → 6 | LtRelease, stepped | core bass group 1 |
  | corebassdistant | 26–27 → 6 | LtRelease, stepped | core bass group 2 |
  | hifi_lmg | 18–19 → 6 | LtRelease, stepped, delay | hifi |
  | noise_close | 15–16 → 14 | LtRelease, stepped, delay | noise group 1 |
  | noise_distant | 22–23 → 14 | LtRelease, **unstepped**, delay | noise group 2 |
  | noise_far | 20–21 → 14 | LtRelease, **unstepped**, delay | noise group 3 |
  | lfe | 24–25 → 4 → LFE bus | LtRelease, stepped | first person only (v78) |
  | rattle | 9–10 → 6 | LtForward | first person only (v78) |
  | reflections close / far | 7 / 8 → 2 and 5 | LtNone | reflections groups (at the stop) |
  | reload, handling, shell casings | 17, 13, 12 → 11 | LtNone | handling crossfaders |

* **Routing** (1-based connections): voice 6 (`Sub0 HI20 PI20`) → voice 3 (`Gai0` v59 = 0.8 × IsFirstPerson) →
  voice 1, and → voice 0. Voice 14 (`Sub0 PI20 HI20`) → voice 1, and → voice 5 (`Gai0` v103 = ScaleClamp(v22 1..3 →
  0..0.5): 0 below 1 m, 0.5 from 3 m) → voice 0. Voice 4 → LFE bus. Voices 7–8 → voice 2 and voice 5. Voices 12, 13, 17
  → voice 11.
* **Outputs:** voice 0 = Cone "Cone" (Gain 0, `MinDistance` 10, `HFDampingDistance` 0.8, `ReverbGain` −17); voices
  1 and 2 = Flat "Flat" (Gain 0 and −6, same curve); voice 11 = Cone "Handling" (Gain −29, its own 2-point curve);
  voice 4 = Direct "LFE" (+3 dB, flat 0 dB curve, `EnableHdr` false). Cone curve points (X, Y, K): (0, 0, −0.976),
  (19.766, −17.917, −0.768), (54.614, −34.566, −0.264), (91.156, −41.149, −0.156), (120.438, −46.957, −0.253),
  (179.002, −67.091, −0.389).

**What plays in third person, under reading R** (§6.3; G5 pins these sets):
* **0.5 m:** corebassclose, hifi_lmg and noise_close, all stepped; at the stop: close reflections and shell casings.
* **60 m and 150 m:** corebassdistant (stepped, voice 6) and noise_far (unstepped loop, voice 14); at the stop: far
  reflections (voices 2 and 5, no HPF). Voice 3 is at gain 0 (third person); voice 5 (gain 0.5) carries voice 14's
  already high-passed output to the Cone voice. Every path that is audible between the first round and the stop
  passes one of the two distance HPFs.
* **The LFE path is silent in third person**: its only sampler is triggered by the first-person case of the
  Switcher on IsFirstPerson.

### 4.6 Other oracle patches [verified]

* **M98B** `weapon_sniper_m98_b_02`: 0–20; (5–35, 300–350); (2–10, 30–60, 300–350); (8–40, 300–350); 1–10; 1–10 linear;
  1–15; gate 300–351 linear (amplitude v89); 1–10 linear.
* **RPG-7** `weapon_at_rpg7`: 5–40; (5–20, 250–300); (5–35, 45–90, 250–300); (15–45, 250–300); 1–15; 1–19 linear; gate
  300–301.
* **M1A2** `sound/vehicles/land/m1a2_abrams/m1a2_abrams`: 90–120 (unlocked, start/stop), 30–40, 40–60 ×2, 15–50
  (unlocked), 80–100 ×2. The control register is v81, which is the output of the M1A2's own Physics node (port
  `Distance`, as v22 on the M16A3) [verified in the implementation wave; the interface-stage text said "not distance",
  which was wrong]. The M1A2 uses non-weapon types, so it compiles only
  with `allow_unimplemented`.
* Connections are 1-based in each (M98B 1..40, RPG-7 1..26, M1A2 1..40, master 1..38).

---

## 5. A1: codec, waves, bank

### 5.1 XAS codec (`sound_stream.h`)

The codec is a port of the IW4L `sound.rs` decoder (Apache-2.0, attribution in the file header). Its API:
* `parse_stream_header`
* `scan_stream` (walk blocks, optionally decode, report frames)
* `decode_stream` (whole stream to int16)
* `decode_xas_frame`
* the incremental `StreamDecoder`: `open`, `read(float*, n)`, `skip_to` (frame-exact, by decoding from the block
  start), `finished`

`StreamDecoder` holds one decoded 128-frame × channels buffer (2 KiB for 8 channels) and no heap. That makes it the
per-voice decoder of the mixer (gap audio §9: decode per voice, keep XAS resident). There is no PCM cache.

### 5.2 Wave reader (`sound_wave.h`)

`read_sound_wave(resolver, instance, budget)` returns `SoundWaveInfo` with no pointers into the partition.
`playable_variations(info, "English")` gives the variation range a listener plays.

### 5.3 Wave bank (`wave_bank.h`)

* `WaveBank::load(install, mount, waves, options)` deduplicates chunks by GUID, checks the total against
  `resident_budget_bytes` (384 MiB) **before reading**, then reads every resident chunk in parallel. Localised waves
  read through `Win32/Loc/<lang>`.
* It checks every segment `SamplesOffset` lies inside its chunk and every stream header parses.
* It publishes an immutable bank. VO is excluded by default (A7 streams it).
* Metro TDM: 629 waves, 1,628 chunks, 265.4 MiB [verified sizes; the load itself is gate G6].
* **Deviation from gap audio §9:** the 21 non-VO waves with a `StreamPool` (16.0 MiB on Metro) are kept resident
  instead of streamed from disk on demand. It fits the budget, saves a streaming path in A1, and A7 (which streams VO
  through the pools) can move them.
* Chunk bytes are re-read at every level load and never cached (plan §2.4: no decoded audio, no raw data in the cache).
  **Load-time budget:** the serial read of all 1,628 Metro chunks took 79–100 ms on a busy machine (§4.1); G6 gates
  `WaveBank::load` at 500 ms. That is 10 % of M1 E5 (warm Metro load ≤ 5 s); when the integration wave adds the bank
  to `Bf3LevelSession`, it reruns `godot.m1_load` and reports E4/E5 with the bank included.

---

## 6. A2: compiler and interpreter

### 6.1 Node type table (`sound_types.h`, implemented)

The table has 58 node types plus 8 child records. Each entry gives the EBX name, a role (Patch / MasterOrMixer /
Child), the `a2` flag, the `audio_rate` flag, and the port, float, option and children layout. The layout for the 32
A2 types and their children comes from the field census (§4.4). The other types list no fields until their wave.

### 6.2 Compiler (`sound_program.h`)

`compile_sound_patch(db, partition, options, budget, stats)` produces a `SoundProgram`:
* **Control program.** The register count, the game inputs (parameters and events bound by NameHash), the nodes in
  topological order, and pooled ports, floats, children, wave references and plugin references. Nodes that A2 does not
  implement compile to no-ops only with `allow_unimplemented`.
* **Audio plan.** Voices with stage and plugin list, render order by ascending stage, construct params, connections
  (voice → voice, or voice → bus by `BusName`; `ConnectionIndex` 1-based, stored 0-based, §4.4), linked plugin
  attributes, and output nodes with their curves, damping and the plugins they drive.
* **Topological sort.** Iterative Kahn over register writers and readers, O(nodes + reads). A cycle is broken at the
  closing edge, which then reads the previous step's value; these are counted as `feedback_edges` [inferred: none
  expected in weapons, and G4 checks it].
* **Hostile input.** The structural caps in `sound_program.h` apply, and every array element and reference is charged
  to the `WorkBudget` before it is read. Register indices are bounded by `kMaxProgramValues`. Every `ConnectionIndex`
  (1..N, each once), `VoiceIndex`, plugin and construct-param range is checked.
* **Blob.** "OBF3SNDP": `decode_sound_program` re-validates everything, so a cached program is as safe as a fresh
  compile.

### 6.3 Evaluation model (`patch_instance.h`)

* **Registers.** One float array per instance. Unconnected ports read their constant.
* **Events.** These are one-step pulses: the register reads 1 for exactly one evaluation, then 0 [inferred; gap audio
  §1.2: "event pulses"].
* **Control rate.** Each block of the mixer core's 512-frame grid is split at the frame offsets of the events posted
  for it. Pending events pulse at each split, the program runs once, and the following frames render. Time-based
  nodes (Adsr, Lfo, Timer, ParameterFilterLinear) advance by the sub-block length. An event therefore lands on its
  exact frame, and G7 measures 75 ms onsets to 1 ms. Cost is bounded by (events + 1) × program size per block, with
  at most 32 events per block (more are deferred and counted).
* **Randomness.** A counter-based generator (SplitMix64 of instance seed, node index and draw count) serves
  NumberGenerator, Lfo `StartAtRandomValue`, WaveSwitcher `IsRandom`/`RandomStartIndex`, Sampler variation choice and
  `ShuffleSegments`. Renders are deterministic per seed on any thread (G7, G8).
* **MultiCrossfader**, reading R [inferred, open question 1]:
  * Groups are ordered.
  * The first group is at full amplitude below its band.
  * Band i crossfades group i out and group i+1 in (Cosine: raised cosine; Linear: linear).
  * The last group fades out over its band, and a single-group crossfader is a fade-out over its band.
  * `FadeAmplitude` scales the group.
  * Start/Stop events are passed only to groups whose amplitude is above 0 when the event arrives. The M16A3 gate
    300–301 m therefore blocks `Fire` beyond 301 m.
  * `LockControlValue` latches the control at Start.
  * Evidence for this reading: three weapons carry the same "gate" crossfader at 300–301 / 300–351 m. It makes sense
    only as an audibility cut-off, and the handling layers 1–15/17/19 m and the 0–20 m hifi layer (1 at 0 m, 0 at
    20 m) only make sense as fade-outs.
  * Under reading R the far noise layer **fades in over 20–50 m** (from noise_distant) and **every layer fades out
    over 250–300 m**. Plan §7.7 item 1 says "far layer fades in over 250–300 m"; open question 1 asks the user to
    confirm, and the plan wording is updated once answered. G5 pins the reading R values (band edges, the 301 m gate
    and the audible sets of §4.5), so a change of reading is a visible gate change, not a silent one.
* **DeMux.** Output index = floor(Value), clamped to the last output (`WrapValue` false) or modulo the outputs (true)
  [inferred]. On the M16A3 this makes EnableStep the fire mode (§4.5).
* **Sampler** [inferred, open question 4; the full rules are in `patch_instance.h`]:
  * A variation's segments split into intro [0, FirstLoop), loop [FirstLoop, LastLoop] and tail (LastLoop, end).
  * `Trigger` starts segment 0 on a free slot, `Delay` seconds after the event frame; Delay is latched at Trigger.
  * **Stepped** (Step connected and EnableStep ≥ 0.5): each `Step` starts the next loop segment (in order, or
    shuffled) on the other slot at the Step frame plus the latched delay. A slot whose segment ends goes idle, so the
    layer's rhythm is exactly the Step rhythm.
  * **Unstepped** (Step not connected, or EnableStep < 0.5): `LtRelease` plays the loop segments back to back on the
    same slot until Release; `LtForward` loops the same way, and on Release finishes its pass (the segments up to
    LastLoop) before the tail; `LtNone` plays the segments once, and a Release ends it after the current segment. On the
    M16A3, noise_distant and noise_far are unstepped: their 94.7 ms segments, at the burst pitch, follow each other
    every 75.0–81.5 ms on their own clock, not the round clock.
  * `Release` (LtRelease): the playing segment finishes, then the tail plays (on an idle slot it starts at the
    Release frame); with no tail the sampler ends with its segment. `LtInstantRelease` fades out over
    `release_fade_ms`.
  * Pitch = `BasePitch × Pitch × MasterPitch` → `Rsp0` ratio (wave rate / 48,000 × pitch), read per sub-block.
* **WaveSwitcher.** `Index` selects from `Waves[]` and `Advance` steps it.
* **Physics.** `Distance`, `Azimuth`, `ElevationAngle`, `Velocity` and `RelativeVelocity` come from emitter and
  listener (Frostbite axes).
* **MixerReceive** writes 0 for AreaType (plan M2a: AreaType stubbed to 0). `set_mixer_value` is the A4 seam.
* **Outputs (Cone/Flat/Direct).** Attenuation = curve(distance) + `Gain`. The curve is a cubic Hermite through the
  points with slopes K, clamped at the ends [inferred]. Distances below `MinDistance` evaluate at `MinDistance`
  [inferred, tunable `min_distance_clamp`, open question 2]. Cone `InnerAngle`/`OuterAngle`/`OutsideGain` are applied
  on the emitter's forward axis. HF damping drives `LPB0`.
* **CameraShake/PadRumble** write registers that are exported through `PatchHaptics` → `MixerFeedback`; the camera
  consumes them in M2a integration.
* **Analysis hooks** (tests and G8 only, set before Start): a `VoiceTap` on up to 16 voices (mono sum of their chain
  outputs, before their sends) and a caller-owned `SamplerTrace` of segment starts.

### 6.4 Audio plan execution

Voices render in ascending `ProcessingStage`:
* `SnP1` (StreamDecoder + slot state) → `Rsp0` (cubic resampler; rate = wave rate / 48,000 × pitch) → `Pau0`
  (pause/resume) → biquads (`HI20` high-pass, `LI20` low-pass, `BI20` band-pass, `PI20` peaking: RBJ cookbook,
  coefficients ramped per sub-block) → `Gai0` (gain ramp).
* `HighPassIir2`/`LowPassIir2` have no Q port; they use `iir2_q` (0.7071, Butterworth) [inferred]. A high-pass at
  0 Hz is the identity and passes samples through bit-exactly (G7 row f).
* `Sen0` sends to a submix voice's `Sub0` input or to a bus.
* Output voices apply `LPB0` (one-pole HF damping), `VuM0` (meter only), and `Pn21`, which folds the 4.0 pan to
  stereo equal-power. The pan uses the construct params; 4.0/5.1 output is A3.
* Buses fold to stereo: GameMixMain/Alt at 0 dB; LFE at `lfe_fold_db`; the Reverb send is dropped until A4.
* **Level.** There is no HDR window (A3). Output gain = `Loudness − window_top_db − window_headroom_db`, a fixed
  window [inferred], so a 110 dB M16A3 shot sits at −11 dBFS before its own attenuation.

### 6.5 Mixer core (`mixer_core.h`) and Godot (`audio_stream.h`)

* The core renders in its own 512-frame grid from frame 0 and serves `mix(float*, n)` for any n through a one-block
  output FIFO. The output therefore depends only on the commands and their frames, not on the caller's call sizes
  (Godot's call size is not assumed), and an offline replay of the same commands is sample-identical (G8).
* `mix()` sets FTZ/DAZ for its duration and restores MXCSR, so denormal handling does not depend on the thread.
* `Bf3AudioPlayback::_mix` writes straight into Godot's `AudioFrame` buffer. The layouts are identical, so there is no
  copy. It always returns the frame count: returning fewer makes Godot fade the stream out (gap audio §10).
* `project.godot` gets an additive `[audio] driver/mix_rate=48000` in the A1 wave, so the server runs at the data's
  rate and no resampling happens. WASAPI is opened with `AUDCLNT_STREAMFLAGS_RATEADJUST`, which follows the project
  rate (gap audio §10) [research]. No other worktree adds an `[audio]` section today.
* One `AudioStreamPlayer` on the Master bus plays the stream for the whole session. Godot buses are used only for
  device output and volume (gap audio §11.2).
* **Scheduling.** A new burst is stamped at `MixerCore::earliest_frame()` = `clock()` + `schedule_lead_blocks` × 512
  (default 2 blocks = 21.3 ms) unless the game has an earlier prediction; every later round at first frame + round ×
  interval. A command whose frame was already mixed lands at offset 0 and counts `late_commands`; G8 requires 0.
* **Ownership.** A refused `push` of a Start leaves the instance with the caller. The session releases the
  `MixerCore` on the game thread (§3).

### 6.6 Tuning (`audio_config.h`, `bf3-opensource/config/audio_tuning.cfg`)

There are 16 keys in 5 sections, each with a range and a source note. All are [inferred] or fixed-window stand-ins,
and none is hard-coded in the evaluator (plan D14). The parser refuses unknown keys, out-of-range values and malformed
lines, reporting the line number. `audio.contract` checks that the project file parses and names every key. The
review added `iir2_q` (filters) and `schedule_lead_blocks` (mixer).

### 6.7 Weapon seam (`weapon_sound.h`)

* `bind_weapon_sound(program, shot_interval, rate)` finds the start, stop, step and EnableStep inputs.
* `WeaponSoundDriver(binding, fire mode, handle)`:
  * `prepare(instance, first_person)` before Start: IsFirstPerson, and EnableStep from the fire mode (Single 0,
    Burst/Automatic 1). EnableStep is never toggled during a burst (§4.5: dropping it would stop the stop event from
    releasing the loops after 3 s).
  * `trigger_down(mixer, first_frame)` posts Fire at round 0.
  * `round_fired(mixer, k)` posts Step at first + round(k × interval × rate) (Single: Fire).
  * `trigger_up(mixer, frame)` posts `DefaultStopEvent`.
* The M2a gunplay integration calls only this. It converts the client's predicted shot time to a mix frame and binds
  the patch from `WeaponDef::sound`. The shot interval is `gameplay::shot_interval(FireFunction)`: 75 ms = 3,600
  frames for the M16A3.

---

## 7. Interfaces and handoff

### 7.1 Headers (all compile now; G0)

| header | key API |
|---|---|
| `bf3/core/audio/sound_stream.h` | `SoundCodec`, `SoundStreamHeader`, `parse_stream_header`, `scan_stream`, `decode_stream`, `decode_xas_frame`, `StreamDecoder` |
| `bf3/core/audio/sound_wave.h` | `SoundWaveInfo` (+ chunk, variation, segment, localisation), `read_sound_wave`, `playable_variations` |
| `bf3/core/audio/wave_bank.h` | `WaveBank::load/find/chunk/resident/stats`, `WaveBankOptions`, `WaveId` |
| `bf3/core/audio/sound_types.h` | `SoundNodeType`, `SoundNodeTypeInfo`, `sound_node_types/type_info/type`, enumerator enums |
| `bf3/core/audio/sound_program.h` | `SoundProgram` (control program + audio plan), `compile_sound_patch`, `encode/decode_sound_program`, `compile_sound_patch_cached`, `fourcc`, `kStoredConnectionNone`, `MasterBus`, `AudioCurve` |
| `bf3/audio/audio_config.h` | `AudioTuning`, `audio_tuning_keys`, `parse_audio_tuning` |
| `bf3/audio/dsp.h` | `BiquadCoefficients::design`, `Biquad`, `OnePoleLowPass`, `apply_gain_ramp`, `equal_power_pan` |
| `bf3/audio/patch_instance.h` | `PatchInstance::create/set_parameter/post_event/set_mixer_value/set_geometry/process_block/evaluate`, inspection (`value`, `plugin_attribute`, `crossfader_group_amplitude`, `sampler_slots`, `output_gain_db`, `haptics`), test hooks `override_plugin_attribute`, `set_voice_tap` (`VoiceTap`), `set_sampler_trace` (`SamplerTrace`) |
| `bf3/audio/mixer_core.h` | `MixerCore` (`push`, `next_handle`, `clock`, `earliest_frame`, `reclaim`, `poll_feedback`, `mix`, `set_tap`), `MixerCommand`, `MixerStats`, `MixerFeedback`, `MixTap` |
| `bf3/audio/wav_io.h` | `write_wav` (PathGuard), `read_wav`, `WavRecorder` |
| `bf3/audio/audio_analysis.h` | `detect_onsets`, `rms_db`, `peak_db`, `band_energy_db`, `predicted_highpass_band_db`, `onset_spacing` (`BurstWindow`) |
| `bf3/audio/audio_check.h` | the M16A3 check: `m16_check_script`, `schedule_check`, `CheckDriver`, `TapRecorder`, `analyse_check`, `CheckReport` |
| `bf3/audio/weapon_sound.h` | `WeaponSoundBinding`, `bind_weapon_sound`, `WeaponFireMode`, `WeaponSoundDriver`, seam hashes |
| `bf3_godot/src/audio_stream.h` | `Bf3AudioStream`, `Bf3AudioPlayback` (registered; `_mix` calls `MixerCore::mix`) |
| `bf3_godot/src/audio_demo.h` | `Bf3AudioDemo` (scene `audio_m16.tscn`; `--audio-check`, `--audio-record`, `--quit-after`) |

### 7.2 Seams for later M2a integration

* **Gunplay → audio.** `WeaponSoundDriver` (§6.7). Remote soldiers get one `PatchInstance` each, with `IsFirstPerson`
  0 and an emitter at the muzzle. The first-person viewmodel (the M1 viewmodel demo and the M2a 1p integration) uses
  the same driver with `IsFirstPerson` 1.
* **Seeds.** `splitmix64(entity id << 32 | the weapon's burst sequence number)`, both replicated for hit
  registration, so every client and a replay render the same sound for the same burst.
* **Effect sounds (VFX3).** The FX instancer resolves `SoundEffectEntityData.Sound` to a program (same compile and
  cache), creates a `PatchInstance` seeded from the effect instance id, sets its emitter, pushes Start and posts the
  program's start event at the effect's frame; the instance finishes itself and comes back through `reclaim()`. No
  driver is needed for one-shots.
* **Session.** `Bf3LevelSession` (M1 T6) owns one `MixerCore`, one `Bf3AudioStream` and one `AudioStreamPlayer`. The
  level loader adds a wave-bank + program step, on the loader thread, inside the existing load phases.
* **Camera.** `MixerCore::poll_feedback` → camera shake.
* **A3 (M2b).** The HDR window, scopes and culling replace the fixed window in the output gain step and add voice
  priority. `MixerStats` gains the profiler counters.
* **A4 (M3).** `set_mixer_value(kHashAreaType, …)` from the area evaluator, plus the Reverb bus.

### 7.3 Underrun accounting (G8)

* `short_blocks`: `mix` produced fewer frames than asked. This should be impossible by construction, and the gate
  pins it.
* `late_blocks`: computing a grid block took longer than its real-time length of 10.67 ms (wall time).
* `late_commands`: a command arrived after its frame was mixed.
* `ring_overflows`: a command push was refused.
* `starved_voices`: a slot whose wave was not resident.
* Reported beside them: `max_block_ns` and `max_call_gap_ns` (the longest wall gap between two `_mix` calls).

These measure the mixer's compute and ring health; Godot does not expose device xruns to a GDExtension.

---

## 8. Caching

| item | cached? | kind / key | versions |
|---|---|---|---|
| compiled `SoundProgram` | yes, the `WeaponDef` two-entry scheme (`weapon_blob.h`) | `sound-patch-deps`: SHA1 of the patch partition + params `patch=<name>` → the sorted list of partitions read. `sound-patch`: the SHA1s of those partitions (zero for an absent one) + SHA-256 of the list → the "OBF3SNDP" blob | `kSoundProgramFormatVersion` 1, `kSoundProgramConverterVersion` 1, engine id |
| `SoundWaveInfo` | no (909 small partitions; reading them is part of the EBX pass) | — | — |
| chunk bytes, decoded PCM | **never** (plan §2.4) | — | — |
| WAV recordings | not a cache: `b/shots/audio/` (gitignored) or scratch | — | — |

The program cache sits under the process cache root (`user://bf3cache` in Godot, `--cache` in tools). A cold load
compiles and puts; a warm one gets. A patched partition misses on the `sound-patch` key. Gate: `audio.cache` (install)
runs cold → warm → patched (an in-memory digest override), the same three steps as the weapon cache test.

---

## 9. Recording and analysis

* **Offline.** `bf3dump audio render <patch> --distance <m> --rounds <n> --seed <s> --out <wav>` and the G7 test render
  through `MixerCore` with a `WavRecorder` tap.
* **In Godot.** `--audio-record=<path>` attaches the same `WavRecorder`, and the check driver's core and noise taps
  are saved beside it (`<path>` with `_core` / `_noise` before the extension). The WAVs are the mixer's output, not a
  loopback capture, so they work under the Dummy driver too.
* Every path goes through `PathGuard`: writes inside the install or outside the allowed roots fail before opening.
* Recordings go to `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\audio\` (gitignored `/b/`) and are never
  committed.
* **Analysis** (`audio_analysis.h`):
  * **Onsets at single-frame resolution:** a running 2 ms (96-frame) energy envelope per frame (no hop), a floor =
    the **mean** over the previous 20 ms (not the minimum: §16), an onset where the envelope first rises 12 dB above its floor (and above
    −90 dBFS), refined back to the first frame within the window that reaches −20 dB of the local peak and 4 × the floor's RMS (`bed_sigma`), then a 30 ms
    refractory period. A hopped envelope would quantise 3,600-frame gaps to 3,552/3,648, exactly the tolerance.
  * **Burst segmentation** comes from the script (`BurstWindow` = [first round − 10 ms, stop)), never from the
    onsets, so the gaps between bursts and the tails after a stop are not counted.
  * RMS and peak in dBFS.
  * **Band energy:** Hann-windowed 8,192-point FFT frames, 50 % overlap, bins whose centre lies in the band (5.9 Hz
    bins). The HPF rules use **20–100 Hz** everywhere.
  * `predicted_highpass_band_db` weights each bin of an unfiltered render by the analytic |H(f)|² of the RBJ
    high-pass, to predict the filtered render's band energy.

---

## 10. Test plan

### 10.1 No data (doctest)

| test | content |
|---|---|
| `audio.contract` (G0, **exists**) | headers compile together; type table counts, unique names, name round trip; enumerator mapping; seam hashes = `ebx_name_hash`; FourCCs; `kStoredConnectionNone`; the M16A3 check constants; tuning parser and the project cfg (16 keys) |
| `audio.xas.unit` | hand-built streams. Frames with each coefficient pair against a reference computed in the test from the format definition; header fields; multi-block walk; `skip_to` equals decode-and-drop. Hostile: truncated block, size 0, size past end, channels 0 / > 8, rate out of range, unknown codec, frames > cap, data past header frames, missing end block, budget exhaustion |
| `audio.patch.unit` | synthetic `SoundProgram`s built in code. Per A2 type: semantics at edge values (ScaleClamp clamp, SimpleTransform every op incl. domain errors → 0, Condition/Logical truth tables, DeMux floor/clamp/wrap, Switcher/ValueSelector default, Timer modes, Adsr phases, Lfo range/period, NumberGenerator range/determinism, MultiCrossfader reading R incl. gating and lock, EventGate cool-down, ParameterFilterLinear slew). Sampler: stepped steps on the event frame on alternating slots; unstepped LtRelease plays loop segments back to back; Release finishes the segment then plays the tail (and ends without a tail); latched Delay shifts Trigger and Steps; **Trigger and Release in the same evaluation** (single shot); the M16A3 fire-mode network rebuilt in a synthetic program: EnableStep 0 releases on every Fire, 1 releases only on the stop, and the stop still releases 2.9 s after EnableStep falls but not 3.1 s after. Topological order; cycle → feedback edge; event pulse lasts one evaluation; sub-block split at event frames; `encode/decode` round trip; every corrupted byte of a blob is refused or decodes to a valid program; `ConnectionIndex` 0-based misreads (N + 1, a duplicate, a gap) are refused |
| `audio.dsp.unit` | biquad magnitude at fc, fc/2 and 2fc within 0.1 dB of analytic for each shape; a 0 Hz high-pass is bit-exact identity; coefficient ramp continuity; one-pole cutoff; resampler passes a sine at ratio 1 bit-exact and at 0.5/2 within −60 dB THD; pan law at 0 / ±90°. Analysis: a synthetic 75 ms click train over a sustained noise bed 20 dB down gives exactly the clicks, each within 2 frames; `band_energy_db` of sines in and out of the band; `predicted_highpass_band_db` of white noise within 0.5 dB of the measured filtered energy |
| `audio.mixer.unit` | silence when idle; ring full → refuse + count, the Start's instance stays with the caller; command at frame f lands at offset f mod 512; late command → offset 0 + count; instance return and `reclaim`; bank epoch retire; tap sees every frame once; `mix` with odd and varying frame counts gives the same samples as 512-frame calls; `earliest_frame` = clock + lead |

### 10.2 Install (exit 77 without; each checks install unchanged)

* **G1** `audio.waves`, **G2** `audio.loc`, **G3** `audio.decode.perf`, **G4** `audio.census`, **G6** `audio.bank`:
  §1 table.
* **G5** `audio.oracles`. Every value is evaluated, not read back. For band checks, Physics `Distance` is swept
  0–400 m in 0.25 m steps through `set_geometry` and `evaluate`.
  1. Census: the G4 numbers. The suite reuses the census.
  2. M16A3 bands. Every crossfader group's amplitude is 1 (or 0) outside its band, monotonic inside it, and changes
     within ±0.25 m of the §4.5 edges. The hifi layer is 1 at 0 m and 0 at ≥ 20 m. **Shape:** the 0–20 m group's
     amplitude at 2.5/5/10/15/17.5 m is 0.5 + 0.5 cos(π d / 20) within 1e-4 (a linear fade is off by ≥ 0.1 at 5 and
     15 m), and the three 250–300 m far groups (core bass, noise, reflections) follow the same cosine law at
     255/262.5/275/287.5/295 m (reading R: they fade out over that band; open question 1). The handling layers are 0 at ≥
     15/17/19 m. The far noise group (v87) rises over 20–50 m and falls over 250–300 m (reading R). A `Fire` posted at
     302 m starts no sampler, and one at 299 m does.
  3. M16A3 pitch/HPF. Over 10,000 seeded bursts, v28 ∈ [1.21, 1.262] with min/max within 0.5 % of the ends, and v28
     does not change across the 9 Steps of a burst (drawn per burst). The Lfo term v29 is ∈ [0.96, 1.0] with period
     250 ms ± 1 ms once the Adsr on v75 reaches full amplitude. v30 = v28 × v29. The voice-14 HPF (`plugin_attribute`
     on voice 14 plugin 2) at d = 0, 17, 108.5, 150, 200, 300 m is 0, 0, 155, 225.30, 310, 310 Hz (± 0.01 Hz); the
     voice-6 HPF (voice 6 plugin 1) in third person at 0, 17, 150 m is 80, 119.15, 230 Hz, and in first person at
     17 m 83.90 Hz.
  4. Other bands: M98B 300–350 m (and gate 300–351), RPG-7 45–90 m, M1A2 15–50 and 80–100 m. The M1A2 crossfaders are
     driven by distance like the weapons' (their control v81 is the M1A2's Physics `Distance`, §4.6).
  5. Cone curve: `output_gain_db` at 19.766/54.614/91.156/120.438/179.002 m equals Y + Gain within 0.01 dB, and the
     curve is monotonic between points.
  6. M16A3 routing and audible sets (third person): the compiled connections send voice 4 to the LFE bus and voice 14
     to voices 1 and 5; a Fire at 0.5 m starts exactly corebassclose, hifi_lmg and noise_close; at 60 m and 150 m
     exactly corebassdistant and noise_far; the stop at 150 m adds exactly the far reflections; no lfe or rattle
     sampler starts in third person and both do in first person; at 150 m the `Gai0` of voice 3 is 0 and of voice 5 is
     0.5 (0 at 0.5 m).
  7. Fire mode: with EnableStep 0, v100 pulses in the same evaluation as the gated Fire; with 1 it does not, and the
     stop pulses v100.
* **G7** `audio.m16.offline`: the check of §10.4, rendered offline.
* `audio.cache`: §8.
* `audio.fuzz` (`asan` preset; 77 without an install). Seeded mutations (bit flips, size-field edits, truncation) of
  real XAS chunks, through `scan_stream`/`StreamDecoder`. Mutated patch partitions go through `compile_sound_patch`
  (the `core.fuzz` partition mutator), and mutated program blobs through `decode_sound_program`. The pass rule: no
  crash, no hang (every case finishes under its `WorkBudget`), no allocation above the caps. Default 250 mutants per
  input; `OPENBF3_FUZZ_ITERS` raises it.

### 10.3 Godot

* **G8** `godot.audio`: §1 table and §10.4. It follows `godot_m1_load.cmake`: a probe run decides 77 (no window or
  rendering device), then the real run, then the test parses the PASS/FAIL table and checks the three WAVs exist.
  The demo loads only the M16A3 program and the waves it names (`WaveBank::load` with that wave list), starts the
  script at `earliest_frame()`, records 31 s from there, then renders the same script offline in-process and compares.
* The headless `godot.s0_selftest` gains no audio work. Headless runs use the Dummy driver; `godot.audio` is windowed
  because the gate asks for the real driver path.
* Before the first Godot test after a build, run `"<godot>" --headless --path bf3-opensource --import` once.
* G8 is a timing gate: run it on a quiet machine, like G3 (§1).

### 10.4 The M16A3 check (G7 and G8; `audio_check.h`)

One script, one driver and one analysis serve both gates, so the Godot recording is measured exactly like the
offline render.

**Script** (`m16_check_script`): third person, emitter on the listener's forward axis facing it, 10-round bursts at
3,600 frames, trigger up 1,800 frames after the last round, one burst every 4 s (longer than every tail), start at
0.25 s:

| segment | distance | bursts | seed | pins |
|---|---|---|---|---|
| near | 0.5 m | 2 | s, s + 1 | — |
| mid | 60 m | 2 | s + 2, s + 3 | — |
| far | 150 m | 2 | s + 4, s + 5 | — |
| far_pinned | 150 m | 1 | s + 4 (far's first burst) | voice-14 HPF (v46) and voice-6 HPF (v49) at 0 Hz |

Why these: at 0.5 m every Fire-driven layer is stepped; at 60 and 150 m the audible layers are the stepped
corebassdistant and the unstepped noise_far loop (§4.5); 60 m keeps clear of the 20–50 m noise band edges. Each burst
has its own `PatchInstance`, so far_pinned replays far's first burst with the same random draws.

**Rows shared by G7 and G8** (`analyse_check`):
* **(a) onsets**, on the core tap (voices 26–29: the two corebass samplers, stepped, without a Delay port, one of
  them audible at each distance): per burst window, exactly 10 onsets, every gap 3,600 ± 96 frames (the data's own onset
  jitter is up to 65 frames, §16). The full mix is
  not used for onsets: the unstepped noise loop adds segment onsets on its own 75.0–81.5 ms clock, and the hifi and
  noise layers carry per-burst delays of up to 45 ms.
* **(b) levels**, full mix: mean burst-window RMS near > mid > far, each step ≥ 3 dB. Derivation: the Cone/Flat curve
  alone falls from −9.6 dB (0.5 m, evaluated at `MinDistance` 10; −0.5 dB if `MinDistance` offsets instead, open
  question 2) to −35.9 dB at 60 m and −56.1 dB at 150 m, so the steps are ≥ 20 dB before the change of layer; the
  3 dB threshold leaves 17 dB for differences between the near and far waves' own levels.
* **(c) HPF, full mix**: 20–100 Hz energy over far's first burst window ≥ 10 dB below far_pinned's. Derivation:
  inside the window every audible path passes the voice-14 HPF (225.3 Hz) or the voice-6 HPF (230 Hz), the LFE path
  is silent, voice 3 is at gain 0, and voice 5 only forwards voice 14's filtered output (§4.5; G5 item 6 pins this). A
  Butterworth high-pass at 225.3 Hz (`iir2_q` 0.7071, |H|² = r⁴ / (1 + r⁴)) attenuates 100 Hz by 14.3 dB, 60 Hz by 23.0
  dB and 20 Hz by 42.1 dB; at 230 Hz 14.6 / 23.4 / 42.4 dB. So every in-band bin drops ≥ 14.3 dB; the 10 dB threshold
  leaves ≥ 4 dB for FFT leakage from 100–115 Hz (attenuated 12.0–14.3 dB). The far reflections, which bypass both HPFs,
  start at the stop, outside the window.
* **(d) HPF, noise tap** (voice 14, after its HPF): the same comparison on the tap ≥ 10 dB, and the far tap's
  measured band energy within 1.5 dB of `predicted_highpass_band_db` of the far_pinned tap at the evaluated v46
  (225.3 Hz) and `iir2_q`. This isolates the filter: the two taps differ in nothing else.

**Offline-only rows (G7):**
* **(e) model trace**: noise_far's segment starts are contiguous (each start = the previous segment's end ± 1 frame),
  loop segments only until the stop, then the tail segment (LastLoop + 1); the corebass Steps start exactly on their round
  frames, alternating slots.
* **(f) 0 Hz bypass**: an extra 17 m burst rendered with v46 as data (0 Hz there) and with v46 pinned to 0 is
  bit-identical.
* **(g) determinism**: the script rendered twice with seed s is bit-identical; with seed s + 100 it differs.
* **(h)** 0 starved voices, 0 decode errors, 0 deferred events, 0 dropped tap or trace frames.
* WAVs: `b/shots/audio/m16_offline.wav`, `_core.wav`, `_noise.wav`.

**Godot-only rows (G8):** recording ≥ 30.0 s; the counters of §7.3 at 0; the offline replay of the same commands
equals the recording sample for sample (the core's fixed grid and FTZ/DAZ make this exact; the row prints the largest
difference).

---

## 11. Hostile-input bounds

| input | bound | on violation |
|---|---|---|
| XAS block size | ≤ bytes left in the chunk; data blocks never decode past the header's total frames; ≤ `kMaxStreamFrames` | error with offset |
| channels / rate | 1–8 / 1,000–192,000 Hz | error |
| wave | ≤ 1,024 chunks, ≤ 65,536 variations, ≤ 256 segments per variation; `ChunkIndex` < chunks; localisation ranges inside variations; `SamplesOffset` < chunk size | error |
| wave bank | total ≤ `resident_budget_bytes`, checked before any read; chunk size equals the toc's | error with sizes, never truncated |
| patch | ≤ 4,096 nodes, ≤ 65,535 registers, ≤ 1,024 children per node, ≤ 256 voices × 64 plugins, ≤ 4,096 connections, ≤ 4,096 waves, ≤ 64 curve points; every index range-checked; `ConnectionIndex` 0 or 1..N, each connection named exactly once; every element charged to `WorkBudget` (1 M default) | error; budget exhaustion is an error, not a hang |
| program blob | counts vs bytes left, ranges, connections, every float finite, no trailing bytes (not topological order: the evaluator is safe in any order, §16) | error, cache entry dropped and recompiled |
| per block | ≤ 32 events per instance (rest deferred), ≤ `max_instances`, fixed ring capacity, ≤ 16 tap voices, fixed trace capacity | counted, never blocks or allocates |
| tuning cfg | known keys, ranged floats | error with line |
| WAV reader | ≤ `p_max_bytes`, chunk sizes checked against the file | error |

New decoders, parsers and the interpreter run under the `asan` preset (`audio.fuzz`, plus the unit tests).

---

## 12. Deferred

| item | wave |
|---|---|
| HDR loudness window, pre-cull, scopes (Closest_N, Max_N, DuplicateSpawn), 128-voice cap, profiler counters, 4.0/5.1 output, Pn21 multi-speaker | A3 (M2b) |
| AreaType from areas, mixer graph evaluator, `irf1` convolution reverb, Reverb bus | A4 (M3) |
| Footsteps, impacts (material grid) | A5 (M3) |
| Vehicle patches (DivisibleLoopPlayer, Delta, HighShelf, Route …), doppler | A6 (M5) |
| VO streaming through the StreamPool, VO logic, sound states, MasterUnit presets, the other 14 patch types; streaming the 21 non-VO pool waves | A7 (M7) |
| Effect sounds (`SoundEffectEntityData` from the FX instancer) | with VFX3 (M2a), through the same mixer (§7.2); not in A1/A2 |
| The 1p viewmodel firing the patch; the bank in `Bf3LevelSession` and the E4/E5 re-measure | M2a integration wave |

---

## 13. Files

**Created (interface stage, committed):**
* `src/bf3_core/include/bf3/core/audio/{sound_stream,sound_wave,wave_bank,sound_types,sound_program}.h`
* `src/bf3_core/src/audio/sound_types.cpp` (implemented: the type table)
* `src/bf3_audio/CMakeLists.txt`
* `src/bf3_audio/include/bf3/audio/{audio_config,dsp,patch_instance,mixer_core,wav_io,audio_analysis,audio_check,weapon_sound}.h`
* `src/bf3_audio/src/audio_config.cpp` (implemented: the tuning table and parser)
* `src/bf3_godot/src/audio_stream.{h,cpp}` (compiles; silence; not registered)
* `bf3-opensource/config/audio_tuning.cfg`
* `tests/audio_contract_tests.cpp`
* `docs/design/m2a-audio.md`

**Modified (additive blocks headed "# M2a audio"):**
* `CMakeLists.txt` (`add_subdirectory(src/bf3_audio)`)
* `src/bf3_core/CMakeLists.txt`
* `src/bf3_godot/CMakeLists.txt`
* `tests/CMakeLists.txt` (`audio.contract`)
* `README.md` (own section + one line under the ctest paragraph)

**Implementation waves add:**
* `src/bf3_core/src/audio/{sound_stream,sound_wave,wave_bank,sound_program,sound_program_blob}.cpp`
* `src/bf3_audio/src/{dsp,patch_instance,patch_nodes,patch_sampler,mixer_core,wav_io,audio_analysis,audio_check,weapon_sound}.cpp`
* `src/bf3_godot/src/audio_demo.{h,cpp}`
* `bf3-opensource/scenes/audio_m16.tscn`
* `tools/bf3dump/audio_commands.{h,cpp}`, `tools/bf3dump/audio_patch_commands.cpp`
* `src/bf3_audio/src/{audio_load,patch_state.h}` (and `include/bf3/audio/audio_load.h`: compile by name, read a level's
  or a program's waves)
* `tests/audio_unit_tests.cpp` (suites audio-xas, audio-patch, audio-dsp, audio-mixer), `tests/audio_install_tests.cpp`
  (audio-oracles, audio-cache, audio-fuzz), `tests/audio_gates.cmake` (G1–G4, G6, G7 through bf3dump)
* `tests/godot_audio.cmake`

**Shared files they touch:**
* `register_types.cpp`: the two `GDREGISTER_CLASS` lines (and the demo's) in their own block headed `// M2a audio`,
  after the existing registrations, so a merge with the fx and look tracks stays textual
* `project.godot` (a new `[audio]` section)
* `bf3dump.cpp` (one usage line + one dispatch branch)
* the CMake blocks above
* NOTICE: the repo has none yet. The IW4L port carries its attribution in the file header, which is the existing
  convention for IW4L ports in this tree.

---

## 14. Risks

| risk | effect | mitigation |
|---|---|---|
| Crossfader reading R is wrong (open question 1) | bands play the wrong layer; the gate distance cut is wrong; G7/G8's audible sets change | G5 pins reading R's values explicitly (a change is a visible gate change); one switch in the evaluator; a reference listen decides |
| AudioCurve X not metres, or MinDistance semantics differ (open question 2) | wrong level over distance; G5's Cone values change | `curve_x_metres`, `min_distance_clamp` tunables; G7/G8 row (b) holds either way (derivation in §10.4) |
| Sampler stepping and delay semantics (open question 4) | wrong auto-fire rhythm or doubled segments | onsets measured on the stepped corebass tap, which has no Delay; row (e) checks the model; the Sampler is isolated in `patch_sampler.cpp` |
| Butterworth Q for the IIR2 high-pass is wrong | rows (c)/(d) move | the derivation leaves ≥ 4 dB; row (d) compares with a prediction at the configured `iir2_q`, so a Q change moves prediction and measurement together |
| Shared machine at 100 % CPU | G3, G6's load time and G8 timing flakes (single decode runs measured 0.35–0.85 ms/ch-s wall under load) | G3 on thread CPU time; RUN_SERIAL; the quiet lock; G8 prints the worst block time and call gap so a flake is visible |
| WASAPI ignores the 48 kHz project rate on some device | Godot resamples; `rate_scale` ≠ 1 | log the server mix rate at start; a G8 failure says so; fallback: resample in `Rsp0` to the server rate (the ratio already takes the mix rate) |
| No audio device on the test machine | WASAPI fails → Dummy driver | the tap records the mixer output either way; G8 still measures the mixer thread |
| Godot frees the playback on its audio thread | a free on the audio thread | the session holds the owning reference and releases it last (§3) |
| Unresolved hashes (#3650803780 …) mean unknown game inputs | parameters stay at their defaults | binding is by hash (gap audio §14); defaults are the data's |
| 909 vs plan 908 | gate number differs from the plan text | documented scope difference (§4.1); G1 pins this tree's mount (open question 7) |

---

## 15. Open questions (for the user)

1. **MultiCrossfader semantics.** Reading R (§6.3): the last group fades out over its band, and Start/Stop pass only
   to audible groups. Under it the far noise layer fades in over 20–50 m and every layer fades out over 250–300 m;
   plan §7.7 item 1 says the "far layer fades in over 250–300 m". Is reading R right, and may the plan wording be
   updated to it? G5 pins reading R's values, including the 301 m Fire gate on the M16A3.
2. **AudioCurve X units** (metres assumed, plan §3.10 open), and whether `MinDistance` (10 on the M16A3 Cone) clamps
   the curve or offsets it. G5's Cone values beyond 19.766 m change if the answer is "offset".
3. **LPB0 damping formula** (`HFDamping*` → cutoff). A2 uses tunables. Is a reference recording available for A3?
4. **Sampler stepping.** Is "Fire first round, Step for each further round, stop event → the tail after the playing
   segment" the right per-round model for automatic fire, or does the game re-post `Fire` per round? And does a
   sampler's `Delay` shift only its first segment or (as assumed) every Step of the burst?
5. **Unresolved names** (#3650803780, #1114728387, #1365968171, #2484178491, #3520090073). They are bound by hash and
   left at their defaults in A2 (#1365968171 is added to EnableStep in the fire-mode selector and defaults to 0).
   Acceptable?
6. **Library split.** Data side in `bf3_core`, runtime in a new `bf3_audio` (§2) instead of "`bf3_audio` in
   `bf3_core`" (plan §3.10). OK?
7. **909 vs 908 Metro waves.** G1 pins 909 because this tree's `load_level` mount holds 12,315 partitions; the plan's
   M2a row says 908 (research scope: 11,514 partitions). May the plan wording be updated to 909 (this mount)?

---

## 16. Implementation notes: where the implementation departs from the interface stage

Each item is a rule the implementation wave found wrong or under-specified. The sections above already carry the
corrected text; this list says what changed and why.

1. **Onset floor = mean, not minimum (§9, `audio_analysis.h`).** The 2 ms envelope of a noisy tail swings by more than
   12 dB, so a minimum floor fired inside the M16A3 corebass tails: 11–14 onsets per 10-round burst on the G7 render,
   gaps off by up to 2,256 frames. The mean of the 20 ms before the window gives exactly 10 per burst. The refinement
   is also bounded below by `bed_sigma` (4) × the floor's RMS: without it a bed only 20 dB under the peak (the
   `audio.dsp.unit` click train of §10.1) has samples above −20 dB of the peak in almost every window, and the onset
   lands on the bed up to a window (78–80 frames) early. G7 is unchanged by the bound (same spacing to the frame).
2. **Onset tolerance ± 96 frames, not ± 48 (§1 G7, §10.4 row a).** Measured on the data, not on our render: 22 of 24
   corebassdistant segments open with a 64-frame lead-in at −35 to −60 dB before the attack, and corebassclose has
   late re-attacks, so the first frame at −20 dB of the local peak moves by up to 65 frames from segment to segment
   (`bf3dump audio wave MP_Subway TeamDeathMatch0 <wave partition> --envelope`, item 11; segments 1–24, the intro excluded). The Steps themselves start on their frames
   exactly (row e: 0 frames error). ± 96 = 2 ms, the envelope window; ± 48 would fail on the waves, not on the timing.
3. **The M1A2's v81 is distance (§4.6, §10.2 item 4).** Its crossfaders read v81, the output of its Physics node's
   `Distance` port. G5 item 4 sweeps distance for all three oracle patches; no register pin hook is needed.
4. **Sampler release rules (§6.3).** `LtForward` on Release finishes its pass to LastLoop and then plays the tail (the
   interface text said it ignores Release until Stop); `LtNone` ends after its current segment on Release. A
   continued segment is stamped in the trace on the frame it is first heard: the resampler holds two input samples
   of look-ahead, so the stamp is ceil((2 − frac) / ratio) frames after the next segment's first sample enters the
   history (2 frames at ratio 1; `audio.patch.unit` checks back-to-back segments 1,000 frames apart exactly).
5. **Stop and return (§6.5).** A `Stop` command posts the program's `DefaultStopEvent` at its frame unless the
   instance has already seen it, then marks the instance stop-requested; an instance goes back to the game thread
   once `finished()` and either stop-requested or it has taken at least one Sampler Trigger (a one-shot that is never
   stopped still returns). `Kill` gives the instance back at once, without a fade. Every command is frame-stamped
   (`at_frame`, 0 = the next block); a command whose frame was already mixed lands at offset 0 and counts
   `late_commands`. **A command for a handle whose `Start` is still waiting (stamped later) waits for that Start**: it
   is moved to the Start's frame and applied right after it in push order (counted late only if its own nonzero frame
   was already mixed). Without this a `SetParameter`/`PostEvent` stamped 0 behind a future-stamped Start found no
   instance and was dropped (`audio.mixer.unit` checks both cases). `MixerCore::clock()` is the number of frames
   rendered on the grid (a multiple of 512).
6. **Blob validation does not check topological order (§6.2, §11).** `validate_sound_program` checks every range,
   reference, connection and float; the evaluator is safe in any node order (a register read before its writer sees
   the previous step's value), so order is a timing property, not a safety one, and is left to the compiler.
7. **Audio plan details (§6.4).** `LPB0` is open (no damping until A3; `hf_damping_db_per_metre` = 0). The Reverb send
   is dropped; LFE sends are scaled by `lfe_fold_db`; every other bus sums at 0 dB. Plugin attributes: up to
   `kMaxPluginAttributes` = 16 per plugin (sound/master links attribute 4 and above of its MasterUnit plugins).
8. **Analysis hooks.** An instance takes up to `kMaxVoiceTaps` = 4 voice taps (`add_voice_tap`; the M16A3 check uses
   two, core and noise). `SamplerTraceEntry` carries `prev_end`, `wave` and `variation` for row (e).
   `WaveBank::from_memory` builds a bank from bytes in memory (the unit tests' PCM/XAS waves).
9. **Gates through bf3dump.** G6 (`audio.bank`) and G7 (`audio.m16.offline`) run as `bf3dump audio bank` / `audio
   check --g7` wrapped by `tests/audio_gates.cmake` (as G1–G4), not as doctests: one command per gate, runnable
   without ctest. `audio.oracles` (G5), `audio.cache` and `audio.fuzz` are doctest suites in
   `tests/audio_install_tests.cpp`; the data-free suites are in `tests/audio_unit_tests.cpp`.
10. **Node order (§10.1).** The compiler's topological sort is the free function `order_by_register_dependencies`
    (`sound_program.h`), so `audio.patch.unit` checks it synthetically: a chain written backwards, stable ties, a node
    reading its own output, a two-node cycle and a three-node ring (one feedback edge each), out-of-range registers and
    a fan-in larger than the budget. On real data `audio.census` compiles all 835 graphs with 0 feedback edges. XAS "channels 0" cannot be
    encoded (the header stores channels − 1) and the decoders take no `WorkBudget`: every loop is bounded by the
    bytes left and by the header's frame count, which `audio.xas.unit` and `audio.fuzz` exercise.
11. **Acceptance fixes.** G5 item 2 also checks the fade *shape* on the data (the cosine law of the 0–20 m close layer
    and of the three 250–300 m far groups), not only monotonicity and edges. The plan's "far layer fades in over
    250–300 m" is not tested as written: under reading R nothing fades in over that band (open question 1). `audio_gates.cmake` under `REPORT_ONLY` (asan) still
    fails on a non-zero bf3dump exit or a missing `install.unchanged yes`. `Bf3AudioPlayback::playing_` is a
    `std::atomic<bool>` (written on the main thread, read in `_mix` on the audio thread). The envelope command of item 2
    takes the wave's partition path, e.g. `bf3dump audio wave MP_Subway TeamDeathMatch0
    sound/weapons/shared_content/corebassdistant_lmg/corebassdistant_lmg_04_wave --envelope`.
