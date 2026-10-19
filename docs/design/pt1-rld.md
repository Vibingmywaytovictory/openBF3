# PT1 rld: reload and weapon foley sounds

The M2b playtest found no sound for reloads. This track (branch `pt1/rld`) does three things:

- finds out from BF3's data how reload and handling sounds are triggered;
- finds why ours were silent;
- posts them for the own soldier (1p) and for the others (3p, positioned, under the A3 scopes and voice cull).

## 1. How BF3's data triggers them

Source: the SoundPatchAssets of Metro TDM's kit weapons, compiled to `SoundProgram`s by the repo's own compiler and
read by `read_weapon_foley` (`src/bf3_audio/include/bf3/audio/weapon_foley.h`). Hashes are `ebx_name_hash` names.
Verified 2026-10-19 on all 12 kit defs. The row `audio.foley.install` prints the table again.

| input | what it starts in the graph | kit coverage |
|---|---|---|
| event `Reload` (3294210644) | A MultiCrossfader on the Physics distance (1-19 m linear; pistols 1-17 m) feeds one LtNone Sampler. Its wave comes from a WaveSwitcher on the parameter `IsShortReload` (45329532): `[0] <w>_reload_1p_long_wave`, `[1] <w>_reload_1p_short_wave`. The event #3520090073 (name not resolved) drives the Sampler's Release port. The voice goes to the `Handling` Cone output (Gain -29 dB, MinDistance 1, EnableHdr, BypassHeadroom = IsFirstPerson). | 10/10 firearms, and the M67 (ungated, to Cone) |
| event `Action` (2484178491) with parameter `ActionType` (1114728387) | A MultiCrossfader (1-15 m cosine; pistols 1-10 m) feeds the handling Sampler. Its wave comes from a WaveSwitcher on ActionType: `0 generic_deploy` (`m9_deploy` on pistols), `1 zoom_iron_sight`, `2 trigger_click`, `3 bipod_deploy` (`m249_mount` on the LMGs), `4 bipod_undeploy`, `5 lightswitch`, `6 firemodeswitch_us/_ru`. SimpleValueSelectors on ActionType set its amplitude and pitch. | 10/10 firearms. The knife's Action plays `knife_deploy` (<= 12 m). |
| event `Bolt` (2088812592) | the bolt-action rifles' bolt wave (M98B, 1-10 m) | The MK11 and SVD take the input, but it starts no sampler. |

Long means empty and short means tactical. On the M16A3 the long wave is 2.905 s and the short one 2.367 s; the sim's
ReloadTime is 2.37 s empty and 1.8 s tactical.

The reload is one whole wave (1 variation, 1 segment). Magazine out, magazine in and the bolt are all inside it, so the
sound needs no clip event tracks.

Footsteps are a different mechanism. The soldier's SoundEntity is driven by AntEventEntityData step events
(research/topics/gap-r1-audio-system.md §3). Footsteps do not share this path and are not part of this track.

## 2. Root causes (measured before the fix)

The old code did post the reload one-shot (`weapon_audio.cpp`: OwnTimed kReloadStarted, and on_others on a `reload_seq`
change), but it could not be heard.

How it was measured: `audio.foley.install` runs the game's mixer config offline (`apply_mixer_a3`, the kit scopes,
`audio_tuning.cfg`), with the router driven one 60 Hz frame at a time.

1. **HDR phantom loudness (the main cause).** `PatchInstance::cull_info` gave an instance's loudness as the maximum over
   *every* EnableHdr output of the patch. For a reload instance, that maximum came from the gunshot outputs, which play
   nothing. Measured for an M16A3 reload instance at the listener:
   - cull loudness: 100.4 dB;
   - its EnableHdr outputs: #0 79.5 dB (the reload's Handling output), #2 94.4 dB, #3 100.4 dB, #4 100.4 dB.

   So the HDR window took its top from a sound that does not play: the window top was 100.4 dB while only the reload
   played. That put the reload 21 dB down:
   - the reload: **-39.6 dBFS** (loudest 20 ms);
   - the 30-round M16A3 burst, for scale: peaks at -0.5 dBFS;
   - after an emptied magazine: -39.7 dBFS (window top 100.4).

   Any quieter sound alongside the reload was ducked by the same 21 dB.
