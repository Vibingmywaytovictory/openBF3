# PT1 snd: the "sound glitching" playtest finding

Track `snd` of the first playtest fixes, on branch `pt1/snd` from main 6a4061e (M2b). The finding: "Sound glitching
sometimes". This track covers only that sentence; reload sounds, friend or foe, the run animation and the HUD are
other tracks. Rules the data does not state are marked **[inferred]**, `measured = false`.

## 1. Commits

| commit | what |
|---|---|
| a44db21 | bf3_audio: the instrumentation `[edge]` / `[cuts]`, the cut fade `[fade]` and the cap hysteresis `[hold]` (§4, §5); the `audio-cuts` suite in `tests/audio_hdr_tests.cpp`. |
| 9efc25c | The `audio.cuts.unit` row (new `# PT1 snd` block at the end of `tests/CMakeLists.txt`). |
| 9a99f00 | Named insertion `[capture]` in `game_audio.cpp` (shared): the `[pt1] audio health:` line and `--audio-capture`. `audio_stream.*`: the audio thread's priority readout. |
| 71aea08 | Named insertion `[fade]` in `game_audio.cpp`'s `build()`: `--audio-cut-fade=<frames>`. |
| 7ff2378 | Named insertion `[hold]` in `game_audio.cpp`'s `build()`: `--audio-cap-hold=0`. |
| 5443b59 | Named insertions `[cuts]` `[fade]` `[hold]` in the 64-shooter case (`audio.game.crowd`, `audio.game.underruns`). |
| 8e06a2b | `LateBlock` cut events. |
| 3d705bf | Named insertion `[capture]` in `game_audio.cpp`'s `load()`: the capture's PathGuard (a crash fix, §3.6). |
| 9724433 | `tools/audio_capture_check.py`. |
| a8e89ce | The output limiter `[limit]` (§6) and its audio-cuts case. |
| 1a33310 | The audio thread's priority `[prio]` (§7): `audio_stream.*` and the PT1 snd block of `game_audio.cpp`. |
| 7c8c8b8 | Named insertion `[limit]` in `game_audio.cpp`'s `build()`: `--audio-limit=0`. |
| 83caba4 | Named insertion `[limit]` in the 64-shooter case. |
| 3a84224 | The limiter's block counter; the capture check counts frames over 0 dBFS. |
| (this) | This file and the README section "PT1 snd". |

## 2. How it was measured

Nothing here reads, hooks or runs bf3.exe. The install is only read through the repo's readers, and the captures are
our own mixer's output. They are derived data under `b\shots\pt1-snd\` (gitignored) and never committed.

* **Cut counters** (`mixer_core.h` `MixerCutStats`, `[cuts]`). A *cut* is a moment an instance's part of the mix
  starts or stops without a ramp:
  - a cap drop, a Kill, a loop parked as pending, a natural finish;
  - a Start, a resume.

  Its *step* is the sample the instance's sends wrote on the last frame before the cut, or on the first frame after
  a start (`PatchInstance::pt1_edge()`, `[edge]`). That is the discontinuity the cut leaves in the mix. Each kind
  counts its events, the steps above -60 dBFS (1e-3) and above -40 dBFS (1e-2), and its largest step. Also counted:
  - *abrupt gain ramps*: a send gain change over 3 dB within fewer than 64 frames, so the HDR gain and the curves are
    covered;
  - the gaps between Godot's `mix()` calls (a histogram);
  - `LateBlock` events: a block that took longer than its 10.67 ms to render, with its frame;
  - the limiter (§6) and the frames over 0 dBFS before and after it.
* **`[pt1] audio health:`** One line in godot.log when every game mixer shuts down. Besides the counters: late
  commands, starved voices, decode errors, ring overflows, the output latency, the audio thread's priority before
  and after `[prio]`, and the main thread's hitches over 50 ms.
* **`--audio-capture=<dir>`** (user arg) records, through the install's PathGuard:
  - the mix in `mix.wav` (float, 48 kHz stereo, the first `--audio-capture-seconds`, default 150);
  - every cut in `cuts.csv`: its frame, handle, step, loudness, distance and program;
  - the main thread's hitches in `hitches.csv`.

  `tools\audio_capture_check.py <dir>` (numpy) analyses the capture:
  - the frames over 0 dBFS and their episodes (runs of blocks);
  - isolated sample steps (|dx| > 0.01 and 12 times the local median), split by whether they come within 200 ms
    after a start (an onset) or not;
  - zero gaps of 64 frames or more inside sound (a decoder underflow);
  - the cuts by kind, and which cuts above -40 dBFS show in the mix at their frame;
  - the loops parked more than twice.
