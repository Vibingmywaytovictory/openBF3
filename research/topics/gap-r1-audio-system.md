# Gap r1: the audio subsystem (sound-graph census, mixer architecture, VO, footsteps, ambience)

Date 2026-10-14. Gap slug `audio-system` (critic-r1.json). Scope: design BF3 multiplayer audio from the shipped data
and from measurements. All BF3 reads were read-only. All data and scripts are in
`S = C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\audio`.
Tags: **[V]** verified here (data dump, measurement or source read), **[RC]** report claim (web or another report),
**[I]** inferred.

Tooling [V]: `S\tool` is the probe `ebx-dump` (README in `S\tool\README.md`). It adds the audio commands `pdump`,
`idump`, `bundles`, `waves`, `lwaves` and `locscan` (`S\tool\src\audio.rs`). It also has `S\tool\src\xas.rs`, a 1:1 port of
the IW4L EA-XAS decoder (`C:\Users\olku\iw4L\crates\resource_bf3\src\sound.rs`). An earlier session of this run
(2026-10-10) built it and dumped `S\sound-parts.jsonl`, which holds every `sound/` partition of the MP set, 3,319
partitions in all. That session left no report. I re-ran or extended every step I cite below. My new outputs are in
`S\r2\`, and the analysis scripts are in `S\py\` (`census.py`, `plugins.py`, `summ.py`, `desc.py`, `descj.py`,
`waves.py`, `levelwaves.py`).

---

## 0. Answers in brief

1. **How the environment is selected for each shot. This was an open item in 01 §4 and is now solved [V].**
   - Each level has a client-side "ambient sound areas" schematic. Its `SoundAreaEntityData` volumes
     (`VolumeVectorShapeData` or `OBBData` shapes) output a 0..1 **`Proximity`** value.
   - `MathOpEntityData` (operator `Add`) sums those values per class.
   - The sums go into `MixerEntityData(sound/mixers/areatype_mixer)` on the inputs **Urban, Indoor_Garage, Urban_Alley,
     Desert, Forest** and a 6th class (index 6, indoor_small by its wave tables).
   - Inside the mixer, each input is accumulated with `Max` and weighted ×1.3/1.5/1.4/1.2/1.1/1.6. A
     `MinMaxValueSelector` then picks the arg-max against a constant 0.7 for "openfield".
   - The result is published as the mixer output **`AreaType`**, an integer 0..6.
   - 214 patches read `AreaType` through `MixerReceiveNodeData`. The weapons feed it into `WaveSwitcherNodeData.Index`
     for the noise, reflection and shell-casing layers.
   - So the class is a global, listener-side value. It is not chosen per shooter (§5).
2. **Graph interpreter or hand mixer: interpret [I, from data].**
   - 833 `SoundPatchAsset` hold 32,113 node instances of 46 node types.
   - The weapons alone need 32 types, and every major category needs 20–33.
   - About 25 of the types are trivial control-rate scalar or event operators.
   - The audio-rate side is already compiled in the data: `SoundGraphInfo.Voices[].Plugins[]` lists FourCC DSP
     plugins, and only 26 plugin kinds exist, 11 of them dominant (§2.3).
   - Hand-mixing 834 patches cannot reproduce the data's crossfade bands, randomisation and AreaType tables. IW4L's
     vehicle mixer stays as a fallback.
3. **Our own mixer or Godot nodes: our own C++ mixer [I, from Godot source and data].**
   - It would run as one custom `AudioStreamPlayback::_mix` (a GDExtension virtual, verified in the dev7 API) that
     feeds a few Godot buses.
   - BF3 needs: a global HDR loudness window computed from every voice before any gain is applied; 2–3 biquads per
     voice; cone and occlusion low-pass; a 2-slot convolution reverb; and loudness-priority culling.
   - Godot gives each playback 1 high-shelf "attenuation filter" and at most 6 buses. It has no convolution effect, and
     it has no global loudness pass (§9).
4. **Waves: keep XAS compressed in RAM and decode while streaming, per voice [V measurement].**
   - Decoding costs **0.117–0.120 ms per channel-second** on one core of a Ryzen 7 5800X, about 8,400× real time.
   - Fully decoded PCM16 would take 828 MiB (Metro TDM, non-VO) or 686 MiB (Caspian CQL), against 246 MiB and 204 MiB
     as XAS.
5. **Voice budget for 64 players [I + data].**
   - BF3's own budget mechanism is the HDR window, whose data is *WindowSize 45 dB, MinTop 80 dB* (§7.3), plus 33
     `SoundScopeData` instance strategies such as "Closest_5" and "Max_Instances_4".
   - Recommendation: 128 sampler voices at most, with a target of 64 audible. Pre-cull at window bottom − 3 dB and
     drop the quietest voices first (§11.3).

---

## 1. Census

### 1.1 Assets [V] (`S\sound-partitions.tsv`, 3,319 rows; MP/XP levels + Globals + UI)

| primary type | n | notes |
|---|---|---|
| SoundWaveAsset | 1,879 | 4,720.9 MiB of chunk data, 123,074 s, 173,693 channel-seconds. Channels: 1,205 mono, 357 stereo, 315 quad (4.0), 2 with 6 channels (`S\r2\waves-summary.txt`) |
| SoundPatchAsset | 833 | plus 1 `SoundMasterPatchAsset` (`sound/master`), giving the critic's 834 |
| SoundPatchConfigurationAsset | 462 | a patch plus a `Wave` asset parameter and an `Amplitude` value. Ambient areas mostly use `sound/levels/ambient_soundarea_player` this way |
| BigWorldSettingsAsset | 79 | random distant one-shots (`minDistance`/`maxDistance`, `min/maxDelayTimeInMinutes`) |
| MixerAsset | 19 | `areatype_mixer`, `ambienttype_mixer`, sound-state mixers, `fademixer`, … |
| ImpulseResponseAsset | 17 | `ir_indoor_*`, `ir_outdoor_*`. Chunk magic `irf1`, 2 channels, 48,000 Hz, 1,024 block size, 65–340 KB each (§6.3) |
| VoiceOverLogicAsset / VoiceOverSystemAsset | 7 / 1 | §8 |
| StreamPoolAsset | 3 | VO (6 streams × 1,000,000 B, Win32), GameSound (2 × 2,000,000 B), Music (1 × 2,000,000 B) |
| LevelAudioObstructionAsset | 4 | Office, Factory, Palace and Skybar only (XP2 maps) |
| SoundStateSettingsAsset, AudioSystemAsset, MixerSystemAsset | 1 each | §7 |

Other related data:
- 896 `SoundAreaEntityData` and 1,716 `SoundEffectEntityData` sit in level/object partitions (`S\soundarea.jsonl`).
- `SoundSettings` (default settings) contains only `AudioSystemUri "Sound/AudioSystem"` and empty VO paths.
- **No `AudioSettings` instance ships** (`S\r2\settings.jsonl`), so `maxAudibleSoundCount`, `audioCoreMixJobThreadCount`
  and the wave-cache sizes are code defaults that we cannot read [V].

### 1.2 Node types: counts, schema (VU-Docs) and DSP semantics

Counts come from `S\r2\census.txt` [V]: 32,217 node instances of 58 types, 32,113 of them inside SoundPatchAssets.
The critic's TSV (`critique-r1\sound-types.tsv`) gives 32,222, and SimpleTransform is 7,639 in both. Field lists are
from `C:\Users\olku\VU-Docs\types\fb\<Type>.yaml` (`S\node-schema.txt`). All ports are `AudioGraphNodePort
{UnconnectedValue, ValueIndex, IsConnected}`.

**The graph is a dataflow over a per-instance value array** [V structure; semantics I]:
- `SoundGraphData`/`MixerGraphData` declare a `ValueCount`. A port reads `value[ValueIndex]` when connected, and
  otherwise the constant `UnconnectedValue`.
- Outputs write the same array.
- Events are values that pulse (they are `AudioGraphEvent : AudioGraphParameter`).
- `ValueIndex 0` with `In/Out` = "v0" on DSP nodes marks an **audio-rate** node. Its audio path is not in the value
  array. It is in the compiled plugin chain that the node's `Plugin: SoundGraphPluginRef {VoiceIndex, PluginIndex}`
  points to.

| node (instances / patches) | inputs → outputs (fields) | semantics [I unless noted] |
|---|---|---|
| SimpleTransform 7,639/382 | x, y → z; `operation` (20 ops: Add…Pow…ArcTangent), `angleUnit` | z = x op y, control rate |
| ScaleClamp 5,096/477 | in, inMin, inMax, outMin, outMax → out | linear remap with clamp |
| Sampler 3,541/767 | wave or externalWave, variation, offset, delay, pitch, amplitude, enableStep, buffer, trigger, release, step → output (audio), finished, buffered, position; `basePitch`, `loop ∈ {LtNone, LtForward, LtRelease, LtInstantRelease}`, `shuffleSegments`, plugins {SndPlayer, Pause, Resample, Gain} | wave voice: picks a variation and segment, loops between `firstLoopSegment` and `lastLoopSegment`, plays the tail on release (`LtRelease`). `enableStep`/`step` step through segments, which is per-shot stepping for automatic fire |
| NumberGenerator 2,373/458 | trigger → y; min, max; mode RandomUniform | new random value on each trigger |
| MultiCrossfader 1,742/298 (+ groups) | control, start, stop, lockControlValue; group: fadeBegin, fadeEnd, fadeAmplitude, `FaderType ∈ {Linear, Cosine}` → amplitude, start, stop | amplitude bands over `control`. `control` is the listener distance in every patch read. `lockControlValue` freezes it at start, for one-shots |
| WaveSwitcher 1,298/253 | index, advance → wave (asset handle), indexChanged; `waves[]`, defaultIndex, isRandom | selects which wave asset a Sampler plays (AreaType, material, pose …) |
| SimpleValueSelector 1,059/211 | index → out; `outValues[]` | table lookup |
| ConeOutput 961/751 | cone (direction, inner/outer angle, outsideGain, hfDampingAngle), panSize, centre/LFE, reverbGain/send + OutputNodeData (minDistance, attenuationCurve, gain, hfDamping{Distance,Obstruction,Occlusion}, mainSend bus, enableHdr, transformSource) | positional 3D output with distance curve, directivity, HF damping and reverb send |
| FlatOutput 451/313, DirectOutput 198/196 | angle, reverb (Flat); none (Direct) | non-directional 3D output; unpanned output (LFE, ambience beds) |
| ParameterFilterLinear 848/176, ParameterFilterLp 4/4 | in, attackSpeed, releaseSpeed → out; in, hz → out | slew limiter; one-pole smoother |
| Receive 832/831 | entries {source: AudioGraphParameter/Event → out} | binds the patch's inputs (game parameters and events) to values |
| MixerReceive 218/217 | entries {source parameter, mixer asset → out} | reads a **global mixer output** (AreaType, …) |
| Lfo 748/282 | hz, amplitude → out; min, max, startAtRandomValue | pitch and gain wobble |
| HighPass/LowPass/BandPass/Peaking/HighShelf/LowShelf Iir2 (722/67/26/400/100/3) | in, frequency (+ q, amplitude, bandwidth) → out (audio) | biquads, plugins `HI20 LI20 BI20 PI20 HS20 LS20` |
| Condition 652/146, LogicalExpression 348/205, DeMux 224/131, Switcher 203/176, ValueSelector 133/122, EventSwitcher 6/4, EventGate 64/40 | comparison (`ConditionType` And…GreaterOrEqual), And/Or/Nand/Nor over events, event routing by value, cooldown gate | control logic |
| Gain 631/266, DistortionClip 8/3, Delay 62/48, Limiter 3/3, FrequencyShiftSsb 3/3 | audio | gain, clip, delay line, limiter, SSB shift |
| Adsr 532/236, Timer 172/102, Delta 55/39, SequenceGenerator 3/3, Crossfader2 36/36 | envelopes, timers, d/dt, counters, 2-way crossfade | control |
| Physics 472/464 | → distance, velocity, relativeVelocity, azimuth, elevationAngle | listener-relative kinematics. **Only `Distance` is connected** in all 5 patches read |
| DivisibleLoopPlayer 72/70 | start, stop, amplitude, freezeSegment, crossFadeLength | segment-crossfading loop player (ambience beds, engines) |
| CameraShake 89/89, PadRumble 51/51 | pitch, yaw, roll / high, low | **sound-driven camera shake and rumble**, sent to the game |
| Send 17/17, Route 13/5, Orientation 1/1, DialogSampler 5/5 | send to a parameter, route an audio path, listener-relative orientation, VO playback | |
| master/mixer only: Bus 10, MixerInput/Output 4/6, MinMaxValueSelector 4, MatrixPanner 4, MasterUnit, Compressor, Dac, IrReverb(+Controller), UserMusicArbiter, MixerValue | §7 | |

**Coverage** [V] (`S\r2\census.txt`):
- Patches have a median of 8 nodes, a p90 of 105 and a maximum of 242.
- Implementing types in greedy order gives these shares of patches fully covered: 17 types 52.2 %, 30 types 82.0 %,
  37 types 98.1 %, 46 types 100 %.
- Type sets needed per category: weapons/handheld 32, vehicles/land 32, vehicles/air 32, levels 33, character 30,
  bullet_craft 28, explosions 25, destruction 20.
- Not used by handheld weapons: Delta, DialogSampler, DistortionClip, DivisibleLoopPlayer, EventSwitcher,
  FrequencyShiftSsb, HighShelfIir2, Limiter, LowShelfIir2, Orientation, ParameterFilterLp, Route, Send,
  SequenceGenerator.

### 1.3 Compiled audio-rate plugins (`SoundGraphInfo`) [V] (`S\r2\plugins.txt`)

Each patch carries `Info {Voices[] {Plugins[] {Id FourCC, OutputChannelCount, ConnectionIndex, ConstructParams…},
ProcessingStage}, Connections[] (to master busses), PluginCount}`. Totals are 7,322 voices and 5,017 sound-player voices
in 834 graphs. Voices per patch: median 3, p90 27, maximum 38. Handheld weapons have a median of 26 voices, land
vehicles 22.5, air vehicles 34. The M16A3 has 30 voices and 152 plugins.

| FourCC | instances / patches | meaning [I from name, node `Plugin` refs and channel counts] |
|---|---|---|
| `Sen0` 9,782/834 | | send to a bus (master `GameMixMain`/`Reverb`/`LFE` …) |
| `Gai0` 5,648/834, `GaF0` 144 | | gain, gain fader |
| `SnP1` 5,017/833, `Pau0` 5,017, `Rsp0` 4,873/769 | | sound player (wave decode), pause, resampler (pitch). One set per Sampler voice |
| `Sub0` 2,305/541 | | submix (patch-internal bus) |
| `LPB0` 1,609/833, `VuM0` 1,609 | | output low-pass (distance, obstruction and occlusion HF damping); VU meter (feeds HDR loudness) |
| `Pn21` 1,412/819 | | 3D panner, mono/stereo → 4 or 6 channels (ConeOutput construct params 45°/135°/2.0) |
| `HI20 PI20 HS20 LI20 BI20 LS20` 722/400/99/67/25/3 | | biquads |
| `Del0` 62, `Rou0` 23, `DCl0` 8, `MtP0` 4, `Li10` 3, `FSS0` 3, `Uma0` 2, `Dac0` 1, `Co10` 1, `RIR1` 1 | | delay, route, distortion clip, matrix panner, limiter, frequency shift, user-music arbiter, DAC, compressor, IR reverb |

The game mix is **4.0** (busses GameMixMain and GameMixAlt have 4 channels), with separate LFE (1 channel), Dialog
(1 channel) and 6-channel DirectOut and Movies busses [V, §7.1].

---

## 2. The requested patches: inputs and structure

Data: `S\r2\summ.txt` and the full dumps `S\r2\<patch>.txt` [V]. Parameter and event names are hashed with
djb2-xor (h = h·33 ^ c, seed 5381; `S\py\sg.py`). I resolved them by dictionary search. Unresolved names stay `#hash`.

