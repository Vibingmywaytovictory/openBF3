# M2a audio: A1 (XAS voices, wave bank, mixer) and A2 (weapon patch interpreter)

Track design for the audio part of milestone M2a ("Infantry gunplay", research/PORTING-RESEARCH.md §6), slices
**A1** and **A2** of gap audio §11.6. Branch `m2a/audio`. Status: interface stage. The headers listed in §13 compile.
`audio.contract` passes. Nothing plays yet.

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
`layout.toc`, `cas.cat` and every `.sb`/`.toc` is identical before and after the run. Timing gates run `RUN_SERIAL`.
This machine is shared with other workflows, so every timing in this document says the load it was taken under.

| id | gate (ctest) | threshold | how measured |
|---|---|---|---|
| G1 | `audio.waves` | Metro TDM (`load_level MP_Subway TeamDeathMatch0`): 909 `SoundWaveAsset`, every one read without error; every segment stream of every playable variation (English range for localised waves) decoded; **decoded frames == header frames: 0 mismatches**; header frames vs `SegmentLength × rate`: 0 off by more than 1 frame; 0 errors | `bf3dump audio waves MP_Subway TeamDeathMatch0 --decode`; the test parses its summary line |
| G2 | `audio.loc` | `Win32/Loc/en` chunk toc (base + patch): 4,290 chunks; every stream in every chunk scanned and decoded; 15,890 streams, **0 mismatches**, 0 errors; 7 chunks with no stream (non-sound) are reported and are not errors | `bf3dump audio loc en --decode` |
| G3 | `audio.decode.perf` | best of 3 single-thread decodes of all G1 streams **≤ 0.2 ms per channel-second** | `bf3dump audio waves MP_Subway TeamDeathMatch0 --bench 3` (wall time of `decode_stream` only, chunks already in memory); RUN_SERIAL |
| G4 | `audio.census` | over `@all-mp`: 834 `SoundPatchAsset` (833 under `sound/`) + 1 `SoundMasterPatchAsset`; 32,117 node instances of 46 types in patches; 32,221 of 58 types with master and mixers; 7,323 voices; 1,879 `SoundWaveAsset`; **the 164 handheld weapon patches use exactly the 32 A2 types and all compile strictly**; all 835 patches compile with `allow_unimplemented`; 0 unknown types | `bf3dump audio census @all-mp` |
| G5 | `audio.oracles` | plan §7.7 items 1–4, plus the Cone curve (item 5), all evaluated by the interpreter on compiled programs. Item list and tolerances in §10.2 | doctest `audio.oracles` (install) |
| G6 | `audio.bank` | the Metro TDM wave bank loads with 629 resident waves = **265.4 MiB ≤ 384 MiB** budget; 280 localised waves skipped; every wave named by a program compiled from Metro's 164 weapon patches is resident (0 starved) | doctest `audio.bank` (install) |
| G7 | `audio.m16.offline` | M16A3 rendered offline through `MixerCore` (no Godot) at 5, 50 and 150 m, 10-round bursts at 800 rpm: **onsets 10 per burst, spacing 3600 ± 48 frames (75 ms ± 1 ms)**; RMS(5 m) > RMS(50 m) > RMS(150 m), each step ≥ 3 dB; **HPF at 150 m: 20–120 Hz energy ≥ 10 dB below the same render with the high-pass pinned to 0 Hz**, and ≤ 0.5 dB apart at 17 m; same seed gives a bit-identical render, a different seed does not; 0 starved voices, 0 decode errors; WAVs written to `b/shots/audio/` | doctest `audio.m16.offline` (install) |
| G8 | `godot.audio` | windowed Godot run of `res://scenes/audio_m16.tscn -- --audio-check --audio-record=<b/shots/audio/godot_m16.wav> --quit-after=30` after a probe run: a scripted trigger fires 10-round bursts for 30 s; **underruns 0**, meaning `short_blocks` = 0, `late_blocks` = 0, `ring_overflows` = 0 and `starved_voices` = 0 (MixerStats, §7.3); the recorded WAV covers ≥ 29.5 s and its onsets pass the G7 spacing rule; install unchanged; exit 77 without a window or `OPENBF3_GODOT` | `tests/godot_audio.cmake` (the `godot_m1_load.cmake` pattern), RUN_SERIAL, RESOURCE_LOCK `openbf3_godot_project` |
| G0 | `audio.contract` | every audio header compiles together; type table sets; seam hashes and FourCCs; tuning parser and `audio_tuning.cfg` | doctest, no data. **Passes now** |

Unit gates that need no data: `audio.xas.unit`, `audio.patch.unit`, `audio.mixer.unit`, `audio.dsp.unit`. The hostile
input gate is `audio.fuzz`, which runs under the `asan` preset (§11).

