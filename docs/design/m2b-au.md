# M2b au: audio A3, as built

Track `au` of M2b wave 1 on branch `m2b/au`, from the W1.0 contract head 01a26e8. Design: [m2b.md](m2b.md) §6
(the HDR window, scopes and culling, the profiler), the gates E10 and E11, and the named changes R5 and R6. Where the
design and the code disagree, this file says which was followed and why. Rules the data does not state are marked
**[inferred]**, `measured = false`.

## 1. Commits

| commit | what |
|---|---|
| 252cb37 | New `hdr_setting.cpp`, `sound_scope.cpp` (bf3_core), `hdr_window.cpp`, `voice_cull.cpp` (bf3_audio). The as-built notes in the four W1.0 headers. `# M2b au` CMake blocks in bf3_core and bf3_audio. |
| 0deb5ac | Named insertions `[a3]` and R5 in `patch_instance.h`, `patch_state.h` and `patch_instance.cpp` (§4, §5). |
| d8aab4a | Named insertion `[quiet]` in `PatchInstance::render` (§6). |
| 7a713ab | MixerCore: the named insertions `[a3]` / `[profile]` and the end blocks of `mixer_core.{h,cpp}` (§3, §7). |
| 1ea2c74 | R6: `weapon_audio.h`'s voice limits, and the `m2a.contract3` pin that follows them (§5). |
| b0da580 | `Bf3GameAudio`: the `[a3]` insertions and the profiler readout block in `game_audio.{h,cpp}` (§8). |
| c2ce822 | `tests/audio_hdr_tests.cpp` (rows `audio.hdr.unit`, `audio.hdr.install`), the E11 block and the R6 line in `audio_router_tests.cpp`, the `# M2b au` rows block, `kAuLanded = true`. |
| (this) | This file and the README section "M2b au: audio A3". |

## 2. The install data [verified 2026-10-19, bf3dump on MP_Subway]

* **HdrSetting.** It is the only one, in `sound/soundstates` (MP_Subway superbundle):
  - WindowSize 45, WindowMinTop 80, WindowMinBottom 35;
  - top attack 0.05 s, top release 0.035 to 0.85 s, bottom attack 0 s, bottom release 0.025 s;
  - Headroom 5, AllowedOvershoot 3;
  - CompressFactor 0.5, DischargeFactor 3.35, MaxAllowedEnergy 110;
  - `WindowTopReleaseTimeCurve`: 5 points (X, Y, K), from (0.002, 0.976) to (0.998, 0.008).

  `read_hdr_setting` reads all of it and charges 7 budget units. `retail_hdr_setting()` holds the same values, so the
  data-free row runs the retail window.
* **Scopes.** `sound/audiosystem`'s `AudioSystemAsset.Scopes` holds 33 `SoundScopeData`, read for 166 budget units.
  Each has one `DefaultStrategy`:
  - `ClosestInstancesScopeStrategyData {Count}` sets `closest`;
  - `MaxInstancesScopeStrategyData {Count, KeepOldest}` sets `max_instances` when KeepOldest is true (a new one over
    the count is refused), and `latest` otherwise (the oldest goes);
  - `DuplicateSpawnScopeStrategyData {Distance, Time, ClosestCount, GroupTypes}` sets the radius, the time and
    `closest`.

  Examples:
  - Bullet_Impact_Scope: duplicates within 1 m and 0.4 s, then the closest 5;
  - Explosion_Scope: the closest 10;
  - Death_Scream_Scope: `Max_Instaces_4` with KeepOldest false, so the newest 4;
  - UI_Scope: the newest 1;
  - Glass_Debris and Destruction_small: 4 m and 2 s, then the closest 5.

  The strategy names are only labels. Several "Closest 3" strategies hold Count 5, and the code uses Count.
  GroupTypes is true on two DuplicateSpawn strategies. It is read and not applied (`measured = false`).
* **Patch to scope.** `SoundPatchAsset.Scope` is an external reference to a `SoundScopeData` instance:
  - `bullet_impact_concrete` names Bullet_Impact_Scope;
  - the M16A3 (`weapon_assaultrifle_m16a4_02`) names none, like every handheld weapon.

  An `EbxResolver` cannot list the partitions of a type. So `map_patch_scopes` reads the scope of each patch the game
  compiled (`SoundProgram::partition`). Metro's 64-shooter set has 30 of 56 programs scoped.