| patch | nodes (instances) | voices | loudness / radius / looping / doppler | inputs (Receive) | global inputs | distance bands (MultiCrossfader, m) |
|---|---|---|---|---|---|---|
| `sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02` | 107 (163) | 30 | 110 dB / 0.8 / true / 0 | params **IsFirstPerson** + 5 unresolved; events **Fire** (start), #1920007028 (stop), **Reload**, #2484178491, #1968287961 (sampler `enableStep`), #3520090073 | AreaType | 0–20 cos, 5–35, 250–300, 1–5 lin, 20–50, 3–15, 1–15, 1–17, 1–19 lin, 300–301 |
| `…/m98_b/weapon_sniper_m98_b_02` | 96 | 26 | 112 / 0.8 / true / 0 | IsFirstPerson + 4; events Fire, **Bolt**, **ReloadEnd**, Reload … | AreaType | 0–20, 1–10, 2–10, 5–35, 8–40, 30–60, 300–350, 300–351 |
| `…/rpg-7/weapon_at_rpg7` | 68 | 19 | 115 / 0.8 / true / 0 | same event set as M98B | AreaType | 1–15, 1–19, 5–20, 5–35, 5–40, 15–45, 45–90, 250–300, 300–301 |
| `sound/vehicles/land/m1a2_abrams/m1a2_abrams` | 219 | 30 | 96 / 6.0 / true / **1.0** | **RPM, Throttle, Velocity, Gear, Load, Tilt, Contact, Slip, Boost, Health** + 5; events Start/Stop | none | 15–50, 30–40, 40–60, 80–100, 90–120 |
| `sound/character/soldier_movement_footsteps` | 107 | 11 | 71 / 0.2 / false / 0 | **SoundMaterialId**, **Pose**, IsFirstPerson + 3; 14 events incl. **OnStart, OnStop, OnJump, OnLand** | AreaType | 0.5–0.6, 3–10, 13–15 |