The plan's M2a row says "908 Metro waves". This tree's `load_level` scope finds 909 (§4.1), and G1 pins 909. The
difference is a scope difference, not a reader difference.

---

## 2. Module ownership

| module | owns | Godot? | M2a audio files |
|---|---|---|---|
| `bf3_core` `audio/` | reading the data: XAS stream codec, `SoundWaveAsset` reader, `WaveBank` (resident chunk bytes), the node type table, the `SoundPatchAsset` compiler (control program + audio plan), the program blob and its cache entries | no | `include/bf3/core/audio/*.h`, `src/audio/*.cpp` |
| `bf3_audio` (new library) | the runtime: `PatchInstance` (the A2 interpreter), `MixerCore`, DSP, WAV I/O, analysis, tuning, the weapon seam | no | `src/bf3_audio/**` |
| `bf3_godot` | `Bf3AudioStream`/`Bf3AudioPlayback` (the `AudioStreamPlayback` subclass), the demo node and its checks, scene `audio_m16.tscn` | yes | `src/bf3_godot/src/audio_*.{h,cpp}` |
| `tools/bf3dump` | `bf3dump audio …` subcommands (G1–G4 drivers, offline render) | no | `tools/bf3dump/audio_cmd.cpp` + an additive dispatch line |

Plan §3.10 and gap audio §11.1 put "`bf3_audio` in `bf3_core`". **Decision:** the data side (codec, readers, compiler)
goes in `bf3_core`. That is where every other reader lives, and the dedicated server and `bf3dump` need it without a
mixer. The runtime (interpreter state, mixer, DSP) is a separate Godot-free library `bf3_audio`, client only. The
dedicated server links neither the runtime nor any audio thread; it runs dummy audio (plan §3.10, gap audio §11.3).
The headless split the plan asks for still holds, because every piece is testable without Godot.

---

## 3. Threads

| thread | does | never |
|---|---|---|
| level loader (existing loader thread; plan §2.3) | reads the level's `SoundWaveAsset`s (`read_sound_wave`, charged to a `WorkBudget`); compiles or takes from the cache every `SoundPatchAsset` the level needs (`parallel_map` over patches); reads all resident chunks in parallel (`parallel_for`) into a `WaveBank`; publishes `shared_ptr<const WaveBank>` and the programs | touch the mixer |
| game thread (Godot main) | `PatchInstance::create` (all allocation happens here); per tick pushes POD `MixerCommand`s (start, stop, parameters, events stamped with a mix frame, emitter/listener); `reclaim()` destroys returned instances and retired banks; `poll_feedback()` reads haptics | block on the audio thread |
| audio thread (Godot's AudioServer thread: WASAPI, or the Dummy driver's thread when headless) | `Bf3AudioPlayback::_mix` → `MixerCore::mix`: drains commands, evaluates and renders each instance per sub-block, sums buses, folds to stereo, writes into Godot's buffer; feeds the `MixTap` | allocate, lock, log, read files, wait |
| test / offline render | calls `MixerCore::mix` directly in a loop (G7, unit tests, `bf3dump audio render`) | — |
| WAV writer | `WavRecorder::save` on the game thread after playback stopped (PathGuard checked) | write from the audio thread |

Handoff rules (§7): three SPSC rings (commands in; finished instances out; feedback out). Instances move by pointer.
Banks are retired by epoch. The audio side never frees memory.

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
* Research figures for comparison (gap audio §9): 908 waves, 606 + 21 + 281, 245.7 MiB. The differences come from the
  mount scope.
* With the English range only: 11,973 variations, 12,955 segments. Every stream is codec 4 (EA-XAS v1).
* Rates: 11,025 Hz ×6, 16,000 Hz ×1, 44,100 Hz ×206. At 48 kHz: 11,747 mono, 648 stereo, 346 quad, 1 six-channel.
* The 280 VO waves' chunks are absent from the level mount. Every one of them is found in `Win32/Loc/en` through
  `ui::read_toc_chunk(install, "Win32/Loc/en", id)` (text_db.h).
* **0 read errors. 0 decoded-vs-header frame mismatches. 0 header-vs-`SegmentLength × rate` mismatches above 1 frame.**
* Maxima: 69 chunks per wave, 416 variations per wave (all languages; 102 in English), 29 segments per variation.
* Decode speed, one thread: 0.092–0.115 ms per channel-second when the machine was lightly loaded. It rose to 0.35
  and 0.85 while other workflows held the CPU at 100 % (the 0.85 run covered 27,993 channel-seconds).
  gap audio §9 [research]: 0.117–0.120.