* **Real rounds.** In a headless run the game audio is not built (`scene_ = !headless_ && !m1_check_`, and
  `m2a_load_assets` loads audio only `if (scene_)`), so the real-scene numbers come from windowed rounds:
  - the M2b play check (`--m2b-check=play`), 31 bots, kill limit 30, pre-round 0;
  - eight rounds of 69 to 87 s, each run once with the user away (the user-active flag absent).

  The offline 64-shooter render (`audio.game.crowd`) is the same mixer and router on the same data, without Godot.

| round | config | capture |
|---|---|---|
| B1 | `--audio-cut-fade=0 --audio-cap-hold=0`: the A3 mixer as merged | lost (§3.6) |
| B2 | the same | `windowed-before` |
| A1, A2 | `[fade]` + `[hold]` | `windowed-after`, `windowed-after2` |
| P1, P2 | `[fade]` + `[hold]` + `[prio]` | `windowed-prio1`, `windowed-prio2` |
| F | every fix (the defaults) | `windowed-final` |

## 3. Root causes [measured 2026-10-19]

### 3.1 Hard cuts at the voice cap and on Kill (proved; fixed by `[fade]`, §4)

In the merged A3 mixer (m2b-au.md §4), a voice could leave or come back between two samples:

* a Kill gave the instance back at once, although the header said "short fade";
* the 128-voice cap dropped a sounding one-shot;
* the cap parked a sounding loop as pending, and later resumed it at full level.

Each cut leaves a step of the voice's level in the mix: a click.

| cut (merged) | B2 (76.2 s) | B1 (78.6 s) | 64 shooters offline (66 s) |
|---|---|---|---|
| pend: events / > -60 / > -40 dBFS, max step | 727 / 292 / 83, 0.158 | 663 / 192 / 30, 0.094 | 3234 / 1833 / 404, 0.131 |
| resume | 420 / 115 / 38, 0.144 | 321 / 97 / 17, 0.046 | 2062 / 1086 / 173 |
| drop | 644 / 7 / 4, 0.122 | 727 / 24 / 3, 0.064 | 189 / 36 / 8, 0.040 |
| kill | 5 / 3 / 3, 0.097 | 1 / 0 / 0 | 1 / 0 / 0 |
| finish | 836 / 0 / 0 | 938 / 0 / 0 | 530 / 0 / 0 |

In B2:

* 128 cuts were above -40 dBFS, 13 of them at least 4 times the local sample-to-sample median at their frame in
  `mix.wav`, so they stand out from the content;
* the loudest was an Mk11 at 86 m parked at a step of 0.158 (-16 dBFS), at 50.57 s;
* the programs were the distant automatic weapons (M27, M16A3, AK-74M, SVD, M4A1).

A natural finish never cut: its step was 0 in every run, because a finished one-shot has already decayed. A start
never cut either (the largest head was 0.0007), because the waves start at zero.

### 3.2 The cap flapped (proved; fixed by `[hold]`, §5)

The cap ranked by loudness, and weighted each voice by its sampler slots that were busy at that moment.
Automatic-fire patches re-trigger slots shot by shot, so the total demand went up and down every block. The loop at
the cap's edge was parked and resumed again and again. Offline, one M4A1 loop at 40.5 m, a constant 80.1 dB, was
parked and resumed every 10 to 200 ms.

* 64 shooters: 1843 of 3234 parks lasted under 0.25 s, and 1931 parks came within 1 s of the park before, on 161
  loops.
* B2: 65 loops were parked more than twice; the worst, 26 times in 73 s.

Every flap was a pair of the cuts in §3.1, and also a hole in a distant burst.

### 3.3 The mix clipped (proved; fixed by `[limit]`, §6)

Nothing in A3 bounds the sum of the voices. R5 puts each HDR voice at its Loudness - top - Headroom. The
HdrSetting's energy terms (CompressFactor 0.5, DischargeFactor 3.35, MaxAllowedEnergy 110) are read but not applied:
they have no known formula (m2b.md Q9).