Structure of the M16A3 [V] (`S\r2\m16-compact.txt`):
- 13 Samplers, each with `Pitch = NumberGenerator(1.21–1.262) × Lfo(0.96–1.0, 4 Hz)`.
- The automatic-fire core is a `Loop LtRelease` sampler stepped per round. `Fire` starts it, and the stop event
  releases it into the tail [I from fields].
- Noise close/distant/far, reflections close/far and shell casings use `WaveSwitcher.Index = AreaType`, 7 waves each
  (§5).
- Distance (`Physics.Distance` = v22) drives all 8 MultiCrossfaders, `HighPassIir2` frequency (`ScaleClamp`
  17–200 m → 0–310 Hz; 30–100 m → 0–80 Hz) and gains.
- `IsFirstPerson` swaps gains: `ScaleClamp(in 0..1 → 1..0)`.
- The `SimpleValueSelector`s on AreaType hold per-environment gains, for example `[1.0,1.15,1.4,1.0,1.0,0.9,1.1]`
  and delays `[1,3,2.5,2,1.2,0.8,3]`.
- Outputs:
  - "Cone": minDistance 10, curve 0 m 0 dB / 19.8 −17.9 / 54.6 −34.6 / 91.2 −41.1 / 120.4 −47.0 / 179 −67.1;
    HF damping 0.8; reverb −17 dB.
  - "Handling": −29 dB.
  - 2 × "Flat" with angle 75.
  - "LFE" Direct: +3 dB, `EnableHdr false`, to the LFE bus.
- The PORTING-RESEARCH §3.8 figure (107 nodes) and the 01 §4 figure (163 nodes) are both right: 107 node instances
  out of 163 partition instances [V].

The M1A2 is a parameter-driven engine model:
- 63 ScaleClamp, 69 SimpleTransform and 19 ParameterFilterLinear smoothers map RPM/Throttle/Load/Velocity onto 14
  samplers.
- It has Interior, Engine and "Chassi & Treads" outputs. The Interior Flat output falls to −82.6 dB at 10.4 m.
- It feeds `CameraShake` from sound.
- There is no AreaType input [V]. The IW4L hand mixer (handoff 04 §4.4,
  `C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\15-GODOT-HANDOFF\sections\04-gameplay-sim.md:257`) can be
  replaced by interpreting this patch with the 10 named parameters.

---

## 3. Footsteps and foley: the path from the material grid

Evidence [V]: `S\r2\soldier\characters__soldiers__mpsoldier.json`, `S\r2\matsound.jsonl` (Caspian) and
`S\r2\soldier_movement_footsteps.txt`.

- `characters/soldiers/mpsoldier` has a `SoundEntityData(sound/character/soldier_movement_footsteps)`. It is driven by
  property and event connections:
  - `SoldierSoundComponentData.SoundMaterialId`, `.Pose` and `.Transform` drive the matching inputs.
  - `PropertyCast → IsFirstPerson`.
  - The events OnJump and OnLand (and 2 unresolved) come from `SoldierSoundComponentData`.
  - `CompareFloatEntityData` emits OnStart and OnStop.
  - **4 `AntEventEntityData` events** come from the animation; these are the step triggers.
  - `EventGateEntityData` (2 events) also feeds it.
- `SoldierSoundComponentData`: `WalkVelocityThreshold 1.0`, `RunVelocityThreshold 5.5` (m/s), `WalkPeriod 0.3`,
  `RunPeriod 0.2` (s).
- `SoldierSoundData`: Movement = `soldier_movement_sounds_mp` (breathing, vault, pose change, swim), Death =
  `death_screams_us`, BulletImpact = `bullet_impact_1pplayer`, BreathControl.
- **`SoundMaterialId`** comes from the ground material's `MaterialPropertySoundData.MaterialSoundId`, a float class.
  Caspian's grid holds 20 such entries with ids 0–9 and 12.