`SoundWaveAsset` fields as the partitions' own type table names them. Checked by dumping
`sound/weapons/shared_content/hifi_lmg/hifi_lmg_m4_wave` and `sound/vo/common/ru/autotriggers/vo_ru_mp_alert_hmg`:
* `ChannelCount`
* `Chunks[] {ChunkId, ChunkSize}`
* `Variations[] → SoundWaveVariation {ChunkIndex, FirstLoopSegmentIndex, LastLoopSegmentIndex, SeekTablesSize,
  Segments[] {SamplesOffset, SegmentLength, SeekTableOffset}}`
* `Localization[] {Language → AudioLanguage, FirstVariationIndex, VariationCount}`
* `StreamPool`, `Seekable`, `Selection`, `VoicePriority`, `PersistentVariationCount`, `SubtitleStringIds[]`

The M16A3 wave `hifi_lmg_m4_wave` has 1 variation of 14 segments. Each segment is its own XAS stream at its
`SamplesOffset`:
* segment 0: the first shot, 94.7 ms;
* segments 1–12: the loop, 94.7 ms each;
* segment 13: the tail, 0.377 s.

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

The voice graph has three kinds of voice:
* Sampler voices (stage 0: `SnP1`, `Rsp0`, `Pau0`, `Gai0`, `Sen0` → voice) send into submix voices (`Sub0` …).
* Submix voices feed output voices (`Sub0`, `LPB0`, `VuM0`, `Pn21`, `Sen0` → bus).
* `Pn21` construct params are 45, 135 and 2.0.

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
* `Loudness` 110, `Radius` 0.8, `IsLooping` true.
* **Events:** `Fire` (start, v96), `#1920007028` (stop = `DefaultStopEvent`, v99), `Reload`, `#2484178491`,
  `#1968287961` (the Sampler `Step`, v111) and `#3520090073`.
* **Parameters:** `IsFirstPerson` (v13, v58, v65, v108), `#45329532` (the WaveSwitcher index for long/short reload),
  `#3650803780`, `#1114728387`, `#1365968171` and `#4198762218` (`EnableStep`, v110).
* Name hashes are djb2-xor (`ebx_name_hash`): Fire 2088671869, IsFirstPerson 824639024, Reload 3294210644,
  AreaType 3772263626. `audio.contract` pins these.
* **MultiCrossfader bands:**

  | crossfader | groups |
  |---|---|
  | close | 0–20 m, cosine |
  | two groups | 5–35 / 250–300 m |
  | three groups | 1–5 (linear) / 20–50 / 250–300 m |
  | two groups, LockControlValue | 3–15 / 250–300 m |
  | handling | 1–15, 1–17, 1–19 m, linear |
  | gate | 300–301 m, linear (Start v96 → v97) |

* **HPF:** `ScaleClamp#5` maps v22 from 17..200 to 0..310 → v46 → `HighPassIir2#2` (voice 14, plugin 2).
* **Pitch:** `NumberGenerator#3` U[1.21, 1.262] → v28. It is multiplied by `Lfo#3` (0.96–1.0, 4 Hz, amplitude v75 =
  `Adsr#2` with A 0.7 s) → v30, the pitch.
* **Cone output:**
  * Curve points (X, Y, K): (0, 0, −0.9757), (19.766, −17.9168), (54.614, −34.5664), (91.156, −41.1488),
    (120.438, −46.9568), (179.002, −67.0912).
  * `MinDistance` 10, `HFDampingDistance` 0.8, `ReverbGain` −17.
  * The Handling output is a Cone at −29 dB.
  * The LFE output is a Direct output at +3 dB to the LFE bus, with `EnableHdr` false.
* Samplers have 1–2 plugin slots each (voice polyphony).

### 4.6 Other oracle patches [verified]

* **M98B** `weapon_sniper_m98_b_02`: 0–20; (5–35, 300–350); (2–10, 30–60, 300–350); (8–40, 300–350); 1–10; 1–10 linear;
  1–15; gate 300–351 linear (amplitude v89); 1–10 linear.
* **RPG-7** `weapon_at_rpg7`: 5–40; (5–20, 250–300); (5–35, 45–90, 250–300); (15–45, 250–300); 1–15; 1–19 linear; gate
  300–301.
* **M1A2** `sound/vehicles/land/m1a2_abrams/m1a2_abrams`: 90–120 (unlocked, start/stop), 30–40, 40–60 ×2, 15–50
  (unlocked), 80–100 ×2. The control register is v81, not distance. The M1A2 uses non-weapon types, so it compiles only
  with `allow_unimplemented`.

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
* Metro TDM: 629 waves, 265.4 MiB [verified sizes; the load itself is gate G6].
* Chunk bytes are re-read at every level load and never cached (plan §2.4: no decoded audio, no raw data in the cache).

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
  (voice → voice, or voice → bus by `BusName`), linked plugin attributes, and output nodes with their curves and
  damping.