2. **IsShortReload never set.** No code set the parameter, so every reload played wave 0, the long (empty) reload, even a
   tactical one. On the M16A3 that is 2.905 s of sound for a 1.8 s reload.
3. **The release was never posted.** The data wires the event #3520090073 to the reload sampler's Release. Nothing posted
   it on kReloadCancelled, on a switch or on a death.
4. **The own 1p reload stayed where it started.** Its emitter was the listener's position at the start, and every
   listener move re-sent that start position (`OneShotPool::resend_emitters`). Walking at 3 m/s, the last 1.4 s of the
   reload measured **-50.8 dBFS**, against -39.6 dBFS standing.
5. **Action and Bolt were never posted.** Deploy/switch, zoom, the dry-fire click and the fire-mode switch were silent
   for everyone. The data has them on every kit firearm.
6. **A phantom round 3 s after every one-shot of a single-fire weapon.** This was found while measuring the fix.
   - The one-shot pool sends Stop 3 s after a start (`kOneShotStopAfterSeconds`), and the mixer then posts the patch's
     stop event.
   - On a weapon patch, the stop event is the trigger release, and on the single-fire patches that fires the shot.
   - Measured 3.0 s after a reload one-shot: the M9, MP443, MK11 and SVD patches started their whole shot (corebass,
     lfe, hifi, noise, reflections, shell casings).
   - The old reload one-shot did the same.
7. **The own reload was ducked after firing** (found by the verifier). The mixer did not implement the outputs'
   `BypassHeadroom` port. A reload started 0.1 s after a 30-round M16A3 burst measured **-39.7 dBFS** (window top
   100.4 dB), the same as root cause 1's level: the burst's reflections tail (`reflections_close_openfield`, outputs 2
   and 4) plays until 3.1 s after the last round and holds the top at the patch's static 100.4 dB. An empty reload
   (2.9 s) after a magazine dump fell entirely inside that span.
   - The data: on all 10 kit firearms, *every* output (Handling, Cone, both Flats, LFE) reads its BypassHeadroom
     port from one register (M16A3: register 108). After one evaluation that register is 1 with IsFirstPerson 1 and 0
     with IsFirstPerson 0 (IsFirstPerson is register 1). The knife's Cone has the constant 0; the M67's Cone reads
     register 13.

## 3. The fix

- **`weapon_foley.{h,cpp}` (new).** Reads the foley inputs of a weapon program from its graph:
  - for each event: its samplers, its distance reach (the largest FadeEnd of the distance crossfaders on its path), its
    release event and the outputs its voices feed;
  - the wave names of the IsShortReload and ActionType switchers.

  The reader is bounded: each register and node is visited once, and the sampler-slot loop counts in `uint32_t` (a
  `uint16_t` counter wrapped at 65535 on a hostile `plugins_count`). On hostile data it fails soft and returns empty
  fields. Its result is `WeaponAudioDef::foley`, filled at load.
- **The router** (`weapon_audio.cpp`, PT1 rld insertions) posts the data's events where the weapon state changes.
  - **The own soldier, on the sim's timed events**, at their exact Predicted time:
    - kReloadStarted posts Reload, with IsShortReload = (magazine > 0);
    - kReloadCancelled posts the release;
    - kDeployStarted posts the release, then Action Deploy;
    - kFireModeChanged posts Action FireMode;
    - kBoltStarted posts Bolt.
  - **The own soldier, from the presented own weapons** (`set_own_weapons`, called by `GameAudioDriver::step`):
    - the zoom's rising edge posts Action Zoom;
    - the tick flag kDryFire posts Action DryFire.

    The own 1p one-shots follow the listener.
  - **The others, from their presented fields** on the Render timeline:
    - a `reload_seq` change posts Reload; the magazine is not on the wire, so IsShortReload keeps the data's default of
      0 (long);
    - a def change posts Action Deploy;
    - an `ads` rising edge posts Action Zoom;
    - a weapon change or a death during a reload posts the release.

    Each one sits at the shooter's muzzle. It is not started beyond its event's reach in the data, because the
    crossfader would start nothing there (counted in `foley_out_of_reach`).