- The patch switches waves on that id. The 16-entry table is: 0 dirt, 1 tarmac, 2 grass, 3 grass_tall, 4 gravel,
  5 metal, 6 vehicle_metal, 7 sand, 8 water, 9 wood, 10 forest, 11 water-on-tarmac, 12 wood, 13 snow, 14 ice, 15 glass.
  Walk and sprint variants are chosen by Pose and speed, and there are indoor variants.
- The patch also reads AreaType (`[0,0,0.8,0,0,0,1.8]`) for an indoor reverb-ish layer.
- Its distance bands are 0.5–0.6, 3–10 and 13–15 m. The Movement output curve reaches −45.4 dB at 28.8 m and
  −83.5 dB at 46.8 m, so footsteps are inaudible beyond about 45 m [V data; I interpretation].
- **Impacts and scrapes.** The Caspian grid has 313 `MaterialRelationSoundData` entries, keyed by material pair
  (`impactSound`, `scrapeSound`, `scrapeLength`).
  - 153 have no impact sound.
  - The others point to particle tails (dirt/rock/plastic/wood), `bullet_impact_generic`, `bullet_impact_metal`,
    melee hard/soft and `soldier_body_vs_vehicle`.
  - Bullet impacts use `Bullet_Impact_Scope` (DuplicateSpawn 1.0 m / 0.4 s, closest 5) [V].
  - This is the same grid lookup the damage code uses (handoff 03/04), so the material pair costs nothing extra.

---

## 4. Ambience

- **Areas** [V] (`S\r2\subway-areas.txt`, Metro `layer29_ambient_sound_areas_schematic`):
  - The schematic holds 48 SoundArea, 39 IrReverbEntityData, 30 FloatEntityData, 22 MathOp, 3 MixerEntityData and
    2 SoundEntityData.
  - Area shapes come through `LinkConnections` field 838548383 to `VolumeVectorShapeData` (76) or `OBBData` (12).
  - Areas carry `FadeWidth` (m), `FadeCurve` (Sine/Linear amplitude, LinearDecibel) and `ProximityMultiplier`. On
    Seine, 67 of 85 areas use 1.0 m / 0.8.
  - The area's sound is a `SoundPatchConfigurationAsset` on `ambient_soundarea_player`: a `DivisibleLoopPlayer` with a
    0.5 s crossfade, HPF 175 Hz, `DirectOutput` and HDR enabled.
- **Ambient-type mixer**: `MixerEntity(ambienttype_mixer)` takes MathOp sums on 3–4 inputs and outputs 3 values. Its
  output feeds back into the areas' amplitude (`MixerEntity → SoundArea` field 1947336347), which crossfades the beds.
  The input and output names are unresolved [V wiring; names not determined].
- **Convolution reverb zones**: `SoundArea.Proximity → IrReverbEntityData.Volume`.
  - On Metro, 39 entities, mostly `ir_indoor_big_room`, with gain −15 to −30 dB.
  - On Seine, 85: big_room 70, outdoor_downtown 9, outdoor_alley 6.
  - The master `IrReverbNode` crossfades 2 IR slots (`Reverb0/1`, `Amplitude0/1`, `MaxReverbLength 2.0` s) into the
    `Reverb` bus.
- **BigWorld**: 79 `BigWorldSettingsAsset` play random distant one-shots (e.g. MP_12 industry, XP5 river) under
  `Big_world_scope` (max 4 instances).
- **Scopes (voice limiting)** [V] (`S\r2\audiosystem.txt`): 33 scopes.
  - Examples: Bullet_Flyby_Standard closest 5; Bullet_Flyby_Important closest 2; Explosion closest 10; Bullet_Impact
    DuplicateSpawn 1 m / 0.4 s / closest 5; Glass_Debris and Destruction_small DuplicateSpawn 4 m / 2 s; Death_Scream
    max 4; Big_world max 4; UI latest 1; most ambient and physics scopes closest 5.
  - 811 of 1,295 patches and configurations have **no scope**, including all handheld weapons and the footsteps
    patch. Their only limit is the HDR culling [V].
- `LevelAudioObstructionAsset` exists only on 4 XP2 close-quarters maps. Its fields are a material map
  `{obstructionFrequency, gainReduction}`, frequency and gain slew rates, and `maxRaycastDistanceSquared`. Every
  output node has `HFDampingObstruction`/`Occlusion` coefficients (§2). So obstruction is ray-cast HF damping, used
  sparingly [V data; I mechanism].

---

## 5. How environment selection works, in detail (the 01 §4 open item)

Evidence [V]:
- `S\r2\areatype_mixer.txt` and the property connections in `levels/mp_subway/mp_subway` LevelData
  (`S\r2\subway\levels__mp_subway__mp_subway.json`, 310 connections touch the schematic).
- Re-verified on Seine (`S\r2\seine\*`).

```
SoundAreaEntity(shape, FadeWidth, FadeCurve, ProximityMultiplier) --Proximity-->
  MathOpEntity(Add over In0..In34, plus FloatEntity terms) --Out--> MixerEntity(areatype_mixer).<Class>
areatype_mixer: inputs accumulate MAX over contributors; v_c = input_c * w_c
   w = {Indoor_Garage 1.5, idx6 1.6, Urban_Alley 1.4, Desert 1.2, Forest 1.1, Urban 1.3}
   AreaType = argmax([0.7(openfield), Urban, Indoor_Garage, Urban_Alley, Desert, Forest, idx6])  -> output "AreaType"
patch: MixerReceive(areatype_mixer.AreaType) -> WaveSwitcher.Index / SimpleValueSelector.Index
```

| AreaType | class (input name hash resolved) | M16A3 noise / reflection wave |
|---|---|---|
| 0 | openfield (constant 0.7, the default) | `noise_*_openfield`, `openfield_reflections_far` |
| 1 | Urban (219107215) ×1.3 | `noise_*_urban` |
| 2 | Indoor_Garage ×1.5 | `noise_*_indoor_garage` |
| 3 | Urban_Alley ×1.4 | `noise_*_urban_alley` |
| 4 | Desert ×1.2 | noise uses openfield; `reflection_*_desert` |
| 5 | Forest ×1.1 | `noise_*_forest` |
| 6 | hash 2174540754 ×1.6 (name unresolved) | `noise_*_indoor_small` → indoor_small [I] |

- The class is **one global value per client**, evaluated at the **listener**. The `MixerEntity` and every area
  entity are `Realm_Client`.
- It is not evaluated at the shooter. A shot 200 m away in a forest still uses the listener's alley tables when the
  listener stands in an alley [V wiring; I consequence].
- The `IsFirstPerson` and distance crossfades then decide which layers are heard. A port therefore needs:
  1. the area volumes and their proximity function (point-in-polygon-prism or OBB plus a fade band);
  2. the MathOp/Float logic (FloatEntity values change on destruction events, `IncDecValue` 0.33);
  3. one mixer evaluation per audio tick.

Metro wires Urban, Indoor_Garage, Urban_Alley and idx6, and Seine wires the same set. Desert and Forest are fed only
on levels whose schematics use them (not checked level by level).

---

## 6. Mixer architecture as shipped

### 6.1 Master patch [V] (`S\r2\master.txt`)