* **Topological sort.** Iterative Kahn over register writers and readers, O(nodes + reads). A cycle is broken at the
  closing edge, which then reads the previous step's value; these are counted as `feedback_edges` [inferred: none
  expected in weapons, and G4 checks it].
* **Hostile input.** The structural caps in `sound_program.h` apply, and every array element and reference is charged
  to the `WorkBudget` before it is read. Register indices are bounded by `kMaxProgramValues`. Every `ConnectionIndex`,
  `VoiceIndex`, plugin and construct-param range is checked.
* **Blob.** "OBF3SNDP": `decode_sound_program` re-validates everything, so a cached program is as safe as a fresh
  compile.

### 6.3 Evaluation model (`patch_instance.h`)

* **Registers.** One float array per instance. Unconnected ports read their constant.
* **Events.** These are one-step pulses: the register reads 1 for exactly one evaluation, then 0 [inferred; gap audio
  §1.2: "event pulses"].
* **Control rate.** Each 512-frame mix block is split at the frame offsets of the events posted for it. Pending events
  pulse at each split, the program runs once, and the following frames render. Time-based nodes (Adsr, Lfo, Timer,
  ParameterFilterLinear) advance by the sub-block length. An event therefore lands on its exact frame, and G7 measures
  75 ms onsets to 1 ms. Cost is bounded by (events + 1) × program size per block, with at most 32 events per block
  (more are deferred and counted).
* **Randomness.** A counter-based generator (SplitMix64 of instance seed, node index and draw count) serves
  NumberGenerator, Lfo `StartAtRandomValue`, WaveSwitcher `IsRandom`/`RandomStartIndex`, Sampler variation choice and
  `ShuffleSegments`. Renders are deterministic per seed on any thread (G7).
* **MultiCrossfader**, reading R [inferred, open question 1]:
  * Groups are ordered.
  * The first group is at full amplitude below its band.
  * Band i crossfades group i out and group i+1 in (Cosine: raised cosine; Linear: linear).
  * The last group fades out over its band, and a single-group crossfader is a fade-out over its band.
  * `FadeAmplitude` scales the group.
  * Start/Stop events are passed only to groups whose amplitude is above 0 when the event arrives. The M16A3 gate
    300–301 m therefore blocks `Fire` beyond 301 m, and the far layers at 250–300 m are the last audible ones.
  * `LockControlValue` latches the control at Start.
  * Evidence for this reading: three weapons carry the same "gate" crossfader at 300–301 / 300–351 m. It makes sense
    only as an audibility cut-off, and the handling layers 1–15/17/19 m only make sense as fade-outs.
  * Plan §7.7 phrases item 1 as "far layer fades in over 250–300 m". Under reading R, the 250–300 m band is where the
    mid layer hands over to the far layer and the far layer fades out at the gate. Whether "far layer" means the group
    that fades in or the last group is open question 1. G5 checks the band edges and monotonicity, which both readings
    share, plus reading R's gating.
* **Sampler** [inferred, open question 4]:
  * Each Sampler owns its `Plugins[]` slots (1–2 on weapons).
  * `Trigger` plays the variation's first segment on the next free slot.
  * While `EnableStep` is 1, `Step` starts the next loop segment (in order, or shuffled) on the other slot at the event
    frame, so 94.7 ms segments overlap at 75 ms.
  * `Release` (or the stop event reaching `LtRelease`) plays the segment after the loop, which is the tail.
  * `LtForward` loops the segment range; `LtInstantRelease` fades out over `release_fade_ms`.
  * Pitch = `BasePitch × Pitch × MasterPitch` → `Rsp0` ratio.
* **WaveSwitcher.** `Index` selects from `Waves[]` and `Advance` steps it.
* **Physics.** `Distance`, `Azimuth`, `ElevationAngle`, `Velocity` and `RelativeVelocity` come from emitter and
  listener (Frostbite axes).
* **MixerReceive** writes 0 for AreaType (plan M2a: AreaType stubbed to 0). `set_mixer_value` is the A4 seam.
* **Outputs (Cone/Flat/Direct).** Attenuation = curve(distance) + `Gain`. The curve is a cubic Hermite through the
  points with slopes K, clamped at the ends [inferred]. Distances below `MinDistance` evaluate at `MinDistance`
  [inferred]. Cone `InnerAngle`/`OuterAngle`/`OutsideGain` are applied on the emitter's forward axis. HF damping drives
  `LPB0`.