| round | frames over 0 dBFS | blocks | episodes | peak |
|---|---|---|---|---|
| B2 | 867 | 187 | 126 | 1.81 |
| A1 | 261 | 49 | 43 | 1.49 |
| A2 | 399 | 73 | 65 | 1.56 |
| P1 | 2438 | 296 | 176 | 2.27 |
| P2 | 1445 | 140 | 94 | 2.39 (+7.6 dBFS) |
| 64 shooters offline | 6043 | | | 2.62 |

Godot hands the float buffer on, and the device format clips it, at the latest in the endpoint's conversion. Whether
anything downstream limits is not observable from the extension. These are overs in our own output either way.

### 3.4 Late blocks on the device path (fits the evidence; addressed by `[prio]`, §7)

Godot's WASAPI thread runs `_mix` at normal priority (0). Without `[prio]`, 3 of 4 windowed rounds had late blocks:

| round | late blocks | max block |
|---|---|---|
| B1 | 1 | 12.8 ms |
| B2 | 7 | 21.6 ms |
| A1 | 0 | 3.7 ms |
| A2 | 5 | 107.3 ms |

The mixer's own work offline is under 3.7 ms (64 shooters).

In A2 all five came between 85.95 and 86.41 s, at the round end, beside the main thread's hitches at 85.97 and
85.98 s (57 and 97 ms). The mixer's workload in those blocks was ordinary: the usual few starts, drops and fades per
block. Nothing in the mixer locks or allocates (`mixer_core.cpp`). So the thread was not running: preempted, or
starved by the work around the hitch.

A 107 ms block is ten periods: the device buffer (479 frames, about 10 ms) ran dry, a dropout. With `[prio]`, P1,
P2 and F had 0 late blocks (max block 6.5, 3.9 and 2.9 ms), with main-thread hitches up to 299 ms. Three rounds
against four is not proof. The priority is therefore **[inferred]**.

### 3.5 Checked and not a cause

* **Call gaps and underruns from the call pattern.** Godot calls `mix()` with 512 frames. WASAPI's period is 480
  frames (output latency 479), so in 1 period of 16 there is no call, and the gap is about 20 ms. That predicts
  about 470 gaps over 15 ms in 80 s. Measured gaps of 15 to 30 ms: 483 in B2 and 549 in A1. Outside A2's stall, no
  gap went over 25 ms (max 22.7 ms).
* **HDR gain steps.** 0 abrupt gain ramps in every windowed round. The 64-shooter render had 2, both under 0.0027
  per sample, from program gain events, not the window. An HDR suspend is not a cut: the send ramps to 0 over its
  sub-block, and every such ramp was 64 frames or longer. The counted "step" of a suspend is the level it ramps
  from (max 0.10 in F).
* **Decoder underflow.** 0 starved voices, 0 decode errors and 0 zero gaps inside sound in every capture.
* **The integ-w1 SIMD / fast paths** (`[xas groups]`, `[slot fast]`, `[lazy zero]`, `[biquad pair]`). The sample
  steps that come more than 200 ms after any start: 60 of 333 in B2, 17 of 559 in A1, and 60 of 455 in F.
  - The ones inspected sit inside sustained fire at an unchanged level, or at a rise in level. They are shot onsets
    re-triggered inside running fire loops, which have no Start event of their own.
  - There are no zero gaps inside sound.
  - No evidence that the fast paths glitch. There is no row that compares them against the general loop on
    streams, stereo sources or ratio changes (§10).

### 3.6 The capture itself (found and fixed)

B1 crashed at shutdown (0xC0000005) in `pt1_capture_end`, which read `m.db` for the PathGuard. The level loader
resets its `LevelDatabase` after load (`level_session.cpp`, `db = host::LevelDatabase()`), so the pointer dangled.
`load()` now copies the guard (3d705bf). B1's health line survived; its capture did not.

## 4. The fix: `[fade]`

`MixerConfig::pt1_cut_fade_frames = 512` (one block, 10.7 ms) **[inferred]**, `measured = false`. AudioSystemAsset and
the HdrSetting hold no voice-stop fade or virtualisation time, so the length is a named default. The fade is linear,
on the instance's output send, times the existing gain ramp (`patch_instance.cpp`, `PatchState::pt1_fade_at`).

* **Kill.** A voice that sounded fades out, and its slot goes at the fade's end. A voice that never rendered goes at
  once.