- 10 busses:

  | bus | channels |
  |---|---|
  | GameMixMain | 4 |
  | Reverb | 2 |
  | GameMixAlt | 4 |
  | DirectOut | 6 |
  | Dialog | 1 |
  | LFE | 1 |
  | Music | 4 |
  | Movies | 6 (also `RwMovieBus`) |
  | LoadingMusic | 2 |
  | NarrVoice | 4 |

- `IrReverb` with 2 slots and a 2.0 s maximum.
- `MasterUnit` has 5 presets selected by `SettingsIndex`, each with a compressor, an EQ, parallel distortion and
  gains:

  | preset | compressor threshold, ratio | distortion clip | parallel distortion gain |
  |---|---|---|---|
  | **Wartapes** | −17.9 dB, 14.6:1, attack 30 / release 90 | −6 | −10 |
  | TV | −12.3 dB, 11.9:1 | 0 | −5 |
  | **HiFi (default)** | −12.3 dB, 11.9:1 | 0 | −10 |
  | Home Cinema (flat) | −12.3 dB, 11.9:1 | 0 | −96 |
  | Headphones | −12.3 dB, 11.9:1 | 0 | −10 |

- Then `MatrixPanner` (LFE), `UserMusicArbiter` and `Dac` (48,000 Hz).
- `AudioSystemAsset`: `SampleRate 48000`, `DopplerFactor 0.5`, 8 languages, 3 stream pools, 33 scopes.
- GDC 2009 (§10) says "LFE and dialog always bypass" the realtime mastering [RC]. This matches the separate busses [V].

### 6.2 Mix groups and sound states [V]

- `sound/mixersystem` has 88 `MixGroup`s in a tree: All → Game/World/Music/UI/VO/VIP → Weapons/Handheld/Silenced,
  Explosions, Footsteps, … Every patch has a `MixGroup`.
- `MixerPreset.groups` (`MixerPresetGroupData {group, state Normal/Mute/Solo, attackTime, releaseTime, properties}`)
  give **snapshot ducking per group**, used by sound states.
- `sound/soundstates` (`SoundStateSettingsAsset`, ImpairedHearing threshold 300, Scream 200, Suppression 0.1) defines
  14 states, each with `LpCutoffFrequency`, fades, an optional bypass sound and an optional mixer:

  | state | low-pass cutoff | other |
  |---|---|---|
  | Default | 24 kHz | carries **the only `HdrSetting`** |
  | Menu | 600 Hz | |
  | LowHealth | 4.5 kHz | |
  | Supression | — | mixer only |
  | ManDown | 950 Hz | |
  | Dead | 950 Hz | |
  | Flashbang | 50 Hz | 5 s |
  | ImpairedHearing | 90 Hz | 1.4 s |
  | InsideCarLP | 1 kHz | |
  | EOR | 300 Hz | |
  | Leaving_Area | 2.5 kHz | |
  | ScriptedLowPass, COOP_Menu, SP_Fail | | |

### 6.3 HDR (the loudness window) [V data] + [RC algorithm]

- `HdrSetting`:
  - `WindowSize 45` dB, `WindowMinTop 80`, `WindowMinBottom 35`.
  - Top attack 0.05 s; top release from `WindowTopMinReleaseTime 0.035` to `WindowTopMaxReleaseTime 0.85` s, shaped by
    a spline (`WindowTopReleaseTimeCurve`).
  - `WindowBottomAttackTime 0`, `WindowBottomReleaseTime 0.025`.
  - `Headroom 5`, `AllowedOvershoot 3`, `CompressFactor 0.5`, `DischargeFactor 3.35`, `MaxAllowedEnergy 110`.
- Patch `Loudness` (dB SPL at the reference distance):

  | category | median | range |
  |---|---|---|
  | handheld weapons | 107 | 75–116 |
  | explosions | 112 | up to 130 |
  | land vehicles | 90 | — |
  | air vehicles | 100 | — |
  | ambient levels | 65 | — |
  | footsteps | 71 | — |
  | bullet craft | — | up to 135 |

- Output nodes: 942 Cone and 423 Flat outputs have `EnableHdr true`. The 193 Direct outputs (LFE, beds) are mostly
  `false`.
- Algorithm (Clerwall, GDC 2009 slides, which I extracted to `S\web\hdr.txt`) [RC]:
  - Specify loudness in dB SPL.
  - "Measure each sound at the listener position" (distance and angle attenuation).
  - The window is "Defined by MinTop, Size, ReleaseTime", and the "Loudest sound can move the window".
  - Amplitudes are computed relative to the window, with headroom applied.
  - **Pre-culling** uses the "Previous update's HDR window bottom" as the cutoff, after "Subtract 3dB" for the
    peak/RMS relationship. One-shots below it are dropped. Loops become pending.
  - **Active sounds** that turn inaudible are suspended; loops resume.
  - Base priority (None/Low/Medium/High/Permanent) is combined with loudness. When the maximum render time is
    reached, the quietest voices are expelled.
  - Mastering: EQ and compressor per listening setup.
- The 2008 DICE "Adaptive Mixing in Frostbite" deck adds that HDR "functions as a culling algorithm", "Priority is set
  according to loudness" and it "does not touch the actual audio waveform" [RC,
  https://www.slideshare.net/slideshow/adaptive-mixing-in-frostbite/3128152].
- **The BF3 fields `CompressFactor`, `DischargeFactor` and `MaxAllowedEnergy` go beyond the 2009 description.** Their
  exact formulas are not public [could not determine]. Implement MinTop, Size, attack, release and headroom first.

---

## 7. Material grid → impacts and footsteps

See §3 for the details. Implementation:

1. The material-pair lookup the bullets already use returns a `MaterialRelationSoundData`. Spawn its `ImpactSound`
   patch under the patch's scope.
2. The soldier's ground material (`footMaterialPair`, handoff 04:478) gives the
   `MaterialPropertySoundData.MaterialSoundId` float for `SoundMaterialId`.
3. Ant footstep events (03-animation), or a velocity-period fallback (walk 0.3 s above 1.0 m/s, run 0.2 s above
   5.5 m/s), fire the footstep events.

---

## 8. Voice-over: Loc/en and the VO logic

- `C:\Program Files\EA Games\Battlefield 3\Data\Win32\Loc\en.toc` (210,614 B) and `en.sb` (1,209,346,277 B) form a
  **chunk-only superbundle with no bundles and therefore no ebx or res** [V] (`S\loc-en-bundles.tsv` is empty;
  `locscan` in `S\r2\loc-dec.tsv`).
  - 4,285 chunks cover the whole `.sb`.
  - 4,280 chunks are EA-XAS sound streams; 5 chunks do not start with a sound header [V; content not determined].
  - Rates and channels: 3,019 chunks at 48 kHz mono, 1,223 at **16 kHz mono** (radio/comrose quality [I]), and a few
    4/6/8-channel ones (cinematics).
  - 15,806 streams, 36,308 s = **10.09 h** of audio.
  - Full decode with timing: 4,790 ms for 40,096 channel-seconds, **0.1195 ms per channel-second**, 0 frame
    mismatches and 0 errors.
- The VO `SoundWaveAsset`s live in the level bundles. MP data has 393 VO waves:
  - en/mp/self 70, en/mp/comrose 44, en/mp/autotriggers 32, en/mp/gamemode 30, en/sp/sp00 112;
  - `common/ru/*` 99 (Russian soldiers speak Russian in every language) [V names; I purpose].