* **CameraShake/PadRumble** write registers that are exported through `PatchHaptics` → `MixerFeedback`; the camera
  consumes them in M2a integration.

### 6.4 Audio plan execution

Voices render in ascending `ProcessingStage`:
* `SnP1` (StreamDecoder + slot state) → `Rsp0` (cubic resampler; rate = wave rate / 48,000 × pitch) → `Pau0`
  (pause/resume) → biquads (`HI20` high-pass, `LI20` low-pass, `BI20` band-pass, `PI20` peaking: RBJ cookbook,
  coefficients ramped per sub-block) → `Gai0` (gain ramp).
* `Sen0` sends to a submix voice's `Sub0` input or to a bus.
* Output voices apply `LPB0` (one-pole HF damping), `VuM0` (meter only), and `Pn21`, which folds the 4.0 pan to
  stereo equal-power. The pan uses the construct params; 4.0/5.1 output is A3.
* Buses fold to stereo: GameMixMain/Alt at 0 dB; LFE at `lfe_fold_db`; the Reverb send is dropped until A4.
* **Level.** There is no HDR window (A3). Output gain = `Loudness − window_top_db − window_headroom_db`, a fixed
  window [inferred], so a 110 dB M16A3 shot sits at −11 dBFS before its own attenuation.

### 6.5 Mixer core (`mixer_core.h`) and Godot (`audio_stream.h`)

* `MixerCore::mix(float*, n)` always fills n frames, internally in 512-frame blocks.
* `Bf3AudioPlayback::_mix` writes straight into Godot's `AudioFrame` buffer. The layouts are identical, so there is no
  copy. It always returns the frame count: returning fewer makes Godot fade the stream out (gap audio §10).
* Godot's mix block is a fixed 512 frames.
* `project.godot` gets an additive `[audio] driver/mix_rate=48000` in the A1 wave, so the server runs at the data's
  rate and no resampling happens. WASAPI is opened with `AUDCLNT_STREAMFLAGS_RATEADJUST`, which follows the project
  rate (gap audio §10) [research].
* One `AudioStreamPlayer` on the Master bus plays the stream for the whole session. Godot buses are used only for
  device output and volume (gap audio §11.2).

### 6.6 Tuning (`audio_config.h`, `bf3-opensource/config/audio_tuning.cfg`)

There are 14 keys in 5 sections, each with a range and a source note. All are [inferred] or fixed-window stand-ins,
and none is hard-coded in the evaluator (plan D14). The parser refuses unknown keys, out-of-range values and malformed
lines, reporting the line number. `audio.contract` checks that the project file parses and names every key.

### 6.7 Weapon seam (`weapon_sound.h`)

* `bind_weapon_sound(program, shot_interval, rate)` finds the start, stop and step inputs.
* `WeaponSoundDriver` turns trigger down, each later round and trigger up into commands stamped with mix frames.
  Trigger down sets `IsFirstPerson` and posts `Fire`. Later rounds post Step. Trigger up posts `DefaultStopEvent`.
* The M2a gunplay integration calls only this. It converts the client's predicted shot time to a mix frame and binds
  the patch from `WeaponDef::sound`.
* The shot interval is `gameplay::shot_interval(FireFunction)`: 75 ms for the M16A3.

---

## 7. Interfaces and handoff

### 7.1 Headers (all compile now; G0)

| header | key API |
|---|---|
| `bf3/core/audio/sound_stream.h` | `SoundCodec`, `SoundStreamHeader`, `parse_stream_header`, `scan_stream`, `decode_stream`, `decode_xas_frame`, `StreamDecoder` |
| `bf3/core/audio/sound_wave.h` | `SoundWaveInfo` (+ chunk, variation, segment, localisation), `read_sound_wave`, `playable_variations` |
| `bf3/core/audio/wave_bank.h` | `WaveBank::load/find/chunk/resident/stats`, `WaveBankOptions`, `WaveId` |
| `bf3/core/audio/sound_types.h` | `SoundNodeType`, `SoundNodeTypeInfo`, `sound_node_types/type_info/type`, enumerator enums |
| `bf3/core/audio/sound_program.h` | `SoundProgram` (control program + audio plan), `compile_sound_patch`, `encode/decode_sound_program`, `compile_sound_patch_cached`, `fourcc`, `MasterBus`, `AudioCurve` |
| `bf3/audio/audio_config.h` | `AudioTuning`, `audio_tuning_keys`, `parse_audio_tuning` |
| `bf3/audio/dsp.h` | `BiquadCoefficients::design`, `Biquad`, `OnePoleLowPass`, `apply_gain_ramp`, `equal_power_pan` |
| `bf3/audio/patch_instance.h` | `PatchInstance::create/set_parameter/post_event/set_mixer_value/set_geometry/process_block/evaluate`, inspection (`value`, `plugin_attribute`, `crossfader_group_amplitude`, `sampler_slots`, `output_gain_db`, `haptics`), test hook `override_plugin_attribute` |
| `bf3/audio/mixer_core.h` | `MixerCore` (`push`, `next_handle`, `reclaim`, `poll_feedback`, `mix`, `set_tap`), `MixerCommand`, `MixerStats`, `MixerFeedback`, `MixTap` |
| `bf3/audio/wav_io.h` | `write_wav` (PathGuard), `read_wav`, `WavRecorder` |
| `bf3/audio/audio_analysis.h` | `detect_onsets`, `rms_db`, `peak_db`, `band_energy_db`, `onset_spacing` |
| `bf3/audio/weapon_sound.h` | `WeaponSoundBinding`, `bind_weapon_sound`, `WeaponSoundDriver`, seam hashes |
| `bf3_godot/src/audio_stream.h` | `Bf3AudioStream`, `Bf3AudioPlayback` (renders silence until A1; not registered yet) |