## 3. HDR window (`hdr_window.h`) and the mixer step

* **Per block.** Before the instances render, MixerCore's `a3_block` does three things:
  1. It culls (§4).
  2. It steps the window on the loudest kept HDR voice.
  3. It gives each kept voice an `HdrVoiceWindow`: active only for an instance with an EnableHdr output, plus the
     top, the headroom, the overshoot and `suspended` (below the bottom).
* **Loudness at the listener** (`PatchInstance::cull_info`): the patch's Loudness, plus the loudest output's distance
  curve (MinDistance clamp, `curve_x_metres`), plus its Gain, plus its last cone attenuation. It is taken over the
  EnableHdr outputs; over every output when there is none.
* **Top and bottom.** The top target is max(WindowMinTop, loudest). The bottom is max(WindowMinBottom,
  top - WindowSize).
* **[inferred] Attack.** The top rises on a linear ramp in dB that reaches the target `top_attack_s` after it started.
  A higher target during the ramp restarts it from the current top. 80 to 110 dB takes 5 blocks (53.3 ms).
* **[inferred] Release.** The fall to the target takes T = lerp(0.035, 0.85, clamp(drop / 45)) seconds. Its shape is
  the release curve read as the fraction of the drop still left at x = elapsed / T, rescaled to run from exactly 1 to
  0. A fall of 30 dB has T = 578 ms. On the retail curve it is within 1e-3 dB of the target after 49 blocks (523 ms).
* **[inferred] Bottom.** It follows its target at WindowSize / attack time when rising (instant at 0 s) and at
  WindowSize / release time when falling (1,800 dB/s at 0.025 s).
* **[inferred] AllowedOvershoot.** During the attack a voice sounds at most AllowedOvershoot above the top:
  gain = min(loudness, top + 3) - top - Headroom. Without the clamp, a 110 dB shot's first block would be +18.6 dB
  over its steady level, against +3 dB with it.
* **[inferred] Pre-cull edge.** The bottom of the previous step - 3 dB.
* **Not applied.** CompressFactor, DischargeFactor and MaxAllowedEnergy have no known formula (Q9).
* **When the step runs.** Only with an HdrSetting, a ScopeTable, or more slots than `max_voices`. Otherwise the block
  is M2a's exactly; `audio.mixer.unit` and every M2a row are unchanged.

## 4. Culling (`voice_cull.h`)

The order is m2b.md §6.2's:

1. **Pre-cull.** A starting HDR voice below the edge goes. A one-shot is dropped (`Precull`); a loop becomes
   `Pending`. Only starting voices are pre-culled. A playing voice that falls below the bottom is suspended instead:
   silent, still rendered.
2. **Scopes.** Duplicates first: a starting voice within the radius of an earlier kept voice of the same scope that
   started less than the time before it. Then the closest N by distance, then the newest N (`latest`), then the
   oldest N (`max_instances`).
3. **The cap.** The voices are ranked by loudness x priority (priority 1: the data has none). A loop over the cap is
   pending; a one-shot is dropped.

A dropped voice goes back to the game thread with a finished feedback, so the router's voice and the one-shot pool's
slot are freed. A pending loop is kept but not rendered, and it resumes when the cull keeps it. A stop request drops
it. Every pass is O(n²) over at most 1,024 voices with a fixed stack array. A larger span is refused: every verdict
is Keep and nothing is counted.

**The cap counts sampler voices, not patch instances.** This is followed over the plain reading of "128 voices".
`CullVoice::voices` is an instance's busy sampler slots (playing, or armed to start) at the block's start, at least
1. The kept voices fill the 128 in score order; the first that does not fit goes, with every voice after it.
`MixerStats::live_voices_max` is that count. The reasons:

* In BF3 a voice is one playing sound. A weapon patch instance holds 1 to 8 of them.
* With R6 the router no longer steals tails. An instance cap of 128 kept up to 128 weapon instances on the 64-shooter
  load: about 125 instances, 396 busy slots and 3,420 voice chains per block. That made the mix 3 to 5 times slower
  than M2a's (wall 29 to 58 s for 66 s of audio, late blocks 650 to 1,609).