- Each VO wave carries `Localization[] {language, firstVariationIndex, variationCount}` for 8 languages, with 40,944
  variations in total. Their chunks resolve into `Loc/en` (389 waves hit 389 distinct chunks).
  - This install contains only `en`. Other languages' chunks are absent, which explains the `vo_ru…` "chunk … in no
    mounted bundle" errors when `Loc` is not mounted [V].
  - Waves are mapped to languages through `SoundWaveLocalizationInfo` [V schema and data].
- VO streams through `StreamPool VO` (6 streams × 1 MB on Win32) [V].
- **VO logic** [V counts] (`S\sound-parts.jsonl`; schema VU-Docs `VoiceOver*.yaml`):

  | asset | instances | conversation nodes | content |
  |---|---|---|---|
  | `comrose` | 4,732 | 197 | |
  | `autotriggers_mp` | 1,798 | 80 | 14 `VoiceOverEvent`s: OutOfAmmo, SoldierReloading, SoldierSuppressed, ManDown, IncomingGrenade, SoldierFire, RadioChatter, LeavingArea, ExplosionClose, SoldierHit, C4Placing, C4Detonating, DefibrillatorInitiated, DefibrillatorAccepted |
  | `gm_conquest` | 257 | 14 | events ObjectiveTaken/Lost, RoundWonTeam1/2, EndgameWinningTeam1/2 |
  | `gm_rush` | 355 | — | |
  | `gm_sdm` | 1,592 | — | |
  | `gm_generic` | 193 | — | |
  | `vo_system` | 172 | — | 59 labels, 21 intervals, 15 conversation queue groups, 27 global constants |

  - The graphs are value-connection node networks (`VoiceOverValueConnection`, `…InfoNode`, `…CompareNode`,
    `…ConstantNode`, `…ConversationNode`/`DialogClip/Track/Group`).
  - VO is played through 5 patches: `vo_radio_mp(_self)`, `vo_talk_mp/_self` and a prototype; these contain the 5
    `DialogSamplerNodeData`.
  - `mpsoldier` references `comrose` 34 times and `autotriggers_mp` 3 times.

---

## 9. Waves: memory and decode cost (Metro TDM, Caspian CQL)

Per-level totals come from `S\py\levelwaves.py`, giving `S\r2\levelwaves.txt` [V]. They count the ebx in the level
bundle sets and the wave headers. PCM16 is computed at 48 kHz × channels.

| level set | waves | non-VO resident (no stream pool) XAS → PCM16 | streamed (GameSound/Music pool) | English VO XAS → PCM16 |
|---|---|---|---|---|
| Metro TDM (main + TDM bundles) | 908 | 606 waves, **245.7 MiB → 827.9 MiB** | 21, 16.0 → 53.9 MiB | 281, 491.0 MiB, 17,250 s → 1,579 MiB |
| Metro CQL | 911 | 609, 247.2 → 832.7 | 21, 16.0 → 53.9 | 281, 491.0 → 1,579 |
| Caspian CQL | 960 | 697, **203.6 → 686.0 MiB** | 22, 13.0 → 43.7 | 241, 448.6 → 1,437 |
| Caspian TDM | 864 | 591, 177.5 → 598.3 | 22, 13.0 → 43.7 | 251, 459.6 → 1,474 |

- XAS compresses 3.37:1 against PCM16. Metro's main bundle also ships co-op and MP15 waves, for example
  `coop_03_hostage` at 33.5 MiB, which inflates its totals [V].
- **Decode cost** [V] (`ebx-dump waves … every:3` on Metro and `locscan` on Loc/en, Ryzen 7 5800X, single thread,
  release build):

  | data | time | channel-seconds | ms per channel-second | frame mismatches |
  |---|---|---|---|---|
  | Metro (every 3rd wave, 91.98 MB) | 377 ms | — | 0.1171 | 0 |
  | Loc/en (all) | 4,790 ms | 40,096 | 0.1195 | 0 |

  - That is about 2.4 ns per sample per channel.
  - 128 mono voices decoding in real time cost 128 × 0.12 ms = **15 ms per second, 1.5 % of one core**.

**Decision** [I]:
- Keep the level's XAS chunk bytes resident: about 250 MiB, read once from the install into RAM. The raw `.sb` reads
  could also be mapped instead.
- Decode per voice inside the mixer, in 128-sample XAS frames (one 76-byte frame per channel).
- Stream VO and the stream-pool waves from disk on demand.
- No PCM cache is needed. An optional `user://` cache keyed by content digest stays possible for hot one-shots, but
  measurement says it is unnecessary.

---

## 10. Godot 4.8 dev7: what the engine gives us

Verified in the source at commit c971f93e7
(`…\scratchpad\godot-engine\godot-src`) and in `research\godot-api\extension_api.json` (header `4.8 dev7 official`).

| fact | evidence |
|---|---|
| Virtuals exist: `AudioStream._instantiate_playback`, `AudioStreamPlayback._mix(AudioFrame*, float rate_scale, int frames) -> int`, `_start/_stop/_is_playing/_get_playback_position/_set_parameter`, `AudioStreamPlaybackResampled._mix_resampled`/`_get_stream_sampling_rate` (+ `begin_resample`), `AudioEffect._instantiate`, `AudioEffectInstance._process(const AudioFrame*, AudioFrame*, int)`/`_process_silence` | [V] API dump; `scene/resources/audio/audio_stream.h:83-92,146-147,172` |
| `AudioStreamWAV.Format` = 8-bit, 16-bit, IMA-ADPCM, QOA. No EA-XAS | [V] API dump; `scene/resources/audio/audio_stream_wav.h:100-103` |
| **The mix block is hard-coded to 512 frames** ("limits audio latency to an absolute minimum of 11ms") | [V] `servers/audio/audio_server.cpp:1336-1338` |
| Default `audio/driver/output_latency` 15 ms; default `audio/driver/mix_rate` 44,100 (BF3 data is 48,000, so set 48,000) | [V] `main/main.cpp:2843`, `servers/audio/audio_driver.h:167` |
| Per playback: at most **6 buses** (`MAX_BUSES_PER_PLAYBACK`); at most 4 stereo pairs per bus (`MAX_CHANNELS_PER_BUS`); a 64-frame look-ahead for fade-outs | [V] `servers/audio/audio_server_constants.h:36-38`, `audio_server.cpp:175-222` |
| Per-playback DSP is one high-shelf "attenuation filter" per channel pair ("hardcoded effects … to support 3D audio players") | [V] `audio_server.cpp:457-470` |
| `AudioStreamPlayer3D`: each node calls `intersect_point` (up to 32 areas) on every physics tick for reverb buses, and computes listener volumes and doppler. Properties include `max_polyphony`, `attenuation_model`, `unit_size`, `max_distance`, `emission_angle_*`, `attenuation_filter_*`, `panning_strength`, `doppler_tracking` | [V] `scene/3d/audio_stream_player_3d.cpp:283-331,453-580`; `.h:67` |
| `Area3D` has `reverb_bus_*` (algorithmic reverb amount/uniformity); `AudioEffectReverb`, `Filter`, `EQ`, `Compressor`, `Limiter`, `HardLimiter` exist; **no convolution reverb class** | [V] API dump (`AudioEffectConvolutionReverb` absent) |
| `AudioStreamGenerator`/`push_buffer`, `AudioStreamPolyphonic`/`play_stream`, `AudioStreamSynchronized`/`Interactive`/`Randomizer` exist | [V] API dump |