### 7.2 Seams for later M2a integration

* **Gunplay → audio.** `WeaponSoundDriver` (§6.7). Remote soldiers get one `PatchInstance` each, with `IsFirstPerson`
  0 and an emitter at the muzzle.
* **Session.** `Bf3LevelSession` (M1 T6) owns one `MixerCore`, one `Bf3AudioStream` and one `AudioStreamPlayer`. The
  level loader adds a wave-bank + program step, on the loader thread, inside the existing load phases.
* **Camera.** `MixerCore::poll_feedback` → camera shake.
* **A3 (M2b).** The HDR window, scopes and culling replace the fixed window in the output gain step and add voice
  priority. `MixerStats` gains the profiler counters.
* **A4 (M3).** `set_mixer_value(kHashAreaType, …)` from the area evaluator, plus the Reverb bus.

### 7.3 Underrun accounting (G8)

* `short_blocks`: `mix` produced fewer frames than asked. This should be impossible by construction, and the gate
  pins it.
* `late_blocks`: computing a block took longer than its real-time length of 10.67 ms.
* `ring_overflows`: a command push was refused.
* `starved_voices`: a slot whose wave was not resident.

Godot does not expose device xruns to GDExtension. The demo also logs gaps between `_mix` calls above 3 block lengths
as report-only.

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
* **In Godot.** `--audio-record=<path>` attaches the same tap. The WAV is the mixer's output, not a loopback capture,
  so it works under the Dummy driver too.