- **The voice cull** (`patch_instance.cpp`, `// PT1 rld [live outputs]`). An instance's loudness now counts only the
  EnableHdr outputs that its busy sampler slots feed, using voice send reachability computed at create.
  - Before any slot is busy (the first block), it counts the outputs the starter names: `set_loudness_outputs`, to which
    the router passes the event's outputs.
  - With no names, it counts all of them, as before.
- **BypassHeadroom** (`// PT1 rld [bypass headroom]`: `patch_nodes.cpp` reads port 0 of each Cone, Flat and Direct
  output every evaluation; `PatchInstance::State::hdr_send_gain` in `patch_instance.cpp` replaces the two copies of
  M2b au R5's over / target lines; `HdrVoiceWindow::alone_top_db`, set in `mixer_core.cpp`).
  - With the port on, an EnableHdr output's gain uses the top `min(window top, alone_top_db)`, where alone_top_db =
    max(WindowMinTop, the instance's cull loudness): the top the instance would set if it played alone. It is not
    suspended either.
  - So the own 1p reload (79.5 dB) plays at the 80 dB window it has alone, whatever the own burst's tail does.
    The own gunshot sets the top itself (alone_top 100.4 = top), so its gain is unchanged. 3p sounds (IsFirstPerson
    0) keep R5's gain.
  - BF3's meaning of the port is not known (its name suggests the own sounds skip the HDR headroom): this is a named
    default, `measured = false`. One consequence to listen for: the own 1p sounds are not ducked by a louder 3p sound
    (an explosion) either.
- **The others' foley follows them** (`// PT1 rld [follow]`): `OneShotPool::set_emitter` moves a live one-shot and
  keeps the new emitter for `resend_emitters`. The router keeps up to 4 running foley one-shots per other soldier and
  moves them to his muzzle each frame it changes; the own 1p foley uses the same call.
- **`PatchInstance::skip_stop_event`** (`// PT1 rld [no stop]`). The router's foley one-shots treat the stop event as
  already seen. They end when their samplers are idle, and the pool's Stop posts nothing to them.

**What the data's release does.** Every kit reload wave has 1 variation of 1 segment, and its sampler is LtNone. Under
the engine's sampler rules (docs/design/m2a-audio.md §6.3, as built, not measured against BF3), a Release on LtNone ends
the sampler after its current segment. A cut reload therefore still plays its whole wave. The router posts the release
where the data wires it (shown in the trigger table) and adds no fade of its own.

**Sync.** Every foley cue is posted at the mixer frame its sim time maps to (`SimMixerClock::frame_of`): the Predicted
timeline for the own soldier, the Render timeline for the others. The viewmodel clip and the gunshots use the same time
and the same mapping. In the trigger table, every cue's wave starts +0.0 ms after that frame. The sampler Offset
(0.015 s) skips into the wave; it does not delay it.

## 4. Measured (before, after)

The numbers come from `audio.foley.install`: the game's mixer configuration, run offline, with the router driven one
60 Hz frame at a time. WAVs of our own mix are in `b\shots\pt1-rld\` (derived data, gitignored).

| measurement | before | after |
|---|---|---|
| own M16A3 tactical reload alone, loudest 20 ms | -39.6 dBFS (window top 100.4 dB) | **-19.2 dBFS** (window top 80.0 dB) |
| cull loudness of a reload instance at its start | 100.4 dB | 79.5 dB (its Handling output) |
| the same reload's last 1.4 s, walking at 3 m/s | -50.8 dBFS | -19.2 dBFS (the same as standing) |
| 30-round M16A3 burst | -0.5 dBFS | -0.5 dBFS |
| the reload 0.6-2.9 s after the burst's last round | -39.7 dBFS (top 100.4) | **-19.3 dBFS** (top still 100.4: the tail's) |
| the Handling output's BypassHeadroom, IsFirstPerson 1 / 0 | not read | 1 / 0 |
| tactical reload wave | long (2.89 s): IsShortReload never set | `m16_reload_1p_short_wave`, 2.35 s |
| deploy, zoom, dry fire, fire-mode switch | silent (never posted) | generic_deploy 0.94 s, zoom_iron_sight 0.94 s, trigger_click 0.21 s, firemodeswitch_us 0.05 s |
| a reload one-shot on the M9 / MP443 / MK11 / SVD | a phantom shot 3.0 s later | nothing else plays (checked on all 10 firearms) |
| another soldier's M16A3 reload, 3p | posted at any distance up to 1000 m | 5 m: -26.6 dBFS; 15 m: -48.0 dBFS; 25 m: not started (past the data's 19 m) |
| the same, walking away at 3 m/s from 5 m: its 2.5-3.9 s | stayed at the start (the class of root cause 4) | -39.9 dBFS, against -26.6 dBFS standing |

The own trigger table (M16A3, 1p). Each wave starts +0.0 ms after the frame its event's time maps to:

```
 0.50 s Action        deploy    -> generic_deploy_wave           0.94 s
 1.00 s Reload        tactical  -> m16_reload_1p_short_wave      2.35 s
 3.50 s Reload        empty     -> m16_reload_1p_long_wave       2.89 s
 7.00 s Reload        tactical  -> m16_reload_1p_short_wave      2.35 s
 7.60 s ReloadRelease           -> #3520090073 to the running reload (kReloadCancelled)
 8.50 s Action        dry fire  -> trigger_click_wave            0.21 s
 9.00 s Action        fire mode -> firemodeswitch_us_wave        0.05 s
 9.50 s Action        zoom      -> zoom_iron_sight_wave          0.94 s
10.50 s Reload        tactical  -> m16_reload_1p_short_wave      2.35 s
11.00 s ReloadRelease           -> #3520090073 (a switch)
11.00 s Action        deploy    -> generic_deploy_wave           0.94 s
```

Every kit firearm's reload as played (the empty wave is the long one, the tactical wave the short one):

| weapon | empty | tactical |
|---|---|---|
| AK-74M | 3.62 s | 2.69 s |
| AKS-74u | 3.35 s | 2.35 s |
| M16A3 | 2.89 s | 2.35 s |
| M27 IAR | 2.92 s | 2.35 s |
| M4A1 | 3.15 s | 2.69 s |
| M9 | 2.17 s | 1.90 s |
| MK11 | 2.89 s | 2.29 s |
| MP443 | 2.17 s | 1.90 s |
| RPK | 4.52 s | 3.62 s |
| SVD | 3.89 s | 2.99 s |

The pool's Stop at 3 s cuts none of them.

## 5. Open issues

1. **BypassHeadroom is a named default** (`measured = false`, root cause 7). The tail itself still counts at its
   output's static 100.4 dB (the HDR model as built, m2b-au), so 3p sounds stay ducked under the own burst's tail for
   about 3 s, as before.
2. **The others' sprint-cancelled reloads post no release.** The wire carries the phase but not the magazine, so a
   Reloading -> Ready cut looks the same as a finished reload. A switch or a death does post the release. Under LtNone
   this changes nothing audible today.
3. **The own soldier in 3p.** Its foley sits at the listener, as the old reload did, not at the own soldier's muzzle.
4. **Footsteps** use another mechanism (the soldier's SoundEntity and the Ant step events). They are not implemented
   here, and the Ant reader parses no event tracks.

## 6. Checks (one command each)

From the worktree, after `tools\build.cmd dev`:

- The data table (with each output's BypassHeadroom source), the own trigger table, every kit reload, 3p (and walking
  away), the mix (alone, walking, after a burst) and the corruption sweep:
  `ctest --test-dir b\dev -R "^audio\.foley\.install$" -V`
- The reader on a hostile synthetic program: `ctest --test-dir b\dev -R "^audio\.foley\.unit$" -V`
- The same under asan: `tools\build.cmd asan --no-test`, then `ctest --test-dir b\asan -R "^audio\.foley\." -V`