**Cost and latency estimate** [I]:
- **Own mixer.** One `AudioStreamPlayback` subclass on one `AudioStreamPlayer` (non-positional), outputting 4.0 or
  stereo into a "Game" bus plus a separate LFE/dialog path.
  - Per sampler voice per output sample: XAS decode about 2.4 ns [V], resample (linear or cubic) about 3–5 ns, 2–3
    biquads about 2 ns each, 4-channel pan and gain about 1 ns. That is about 12–15 ns.
  - 128 voices × 48,000 = 6.1 M voice-samples/s, about 75–90 ms CPU per second: **7.5–9 % of one core on the audio
    thread**.
  - The control-rate graph at 93.75 Hz (one evaluation per 512-frame block) or at 187.5 Hz (256 frames) is
    negligible: 107 scalar nodes × 64 patches × 188 Hz ≈ 1.3 M node evaluations/s.
  - Two partitioned FFT convolutions of ≤ 0.7 s stereo IRs (block 1,024, matching the `irf1` header [I]) cost about
    1–2 % of a core [I].
  - Latency is the 512-frame block (10.7 ms at 48 kHz) + `output_latency` 15 ms + up to 1 game tick for parameter
    hand-off ≈ **26–43 ms**. That is acceptable for gunfire. 1p fire should be triggered on the client's predicted
    shot.
- **N × `AudioStreamPlayer3D`.** One node per sampler voice gives 128 nodes, each doing a physics-tick
  `intersect_point` and volume computation on the main thread.
  - It would still need an XAS `AudioStream` subclass.
  - It cannot apply a global HDR window before gain, per-voice HPF/peaking/LPF chains, cones with HF damping
    coefficients, a 2-slot IR reverb, AreaType switching or sample-accurate segment stepping.
  - Reproducing BF3 would need custom `AudioEffect`s per bus, and buses are capped at 6 per playback.
  - Verdict: use it only for UI/menu sounds.
- **Measurement still owed.** A microbenchmark of the C++ voice loop and a 64-bot Metro capture (slice A3 below).
  These are estimates, not measured.

---

## 11. Recommendations (with numbers)

### 11.1 Graph interpreter, not hand mixing

- Build `bf3_audio` in `bf3_core`, Godot-free and testable headless. It compiles each `SoundPatchAsset` once into:
  - a control program: a topologically ordered node list over a float register file of `ValueCount` entries, plus
    event pulses;
  - an audio plan: one entry per `SoundGraphInfo.Voices[i]`, holding the ordered FourCC plugin chain and its bus
    connections.
- Cache the result in `user://` keyed by partition digest (D-rule).
- Node order of work:

  | phase | node types | coverage |
  |---|---|---|
  | (a) M2a weapons | the 32 handheld-weapon types | 100 % of 164 weapon patches |
  | (b) M5 vehicles | + DivisibleLoopPlayer, Delta, HighShelf, Route | |
  | (c) M7 | the rest (Send, EventSwitcher, SequenceGenerator, Limiter, LowShelf, FrequencyShiftSsb, DistortionClip, Orientation, ParameterFilterLp, DialogSampler) | |

  In greedy order, 37 types cover 98.1 % of all patches.
- `CameraShake` and `PadRumble` emit to the game (camera) and are not ignored.
- The game supplies:

  | source | values |
  |---|---|
  | weapons | `IsFirstPerson`, Fire start/stop, Reload/ReloadEnd/Bolt, step event per round |
  | soldier | `SoundMaterialId`, `Pose`, OnStart/OnStop/OnJump/OnLand + Ant step events |
  | vehicles | RPM/Throttle/Load/Velocity/Gear/Tilt/Contact/Slip/Boost/Health, Start/Stop |
  | global | AreaType (from areas), sound-state mixers |

  Unresolved hashes keep their numeric hash. The interpreter binds by hash, so names are not required.

### 11.2 One C++ mixer in a custom `AudioStreamPlayback`

- It contains the HDR window, per-voice plugin chains (SnP1 → Rsp0 → biquads → Gai0 → LPB0 → Pn21 → Sen0), the 10
  master busses collapsed onto 4.0 or stereo plus a reverb send, the 2-slot partitioned convolution (`irf1` IRs), the
  MasterUnit preset (default HiFi) and the sound-state low-pass and mixers.
- Godot buses are used only for the final device output and for user volume sliders.
- The mixer runs in `_mix` on Godot's audio thread. The game thread posts parameters and events into a lock-free ring
  once per tick.

### 11.3 Voice budget and culling for 64 players

- Hard cap 128 sampler voices, target ≤ 64 audible [I].
- Order of culling:
  1. pre-cull at the previous window bottom − 3 dB (GDC09);
  2. per-scope strategies from the data (Closest_N, Max_N, DuplicateSpawn);
  3. when over the cap, drop the voices with the lowest loudness × priority;
  4. loops become pending, one-shots are dropped.
- Worked example [I, using the data]:
  - The local M16A3 is 110 dB with its "Cone" output at 0 dB, so the window top is ≥ 110 and the bottom is ≤ 65 dB.
  - A remote M16 at 54.6 m (−34.6 dB → 75.4 dB) is audible.
  - At 120.4 m (−47.0 → 63 dB) it is culled while you fire. It returns about 0.035–0.85 s after you stop, as the top
    releases toward MinTop 80, which puts the bottom at 35.
  - Footsteps at 71 dB fall below 65 dB beyond about 6 m while you fire, and below 35 dB at 30–40 m when quiet.
- Server: dummy audio, no audio work. Bots need audio only as `aiLoudness` (−1 in weapon patches, so the AI uses
  other data) [V].

### 11.4 Footsteps and impacts

Use the material grid as in §3/§7. This needs no new data structures beyond the grid and the soldier's ground
material.

### 11.5 Voice-over

- Phase 1: play VO waves by event, mapping autotrigger and game-mode event names to waves through the
  `VoiceOverLogicAsset` → `DialogClip` → wave references, with radio patches and 16 kHz chunks.
- Phase 2: interpret the VO logic graphs (value connections, compare/constant/global nodes, queue groups, intervals).
- Stream from `Loc/en.sb` via the VO pool. Missing languages fall back to `en`.

### 11.6 Slices to add to PORTING-RESEARCH §6

| slice | milestone | content | exit test |
|---|---|---|---|
| A1 XAS voice | M2a | XAS decoder in `bf3_core` (port of `S\tool\src\xas.rs`), resident chunk store, `AudioStreamPlayback` mixer with N sampler voices, pan, gain | decoded frame counts equal header frames for all 908 Metro waves; ≤ 0.2 ms per channel-second |
| A2 Patch interpreter (weapons) | M2a | 32 node types, Fire/stop/step events, distance crossfades, AreaType input stubbed to 0 | §12 oracles 1–4 |
| A3 HDR + culling + measurement | M2b | `HdrSetting` window, pre-cull, scopes, 128-voice cap; profiler counters | 64 bots firing on Metro: audio thread < 10 % of a core, no xruns at a 512-frame block |
| A4 Areas + AreaType + IR reverb | M3 | area volumes and proximity, MathOp/Float, mixer graph evaluator, `irf1` convolution | Metro and Seine: AreaType and active IR match §12 oracle 5 at chosen positions |
| A5 Footsteps/impacts | M3 | material grid → SoundMaterialId; impact sounds per pair | oracle 6 |
| A6 Vehicles via graphs | M5 | the M1A2 patch with 10 game parameters (replaces the IW4L hand mixer) | RPM sweep reproduces the crossfade order; doppler factor 1.0 |
| A7 VO + sound states | M7 | autotrigger and game-mode VO, comrose, sound states (ManDown, Flashbang, Suppression), MasterUnit presets | events produce lines; Flashbang LPF 50 Hz for 5 s |