* Every path goes through `PathGuard`: writes inside the install or outside the allowed roots fail before opening.
* Recordings go to `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\audio\` (gitignored `/b/`) and are never
  committed.
* **Analysis** (`audio_analysis.h`):
  * Onsets come from a 2 ms energy envelope rising 12 dB over its floor, with a 30 ms refractory period.
  * RMS and peak in dBFS.
  * Band energy through 4th-order RBJ band splits.
  * G7/G8 apply `onset_spacing` with an expected 3,600 frames.

---

## 10. Test plan

### 10.1 No data (doctest)

| test | content |
|---|---|
| `audio.contract` (G0, **exists**) | headers compile together; type table counts, unique names, name round trip; enumerator mapping; seam hashes = `ebx_name_hash`; FourCCs; tuning parser and the project cfg |
| `audio.xas.unit` | hand-built streams. Frames with each coefficient pair against a reference computed in the test from the format definition; header fields; multi-block walk; `skip_to` equals decode-and-drop. Hostile: truncated block, size 0, size past end, channels 0 / > 8, rate out of range, unknown codec, frames > cap, data past header frames, missing end block, budget exhaustion |
| `audio.patch.unit` | synthetic `SoundProgram`s built in code. Per A2 type: semantics at edge values (ScaleClamp clamp, SimpleTransform every op incl. domain errors → 0, Condition/Logical truth tables, DeMux wrap, Switcher/ValueSelector default, Timer modes, Adsr phases, Lfo range/period, NumberGenerator range/determinism, MultiCrossfader reading R incl. gating and lock, EventGate cool-down, ParameterFilterLinear slew, Sampler slots/step/release). Topological order; cycle → feedback edge; event pulse lasts one evaluation; sub-block split at event frames; `encode/decode` round trip; every corrupted byte of a blob is refused or decodes to a valid program |
| `audio.dsp.unit` | biquad magnitude at fc, fc/2 and 2fc within 0.1 dB of analytic for each shape; coefficient ramp continuity; one-pole cutoff; resampler passes a sine at ratio 1 bit-exact and at 0.5/2 within −60 dB THD; pan law at 0 / ±90° |
| `audio.mixer.unit` | silence when idle; ring full → refuse + count; command at frame f lands at offset f mod 512; late command → offset 0 + count; instance return and `reclaim`; bank epoch retire; tap sees every frame once; `mix` with odd frame counts |

### 10.2 Install (exit 77 without; each checks install unchanged)

* **G1** `audio.waves`, **G2** `audio.loc`, **G3** `audio.decode.perf`, **G4** `audio.census`, **G6** `audio.bank`:
  §1 table.
* **G5** `audio.oracles`. Every value is evaluated, not read back. For band checks, Physics `Distance` is swept
  0–400 m in 0.25 m steps through `set_geometry` and `evaluate`.
  1. Census: the G4 numbers. The suite reuses the census.
  2. M16A3 bands. Every crossfader group's amplitude is 1 (or 0) outside its band, monotonic inside it, and changes
     within ±0.25 m of the §4.5 edges. The close layer is 1 at 0 m and 0 at ≥ 20 m. The handling layers are 0 at ≥
     15/17/19 m. Reading R: a `Fire` posted at 302 m starts no sampler, and one at 299 m does.
  3. M16A3 pitch/HPF. Over 10,000 seeded Fire events, v28 ∈ [1.21, 1.262] with min/max within 0.5 % of the ends. The
     Lfo term is ∈ [0.96, 1.0] with period 250 ms ± 1 ms once `Adsr#2` reaches full amplitude. The final pitch
     register equals their product. The HPF frequency at d = 0, 17, 108.5, 200, 300 m is 0, 0, 155, 310, 310 Hz
     (± 0.01 Hz), via `plugin_attribute` on voice 14 plugin 2.
  4. Other bands: M98B 300–350 m (and gate 300–351), RPG-7 45–90 m, M1A2 15–50 and 80–100 m. The M1A2 crossfaders are
     driven through register v81, since its control is not distance.
  5. Cone curve: `output_gain_db` at 19.766/54.614/91.156/120.438/179.002 m equals Y + Gain within 0.01 dB, and the
     curve is monotonic between points.
* **G7** `audio.m16.offline`: §1 table. Third person, listener on the cone axis.
* `audio.cache`: §8.
* `audio.fuzz` (`asan` preset; 77 without an install). Seeded mutations (bit flips, size-field edits, truncation) of
  real XAS chunks, through `scan_stream`/`StreamDecoder`. Mutated patch partitions go through `compile_sound_patch`
  (the `core.fuzz` partition mutator), and mutated program blobs through `decode_sound_program`. The pass rule: no
  crash, no hang (every case finishes under its `WorkBudget`), no allocation above the caps. Default 250 mutants per
  input; `OPENBF3_FUZZ_ITERS` raises it.

### 10.3 Godot

* **G8** `godot.audio`: §1 table. It follows `godot_m1_load.cmake`: a probe run decides 77 (no window or rendering
  device), then the real run, then the test parses the PASS/FAIL table and checks that the WAV exists.
* The headless `godot.s0_selftest` gains no audio work. Headless runs use the Dummy driver; `godot.audio` is windowed
  because the gate asks for the real driver path.
* Before the first Godot test after a build, run `"<godot>" --headless --path bf3-opensource --import` once.

---

## 11. Hostile-input bounds

| input | bound | on violation |
|---|---|---|
| XAS block size | ≤ bytes left in the chunk; data blocks never decode past the header's total frames; ≤ `kMaxStreamFrames` | error with offset |
| channels / rate | 1–8 / 1,000–192,000 Hz | error |
| wave | ≤ 1,024 chunks, ≤ 65,536 variations, ≤ 256 segments per variation; `ChunkIndex` < chunks; localisation ranges inside variations; `SamplesOffset` < chunk size | error |
| wave bank | total ≤ `resident_budget_bytes`, checked before any read; chunk size equals the toc's | error with sizes, never truncated |
| patch | ≤ 4,096 nodes, ≤ 65,535 registers, ≤ 1,024 children per node, ≤ 256 voices × 64 plugins, ≤ 4,096 connections, ≤ 4,096 waves, ≤ 64 curve points; every index range-checked; every element charged to `WorkBudget` (1 M default) | error; budget exhaustion is an error, not a hang |
| program blob | counts vs bytes left, ranges, topological order, no trailing bytes | error, cache entry dropped and recompiled |
| per block | ≤ 32 events per instance (rest deferred), ≤ `max_instances`, fixed ring capacity | counted, never blocks or allocates |
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
| VO streaming through the StreamPool, VO logic, sound states, MasterUnit presets, the other 14 patch types | A7 (M7) |
| Effect sounds (`SoundEffectEntityData` from the FX instancer) | with VFX3 (M2a), through the same mixer; not in A1/A2 |
| Registering `Bf3AudioStream` and the demo, `[audio] driver/mix_rate=48000`, scene `audio_m16.tscn` | A1/A2 implementation waves (this design fixes the interfaces) |