* With the sampler-voice cap, the same load mixes in 8.5 s of wall time, faster than M2a's 10.2 s, with 0 late blocks
  (branch measurements, indicative only).

Slots that a block triggers count from the next block.

## 5. R5 and R6

* **R5** (`patch_instance.cpp`). The Send gain of an EnableHdr output is
  curve + cone + (Loudness - top - Headroom) - overshoot, and 0 while suspended. This applies when the mixer gave the
  instance an active window. Any other output keeps the fixed window line (`window_top_db` 115, headroom 6) unchanged.
  On the E10 footstep this is +36.0 dB against the fixed window (-23.23 against -59.23 dBFS).
* **R6** (`weapon_audio.h`). The router's limits change:

  | constant | A2 | M2b |
  |---|---|---|
  | `kMaxWeaponVoices` | 32 | `kGameMixerVoices` (128) |
  | `kMaxOneShotVoices` | 64 | 128 |
  | `kGameMixerInstances` | 96 | 256 (the mixer slots, so a pending loop never refuses an instance) |

  The router keeps its bookkeeping at those numbers, so the mixer's cull decides. Two lines follow:
  - `m2a_contract3_tests.cpp:108` pins 128 / 128 / 256;
  - in the 64-shooter case, `CHECK(ws.culled > 0)` becomes ws.culled plus the mixer's three cull counts > 0.

## 6. Quiet voices (`[quiet]`, an E11 cost insertion)

* **Why.** `PatchInstance::render` walks every voice chain of a patch every sub-block, playing or not.
* **The skip.** A voice is checked once after its slot plugins. It is quiet when its buffer holds only zeros, no
  later plugin is a delay line, and every later filter's state is zero. Its later plugins then skip the per-sample
  work: a gain, filter or pan of zeros from a zero state, or a send adding zeros, changes nothing.
* **State.** `State::quiet_plugin` moves the plugin's state exactly as the full path would: ramps to their targets,
  filter coefficients to their designed values. The mix is unchanged; only the sign of a zero sample inside a voice
  buffer may differ. Taps turn the skip off.
* **Measured.** On the 64-shooter render the skip takes about 1 s off 8.5 s of mixing: 8.61 / 8.53 / 8.54 s with it
  against 9.51 / 9.47 / 9.67 s without, indicative only. `audio.patch.unit`, `audio.oracles`, `audio.m16.offline` and
  `audio.game` pass unchanged.

## 7. Profiler (`mixer_core.h` end block)

* **Timing.** `render_block` times the whole block (commands, cull, window, render, tap) and files its share of
  10.67 ms in `mix_ns_hist`. Bucket i < 15 holds the shares below `kMixHistEdges[i]`: 0.01, 0.02, 0.03, 0.04, 0.05,
  0.06, 0.08, 0.10, 0.12, 0.15, 0.20, 0.30, 0.50, 0.75 and 1.00. Bucket 15 holds the late blocks.
* **Quantiles.** `mix_share_quantile` returns the upper edge of the quantile's bucket. It is exact at 0.10, E11's gate.
* **Readout.** `read_mixer_profile` / `format_mixer_profile` give one line:
  - the mix share: last, max, p50 and p99;
  - the instances rendered, and the most sampler voices kept;
  - the culled counts;
  - the suspended voices;
  - the window.

## 8. Bf3GameAudio (`game_audio.{h,cpp}`)

* **Load.** `load()` reads the A3 inputs on the loader thread: the HdrSetting, plus the scopes of every program the
  game plays (the kit drivers and explosions, the impact and effect sounds, the auditions). It adds one report line.
  A part that does not read is noted, and the mixer runs without it.
* **Build.** `build()` sets `max_voices = kGameMixerVoices` and applies both.
* **Readout.** `game_audio_profile()` and `game_audio_profile_text()` are for a dev overlay. The game scene has no
  F3 overlay yet; gx shows them.

## 9. Gates

Measured 2026-10-19 on `m2b/au`, in `b\dev` unless stated, on a shared machine. Wall-clock values are indicative
only: E11's timing gate is measured in the wave-1 merge pass, never on the branch.