---

## 12. Test oracles (from the data)

1. M16A3 close layer: amplitude 1 at 0 m falls with a cosine to 0 at 20 m. The far layer fades in over 250–300 m.
   The handling layers fade over 1–15, 1–17 and 1–19 m. Source: the groups in `S\r2\m16-compact.txt`.
2. M16A3 pitch is uniform in [1.21, 1.262] × LFO [0.96, 1.0] at 4 Hz. The HPF on the distant layer is
   `ScaleClamp(dist 17..200 → 0..310 Hz)`.
3. M98B tail band 300–350 m; RPG-7 45–90 m; M1A2 engine bands 15–50 and 80–100 m.
4. "Cone" attenuation at 19.8 / 54.6 / 91.2 / 120.4 / 179 m is −17.9 / −34.6 / −41.1 / −47.0 / −67.1 dB (spline). The
   curve's X unit is assumed to be metres [I].
5. AreaType equals the arg-max of `[0.7, 1.3·U, 1.5·IG, 1.4·UA, 1.2·D, 1.1·F, 1.6·idx6]`, where each input is the
   max-accumulated MathOp sum of `Proximity`. On Metro, inside an `ir_indoor_garage` zone, the expected AreaType is 2,
   the shells are `shellcasings_762_indoor`, and IR gain is −20 dB.
6. Footsteps: SoundMaterialId 8 selects `walk_water`/`run_water`, 13 selects snow, and 15 selects glass. Walk and run
   switch at 1.0 / 5.5 m/s with periods 0.3 / 0.2 s.
7. HDR: with only a 71 dB footstep source, the window sits at top 80 / bottom 35. After a 110 dB shot the top attacks
   to 110 in 0.05 s. A 63 dB source is then suspended.

---

## 13. Second-source checks of the 5 most load-bearing claims

1. **AreaType mechanism**: traced on Metro (`layer29_ambient_sound_areas_schematic` + LevelData connections), then
   independently on **Seine** (`levels/mp_011`, 85 areas, same mixer inputs Urban/Indoor_Garage/Urban_Alley/idx6, IR
   zones). The index order was checked against the M16A3 wave tables. **Confirmed.**
2. **XAS decode cost**: Metro sample, 0.1171 ms per channel-second; full Loc/en, 0.1195 ms per channel-second. These
   are different data sets with 0 frame mismatches against header frame counts. **Confirmed.**
3. **Node census**: my count (32,217 instances, 58 types, SimpleTransform 7,639) agrees with the critic's independent
   `sound-types.tsv` (32,222; SimpleTransform 7,639). The 5-instance difference comes from scope (master and mixers).
   **Confirmed.**
4. **Godot limits** (512-frame block, 6 buses per playback, no XAS, no convolution): read in the dev7 source
   **and** in the dev7 `extension_api.json`, which are two artefacts of the same build. **Confirmed for dev7**. They
   may change before 4.8 stable [I].
5. **HDR model**: the shipped `HdrSetting` (Size, MinTop, attack and release fields) matches the parameters named in
   the Clerwall GDC09 slides ("MinTop, Size, ReleaseTime", pre-cull on the previous bottom) and the DICE 2008 "Adaptive
   Mixing" deck (culling by loudness). **Consistent**. The extra BF3 fields are unexplained.

---

## 14. Could not determine

- About 150 parameter and event names, e.g. the weapon parameters #45329532 and #3650803780, the stop event
  #1920007028, the footstep events, the ambienttype names and AreaType index 6. They are hash-only. Binding by hash
  works without them. The only source would be bf3.exe strings, which I did not use.
- The formulas for `CompressFactor`, `DischargeFactor`, `MaxAllowedEnergy`, `AllowedOvershoot` and the window-top
  release curve.
- Engine defaults that are not in the data: `AudioSettings.maxAudibleSoundCount`, the mix-job thread count and the
  wave-cache size.
- The `irf1` IR chunk layout beyond its header (version 1, 2 channels, 0x7677 = 30,327 [frames?], 48,000 Hz, 1,024,
  0x1C0, 0x380). I did not decode it.
- The units of `AudioCurve` X (metres or scaled by MinDistance), and the exact Sampler segment-stepping semantics.
  These need a reference listen or VU documentation.
- The ambient-type mixer input and output names, and whether Desert or Forest are fed on any shipped MP level. Only
  Metro and Seine were checked.
- What drives `FloatEntityData` in the area schematic. The `ReferenceObjectData` events (3 ids) are probably
  destruction or state changes, but I did not trace them.
- Which non-sound content the 5 non-sound chunks in Loc/en hold.
- Real voice counts in BF3 at 64 players. There is no capture; the budget is an estimate. The C++ mixer cost is
  estimated, not measured.
- GDC talks by Ben Minto or Stefan Strandberg specific to BF3 audio tech. My searches found only interviews and
  awards (Designing Sound 2011, Audio Technology 2012, MCV). The GDC Vault index was not searched exhaustively.

## Sources

- Data and measurements: `S\r2\*` (census.txt, plugins.txt, summ.txt, levelwaves.txt, waves-summary.txt, master.txt,
  soundstates.txt, audiosystem.txt, areatype_mixer.txt, subway-areas.txt, seine\, soldier\, matsound.jsonl,
  loc-dec.tsv, waves-subway-dec.tsv), `S\sound-parts.jsonl`, `S\soundarea.jsonl`, `S\waves-mp.tsv`,
  `S\loc-en-chunks.tsv`; schema `C:\Users\olku\VU-Docs\types\fb\*.yaml`.
- Godot: `…\scratchpad\godot-engine\godot-src` @ c971f93e7 (files and lines above);
  `C:\Users\olku\BF3 reimplementation\openBF3\research\godot-api\extension_api.json`.
- Handoff: `C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\15-GODOT-HANDOFF\sections\02-resources.md:317`
  (XAS format), `04-gameplay-sim.md:257` (vehicle audio), `04-gameplay-sim.md:478,482` (unknowns).
- Web [RC]:
  - A. Clerwall, "How High Dynamic Range Audio Makes Battlefield: Bad Company Go BOOM", GDC 2009,
    https://media.gdcvault.com/gdc09/slides/GDC09_Clerwall_HDRAudio.pdf (text in `S\web\hdr.txt`).
  - DICE, "Adaptive Mixing in Frostbite", https://www.slideshare.net/slideshow/adaptive-mixing-in-frostbite/3128152.
  - Frostbite news page (https://www.ea.com/frostbite/news/how-hdr-audio-makes-battlefield-bad-company-go-boom,
    HTTP 404 at fetch time; listed by search).
  - Designing Sound, "Stefan Strandberg on the sound of Battlefield 3",
    https://designingsound.org/2011/03/03/stefan-strandberg-on-the-sound-of-battlefield-3/ (interview, no tech
    detail).