---

## 13. Files

**Created (interface stage, committed):**
* `src/bf3_core/include/bf3/core/audio/{sound_stream,sound_wave,wave_bank,sound_types,sound_program}.h`
* `src/bf3_core/src/audio/sound_types.cpp` (implemented: the type table)
* `src/bf3_audio/CMakeLists.txt`
* `src/bf3_audio/include/bf3/audio/{audio_config,dsp,patch_instance,mixer_core,wav_io,audio_analysis,weapon_sound}.h`
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
* `src/bf3_audio/src/{dsp,patch_instance,patch_nodes,patch_sampler,mixer_core,wav_io,audio_analysis,weapon_sound}.cpp`
* `src/bf3_godot/src/audio_demo.{h,cpp}`
* `bf3-opensource/scenes/audio_m16.tscn`
* `tools/bf3dump/audio_cmd.cpp`
* `tests/audio_{xas,patch,dsp,mixer,install,fuzz}_tests.cpp`
* `tests/godot_audio.cmake`

**Shared files they touch:**
* `register_types.cpp` (two `GDREGISTER_CLASS` lines after the M1 T6 line)
* `project.godot` (an `[audio]` section)
* `bf3dump.cpp` (one usage line + one dispatch branch)
* the CMake blocks above
* NOTICE: the repo has none yet. The IW4L port carries its attribution in the file header, which is the existing
  convention for IW4L ports in this tree.

---

## 14. Risks

| risk | effect | mitigation |
|---|---|---|
| Crossfader reading R is wrong (open question 1) | bands play the wrong layer; the gate distance cut is wrong | G5 checks edges common to both readings separately from gating; one switch in the evaluator; a reference listen decides |
| AudioCurve X not metres, or MinDistance semantics differ (open question 2) | wrong level over distance | `curve_x_metres`, `min_distance_clamp` tunables; G7 monotonic-level rule holds either way |
| Sampler stepping semantics (open question 4) | wrong auto-fire rhythm or doubled segments | G7 onset count and spacing catch doubling and drift; the Sampler model is isolated in `patch_sampler.cpp` |
| Shared machine at 100 % CPU | G3 and G8 timing flakes (single decode runs measured 0.35–0.85 ms/ch-s under load) | best of 3, RUN_SERIAL, the quiet lock; G8 `late_blocks` reports the worst block time so a flake is visible |
| WASAPI ignores the 48 kHz project rate on some device | Godot resamples; `rate_scale` ≠ 1 | log the server mix rate at start; a G8 failure says so; fallback: resample in `Rsp0` to the server rate (the ratio already takes the mix rate) |
| No audio device on the test machine | WASAPI fails → Dummy driver | the tap records the mixer output either way; G8 still measures the mixer thread |
| Unresolved hashes (#3650803780 …) mean unknown game inputs | parameters stay at their defaults | binding is by hash (gap audio §14); defaults are the data's |
| 909 vs plan 908 | gate number differs from the plan text | documented scope difference (§4.1); G1 pins this tree's mount |

---

## 15. Open questions (for the user)

1. **MultiCrossfader semantics.** Reading R (§6.3): the last group fades out over its band, and Start/Stop pass only
   to audible groups. Plan §7.7 item 1 says the "far layer fades in over 250–300 m". Which layer counts as "far" in
   that wording? Should G5 pin reading R's gating (no Fire beyond 301 m on the M16A3)?
2. **AudioCurve X units** (metres assumed, plan §3.10 open), and whether `MinDistance` (10 on the M16A3 Cone) clamps
   the curve or offsets it.
3. **LPB0 damping formula** (`HFDamping*` → cutoff). A2 uses tunables. Is a reference recording available for A3?
4. **Sampler stepping.** Is "Fire first round, Step for each further round, stop event → tail" the right per-round
   event model for automatic fire, or does the game re-post `Fire` per round?
5. **Unresolved names** (#3650803780, #1114728387, #1365968171, #2484178491, #3520090073). They are bound by hash and
   left at their defaults in A2. Acceptable?
6. **Library split.** Data side in `bf3_core`, runtime in a new `bf3_audio` (§2) instead of "`bf3_audio` in
   `bf3_core`" (plan §3.10). OK?