* **Cap drop of a one-shot.** The game gets `finished` in the block of the drop, as before, so the one-shot pool
  frees it on time. The mixer keeps the slot until the fade ends, without feedback.
* **Cap park of a loop.** It fades out, then becomes pending. A resumed loop fades in from 0.
* **Voices during a fade.** A fading slot keeps its voices against the cap, so `live_voices_max <= 128` holds.

## 5. The fix: `[hold]`

All three defaults are **[inferred]**, `measured = false`: the data has no voice priority hysteresis. Each is chosen
from the measured flap counts below.

* `pt1_cap_hold_db = 3` dB. A voice that sounded in the last block ranks 3 dB louder at the cap
  (`CullVoice::hold_db`). Pre-cull and the scopes do not see it.
* `pt1_voice_hold_blocks = 47` (0.5 s). An instance weighs the most busy slots it had over the last 0.5 s, so a fire
  loop between shots keeps its place.
* `pt1_resume_margin = 8` voices (at most `max_voices / 16`). A parked loop comes back only when 8 voices stay free
  after it (`CullVoice::resume_margin`). Then the next small rise in demand does not park it again.

| 64 shooters, 66 s | parks | parked < 0.25 s | parked again within 1 s |
|---|---|---|---|
| merged (no fade, no hold) | 3234 | 1843 | 1931 on 161 loops |
| fade only | - | 1889 | 1986 |
| hold 3 dB + 0.5 s, margin 0 | 1688 | 618 | 710 on 113 loops |
| margin 4 | 1094 | 105 | 164 on 58 loops |
| **margin 8 (default)** | **958** | **12** | **25 on 19 loops** |

## 6. The fix: `[limit]`

A peak limiter on MixerCore's stereo output, after `output_gain` and before the tap (`MixerCore::Impl::pt1_limit`).
It adds no latency, because the 512-frame block is the lookahead:

* a backward pass lets the gain fall at most 1 / `pt1_limit_attack_frames` per frame towards a peak, so the gain is
  down when the peak comes;
* a forward pass never rises above that, and returns with a `pt1_limit_release_ms` time constant.

A block under the ceiling at unity gain keeps its samples exactly: the unit case compares them, and no existing row
changed. Only a peak in a block's first frames can still make a gain step, counted as an "edge step": 0 in F, 3 in
the 64 shooters.

Defaults **[inferred]**, `measured = false`: `pt1_limit_ceiling = 0.966` (-0.3 dBFS), attack 48 frames (1 ms),
release 80 ms. A ceiling of 0 or below, or not finite, turns it off. This is a safety net in place of the energy
terms of §3.3. If Q9's formula is found, that is where the level belongs.

* F: 483 frames over 0 dBFS before the limiter, 0 after; peak 0.966; lowest gain -5.3 dB.
* 64 shooters: 2443 of 6188 blocks below -0.1 dB, lowest -8.6 dB. RMS -15.4 dBFS against -14.4 dBFS without it.

## 7. The fix: `[prio]`

When `Bf3GameAudio` builds, it asks for `THREAD_PRIORITY_HIGHEST` (2) on the thread that calls `_mix`. The
priority is set on that thread at the next `_mix` (`audio_stream.cpp`), **[inferred]**, `measured = false`.

* `--audio-thread-priority=keep` leaves the thread as it is; a Win32 value (-2..2, -15, 15) sets that one.
* It stays on Godot's own audio thread and its own process; nothing system-wide changes.
* The health line prints `priority 0 -> 2`.

## 8. Before / after

