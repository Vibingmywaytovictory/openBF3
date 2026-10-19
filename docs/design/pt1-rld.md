# PT1 rld: reload and weapon foley sounds

The M2b playtest found no sound for reloads. This track (branch `pt1/rld`) finds out from BF3's data how reload and
handling sounds are triggered, finds why ours were silent, and posts them for the own soldier (1p) and for the others
(3p, positioned, under the A3 scopes and voice cull).

## 1. How BF3's data triggers them

Source: the SoundPatchAssets of Metro TDM's kit weapons. They were compiled to `SoundProgram`s by the repo's own
compiler and read by `read_weapon_foley` (`src/bf3_audio/include/bf3/audio/weapon_foley.h`). Hashes are
`ebx_name_hash` names. Verified 2026-10-19 on all 12 kit defs; the row `audio.foley.install` prints the table again.

| input | what it starts in the graph | kit coverage |
|---|---|---|
| event `Reload` (3294210644) | MultiCrossfader on the Physics distance, 1-19 m linear (pistols 1-17 m) -> one LtNone Sampler. Its wave is a WaveSwitcher on the parameter `IsShortReload` (45329532): `[0] <w>_reload_1p_long_wave`, `[1] <w>_reload_1p_short_wave`. Its Release port is driven by the event #3520090073 (name not resolved). The voice goes to the `Handling` Cone output (Gain -29 dB, MinDistance 1, EnableHdr, BypassHeadroom = IsFirstPerson). | 10/10 firearms, plus the M67 (ungated, to Cone) |
| event `Action` (2484178491) + parameter `ActionType` (1114728387) | MultiCrossfader 1-15 m cosine (pistols 1-10 m) -> the handling Sampler. Its wave is a WaveSwitcher on ActionType: `0 generic_deploy` (`m9_deploy` on pistols), `1 zoom_iron_sight`, `2 trigger_click`, `3 bipod_deploy` (`m249_mount` on the LMGs), `4 bipod_undeploy`, `5 lightswitch`, `6 firemodeswitch_us/_ru`. Amplitude and pitch come from SimpleValueSelectors on ActionType. | 10/10 firearms. The knife's Action plays `knife_deploy` (<= 12 m). |
| event `Bolt` (2088812592) | the bolt-action bolt wave (M98B, 1-10 m) | the MK11 and SVD take the input but it starts no sampler |

Long = empty and short = tactical. On the M16A3 the waves are 2.905 s and 2.367 s; the sim's ReloadTime is empty 2.37 s
and tactical 1.8 s.

The reload is one whole wave. Magazine out, magazine in and the bolt are inside it, so the sound needs no clip event
tracks. Footsteps are a different mechanism: the soldier's SoundEntity, driven by AntEventEntityData step events
(research/topics/gap-r1-audio-system.md §3). They do not share this path and are not part of this track.

## 2. Root causes (measured before the fix)

The reload one-shot was posted by the old code (`weapon_audio.cpp`: OwnTimed kReloadStarted, and on_others on a
`reload_seq` change), but it could not be heard. Measured with `audio.foley.install`, which runs the game's mixer
config (`apply_mixer_a3`, kit scopes, audio_tuning.cfg) offline, with the router driven one 60 Hz frame at a time:

1. **HDR phantom loudness (the main cause).** `PatchInstance::cull_info` gave the loudness of the instance as the
   maximum over *every* EnableHdr output of the patch. For a reload instance that is the gunshot outputs, which play
   nothing. Measured for an M16A3 reload instance at the listener:
   - cull loudness: 100.4 dB;
   - its EnableHdr outputs: #0 79.5 dB (the reload's Handling output), #2 94.4 dB, #3 100.4 dB, #4 100.4 dB.

   The HDR window therefore took its top from a sound that does not play (window top 100.4 dB while only the reload
   played). It placed the reload 21 dB down, at **-39.6 dBFS** loudest 20 ms; the 30-round M16A3 burst peaks at
   -0.5 dBFS. After an emptied magazine the reload was -39.7 dBFS (window top 100.4). Any quieter sound alongside it
   was ducked by the same 21 dB.
2. **IsShortReload never set.** No code set the parameter, so every reload played wave 0, the long (empty) reload, even
   a tactical one. On the M16A3 that is 2.905 s of sound for a 1.8 s reload.
3. **No release.** The reload's release event (#3520090073) was never posted. A reload cut short by a switch, a sprint
   cancel (kReloadCancelled) or a death kept playing the whole wave.
4. **The own 1p reload stayed where it started.** Its emitter was the listener's position at the start. Every
   listener move re-sent that start position (`OneShotPool::resend_emitters`). Walking at 3 m/s, the last 1.4 s of the
   reload measured **-50.8 dBFS**, against -39.6 dBFS standing.
5. **Action and Bolt never posted.** Deploy/switch, zoom, dry-fire click and fire-mode switch were silent for everyone.
   The data has them on every kit firearm.
6. (Not fixed, noted.) `BypassHeadroom = IsFirstPerson` on the Handling output is not implemented by the mixer.
   Under BF3's rule the own 1p handling ignores the HDR headroom; ours obeys the window like every other output.

## 3. The fix

- `weapon_foley.{h,cpp}` (new): reads the foley inputs of a weapon program from its graph:
  - the event's samplers, its distance reach (the largest FadeEnd of the distance crossfaders on its path), its release
    event and the outputs its voices feed;
  - the IsShortReload / ActionType switchers' wave names.

  It is bounded (each register and node is visited once) and fails soft on hostile data (empty fields). Its result is
  `WeaponAudioDef::foley`, filled at load.
- Router (`weapon_audio.cpp`, PT1 rld insertions): posts the data's events where the weapon state changes.
  - **Own soldier**, on the sim's timed events, at their exact Predicted time (the same time the viewmodel clip of that
    event starts):
    - kReloadStarted posts Reload with IsShortReload = (magazine > 0);
    - kReloadCancelled posts the release;
    - kDeployStarted posts the release, then Action Deploy;
    - kFireModeChanged posts Action FireMode;
    - kBoltStarted posts Bolt.

    From the presented own weapons (`set_own_weapons`): the zoom's rising edge posts Action Zoom, and the tick flag
    kDryFire posts Action DryFire. The own 1p one-shots follow the listener.
  - **Others**, from their presented fields on the Render timeline:
    - a `reload_seq` change posts Reload (the magazine is not on the wire, so IsShortReload keeps the data's default
      0 = long);
    - a def change posts Action Deploy;
    - an `ads` rising edge posts Action Zoom;
    - a weapon change or a death during a reload posts the release.

    Each sits at the shooter's muzzle and is not started beyond the event's data reach (the crossfader would start
    nothing there).
- Voice cull (`patch_instance.cpp`, `// PT1 rld [live outputs]`): the loudness of an instance counts only the
  EnableHdr outputs that its busy sampler slots feed (voice send reachability, computed at create). Before any slot is
  busy (the first block), it counts the outputs the starter names (`set_loudness_outputs`: the router passes the
  event's outputs), else all of them, as before.

## 4. Measured after

(Filled in after the fix: section 5.)

## 5. Checks (one command each)

From the worktree, after `tools\build.cmd dev`:

- Trigger table, the data and the mix: `ctest --test-dir b\dev -R "^audio\.foley\.install$" -V`