| gate | threshold | measured | met | command |
|---|---|---|---|---|
| E10 window numbers | 71 dB alone gives 80 / 35; 110 dB gives top 110 within 0.05 s ± one block and bottom 65; a 63 dB source is suspended | 80 / 35. 110 after 5 blocks (53.3 ms ≤ 60.7 ms), alone and through MixerCore. Bottom 65, the 63 dB source suspended and silent | yes | `ctest --test-dir b\dev -R "^audio\.hdr\.unit$"` |
| E10 retail data | the HdrSetting and 33 scopes read from the install | every value equal to the defaults, 5 curve points; 33 scopes; impact to Bullet_Impact_Scope, M16A3 none | yes | `ctest --test-dir b\dev -R "^audio\.hdr\.install$"` |
| E11 live voices | ≤ 128 every block | 128 max (sampler voices, §4). Culled: 717 pre-cull, 207 scope, 397 cap | yes | `ctest --test-dir b\dev -R "^audio\.game\.crowd$"` |
| E11 late blocks | 0 | 0 on solo runs (max block 2.3 to 3.7 ms); 4 under `-j 4`. Indicative only | not measured under gate conditions | `set OPENBF3_AU3_GATE_LATE=1 && ctest --test-dir b\dev -R "^audio\.game\.underruns$"` (merge pass) |
| E11 mix share | p99 ≤ 0.10 | p50 < 0.15 and p99 < 0.20 (bucket bounds) on the quietest solo run. Indicative only, and likely not met (§10) | no (indicative) | the same row as E11 late blocks |
| install unchanged | snapshot clean | clean (`audio.hdr.install`, `audio.game.crowd`) | yes | `ctest --test-dir b\dev -R "^audio\.(hdr\.install|game\.crowd)$"` |
| asan | the new parser rows and the A3 path pass under asan | `audio.hdr.unit`, `.hdr.install`, `.patch.unit`, `.mixer.unit`, `.router.unit`, `.game`, `.game.crowd`, `.fuzz`, `.oracles`, `m2b.contract` pass; the crowd's cull counts are identical to dev | yes | `tools\build.cmd asan --no-test` then `ctest --test-dir b\asan -R "^audio\.(hdr\.|game)"` |

Other rows run once on the branch, all passing: `audio.contract`, `audio.xas.unit`, `audio.patch.unit`,
`audio.dsp.unit`, `audio.mixer.unit`, `audio.router.unit`, `audio.game` (E7 rehearsal: audition skips none),
`audio.oracles`, `audio.cache`, `audio.fuzz`, `audio.m16.offline`, `m2a.contract`, `m2a.contract3` and
`m2b.contract` (the au row is no longer PENDING). `audio.hdr.unit` takes 0.6 s and `audio.hdr.install` 0.3 s.

## 10. Open issues

* **E11 p99 ≤ 0.10 is probably not met.** With the cap at 128 sampler voices on the 64-shooter load, the mix took
  about 12 % of a core on average on the branch (time per phase, one run):
  - 60 % in the sampler's slots: decode and Catmull-Rom, about 12 ns per voice-frame;
  - 37 % in the other voice plugins and the per-sub-block zero fill of every voice buffer;
  - 3 % in program evaluation.

  The gate needs about half that. The remaining work is M2a's sampler and render code (`patch_sampler.cpp`, the
  zero fill in `render`), which this track does not own. The merge pass measures it; if it fails, the follow-up is a
  sampler pass.
* **Sampler voices or instances.** The sampler-voice reading of "128 voices" (§4) changes what `live_voices_max`
  means against m2b.md's instance-level wording. Audibly, more distant tails and impacts are culled than under A2's
  32 + 64 instance limits: on the 64-shooter load the pool peaks at 15 one-shots, against 64.
* **E14 and the Godot auditions** were not run on the branch: they are window rows (wave 2). Far auditions now sound
  at the HDR gain instead of the fixed window's: about +36 dB at top 80 for a voice that is alone.
* **[inferred] rules**, all `measured = false`: the attack shape, the release duration and curve use, the bottom's
  slew, the overshoot clamp, the pre-cull edge, priority 1 and GroupTypes. Each is listed in §3 and §4.