| | before (merged) | after (the defaults) |
|---|---|---|
| windowed: pend / resume / drop / kill above -60 dBFS | 292 / 115 / 7 / 3 (B2) | 0 / 0 / 0 / 0 (A1, A2, P1, P2, F) |
| windowed: cuts above -40 dBFS, and how many show in the mix | 128, 13 (B2) | 0, 0 |
| windowed: loops parked more than twice (worst) | 65 (26) | 5 to 15 (7 at most) |
| windowed: frames over 0 dBFS, episodes | 867, 126 (B2); up to 2438, 176 (P1) | 0, 0 (F) |
| windowed: late blocks (max block) | 1, 7, 0, 5 (up to 107 ms) | 0, 0, 0 (up to 6.5 ms) |
| 64 shooters: audible pend / resume / drop | 1833 / 1086 / 36 | 0 / 0 / 0 |
| 64 shooters: short parks, re-parks within 1 s | 1843, 1931 | 12, 25 |
| 64 shooters: peak | 2.62 | 0.966 |
| unit, Kill: largest step / peak | 0.79 | 0.1305 (the 1 kHz tone's own slope 0.1309) |
| unit, park and resume: step / sum of peaks | 0.37 | 0.069 |
| unit, one-shot drop: step / sum of peaks | 0.435 | 0.069 |

The fades themselves are 10.7 ms ramps, not steps: 889 in F, 39 of them from a level above -40 dBFS.

For the user's ear:

* `b\shots\pt1-snd\windowed-before\mix.wav` (B2), the merged mixer;
* `windowed-final\mix.wav` (F), every fix;
* each has its `cuts.csv`, which gives the times. B2's loudest parks are at 50.57 s, 49.63 s and 69.12 to 69.16 s.

## 9. Checks, one command each

| check | command | result (2026-10-19, branch) |
|---|---|---|
| the fades, the hold, the margin, the limiter, data-free | `ctest --test-dir b\dev -R "^audio\.cuts\.unit$"` | 7 cases pass |
| the A3 unit row (its scopes / cap case steps past the Kill's fade) | `ctest --test-dir b\dev -R "^audio\.hdr\.unit$"` | pass |
| 64 shooters: no audible cut, short parks and re-parks under 1 in 20, nothing over 0 dBFS | `ctest --test-dir b\dev -R "^audio\.game\.crowd$"` | 0 audible; 12 and 25 of 958 parks; 0 overs |
| the same on the merged mixer (A/B; the checks are off) | `set OPENBF3_PT1_CUT_FADE=0& set OPENBF3_PT1_CAP_HOLD=0& set OPENBF3_PT1_LIMIT=0& b\dev\tests\test_audio_router.exe --test-suite=audio-game "--test-case=64 shooters*"` | 1833 audible parks, peak 2.62 |
| a real round | `tools\play.cmd --m2b-check=play --kill-limit=30 --preround=0 --audio-capture=%CD%\b\shots\pt1-snd\run`, then `python -I tools\audio_capture_check.py b\shots\pt1-snd\run` | §8 |
| the merged mixer in a real round | the same plus `--audio-cut-fade=0 --audio-cap-hold=0 --audio-limit=0 --audio-thread-priority=keep` | §3 |
| the install unchanged | `ctest --test-dir b\dev -R "install\.unchanged"` | pass |

**Merge-pass rows the diff affects:**

* **`audio.game.underruns` (E11, timing).** It runs the 64-shooter case, so the new checks apply there too.
  - The cut fade renders faded sends without `add_scaled`, and the limiter adds a pass over each limited block, so
    such blocks cost a little more.
  - Branch, indicative only: max block 1.1 to 3.5 ms, histogram p99 in the < 0.08 to < 0.10 bucket. Merged: < 0.08.
  - The 0.10 gate is close; the merge pass decides.
* **`godot.m2a_firefight` (E14).** No change to `mix()`'s calls or to the stream; the audio thread runs at priority 2
  there too. Only the voices' cuts and the overs differ.
* **`godot.audio` (G8).** That scene's Kill now fades, and its MixerCore has the limiter, which acts only above
  -0.3 dBFS. It does not build a `Bf3GameAudio`, so `[prio]` does not apply there.

The mix's samples change wherever a cut or an over happened. No row pins a mix hash.

## 10. Open issues

* **Main-thread hitches** of 150 to 299 ms, 7 to 13 per round:
  - at about 1.8, 4.4, 10.4, 14.5 and 18.6 s (deploys, kill cards and the check's screenshots);
  - at the round end.

  During a hitch the game sends no audio commands: the loops keep playing, and the stalled frames' shots arrive
  together. These hitches belong to other tracks; `hitches.csv` lists them. With `[prio]` they no longer take the
  audio thread with them (§3.4).
* **The limiter stands in for the energy terms.** It caught 483 to 2438 frames a round, and the 64-shooter render
  sits below -0.1 dB in 2443 of 6188 blocks. A dense fight runs hot. HDR's energy terms (Q9) would bring the whole mix
  down instead.
* **The fast paths** have no differential row (§3.5).
* **`Bf3GameAudio::audition()`** compiles an unknown patch through `m.db` after load. Like the crash of §3.6, that
  pointer dangles once the loader has reset its database. It is not on the play path, and it is not changed here.
