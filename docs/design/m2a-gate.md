# M2a gate pass (m2a.md §15.1 step 7)

The integrator's gate pass of M2a on `integ/w3` in `wt\integw3`, run on 2026-10-19. Every exit criterion of
m2a.md §1 (E1–E19) was run as defined there, in the running game: headless through the real `ServerHost` and
`ClientSession` on Metro, and windowed through Godot (rule 8: `OPENBF3_GODOT_WINDOWED=1`, after
`user-active.flag` was checked; it was absent for the whole pass). Every wall-clock or GPU row ran under the quiet
lock in the integrator's own tenures (the lock created without `-Force`, waited for when held, held at most 45 min,
released in a `finally`; `quiet.lock` does not exist after the pass). §1's noise policy (repeat once on a failure,
then WARN with the baseline) was never needed: no timed row failed.

Code base: the merge pass ran on 6d2492a (the fifth integration). The gate pass added two commits that change code
(below), so the strict branch pass and the merge-pass phases (`main-timing.ps1`: realtime + timing tenure, then the
window tenure) were run again on 0d54af0. The numbers below come from the gate's own `-V` runs (junit and
LastTest.log keep no output of passing rows); the logs stay in the session scratchpad (`m2a-gate\*.log`).

## Fixes made at the gate

| commit | row | what |
|---|---|---|
| bd36081 | E16 | `Bf3LevelSession::m2a_frame` printed the peak working set but not the VRAM that E16 asks for. It now prints `RENDERING_INFO_VIDEO_MEM_USED` (and textures / buffers) on the first frame after Done and 60 frames on. |
| 0d54af0 | E2 | E2's last clause ("the client's cosmetic flight of the same shot within 1 mm of the server's at 300 m") had no row: sv measured the server half and handed the client half to the client suite, whose case list had no E2 row. New timing case in `host.gunplay.client` (end block `// M2a gate`): from the sky lane the shooter looks down at Metro about 300 m out and fires 8 single M16A3 rounds; each client own-world `Impact` (Predicted timeline) is matched with the server's first static `ImpactEvent` of the same shot. `ClientProjectiles` keeps its in-flight positions inside, so the flights are compared where they end. Hip spread scatters the rounds (285 to 531 m out), so the case asks for at least 3 rounds whose impact is ≥ 300 m out and every compared shot within 1 mm. |

No threshold was changed.

## Rows

Commands run from `wt\integw3` after `tools\build.cmd dev`; windowed rows with `set OPENBF3_GODOT=<exe>` and
`set OPENBF3_GODOT_WINDOWED=1`. "Lock" marks a row run inside a quiet-lock tenure.

| row | threshold | measured | met | command |
|---|---|---|---|---|
| E1 server | 31 shots, first at the press tick, 30 gaps of 4,500,000, trace equal; other firearms per W1; semi taps 1 shot each | M16A3 31 shots, 30 / 30 gaps 4,500,000, first at press tick 57's start, trace 31 / 31; held gaps M4A1 4.5M, M27 4.8M, AK-74M / AKS-74u 5,538,462, RPK 5,142,857; M9, MP443, MK11, SVD 5 / 5 one shot per tap | yes | `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V` (lock) |
| E1 client / game | predicted = server 31 / 31 field for field; 31 1p muzzles | client (600 s loopback run): 31 / 31 equal, 30 / 30 gaps, 36 = 36 shots, 36 OwnShot. Game (range): 31 predicted = 31 server, worst field difference 0, 31 OwnShot, 31 viewmodel cues, 31 1p muzzles; M9 5 taps → 5 shots (held → 1), MK11 5 taps → 5 | yes | `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock) |
| E2 drop | 0.1779 ± 0.01 m at 100 m; 1.5981 ± 0.01 m at 300 m; expiry 975.0 ± 0.01 m | sky lane eye (−102.2, 204.1, −778.5), 1,000 m clear, 2,197.5 m in bounds; 100 m 0.1780 / 0.1785 / 0.1786; 300 m 1.5993 / 1.6030 / 1.6039; expiry 975.0000 × 3 (`Expired`); origin 0.0054 m below the teleport eye | yes | `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V` (lock) |
| E2 client flight | client cosmetic flight within 1 mm of the server's at 300 m | 8 shots, 7 with both impacts, 4 of them 300 m or more out (525–531 m), 7 / 7 within 1 mm, worst 0.0000 mm (one round had no impact on either end) | yes | `set OPENBF3_CL_TIMING=1&& b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client "--test-case=*E2 client flight*"` (lock) |
| E3 | M16A3 chest 4 / head 2 at 5 m, 6 / 3 at 60 m; 25 / 50 / 22.75 ± 1e-3; indicator once per hit | 4 × 25.0000, 2 × 50.0000, legs 5 × 22.7500, 60 m 6 × 18.4 and 3 × 36.8; one death event; in game the same counts and one hit indicator per server hit | yes | `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V`; `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock) |
| E4 | M16A3 head 50; MK11 head 117.5 at 5 m (1 shot), 2 at 60 m | 50.0000; MK11 1 × 117.5, 60 m 2 × 95.5938 | yes | same as E3 |
| E5 | server = predicted tactical 108M (commit +91.8M → 31), empty 142.2M → 30, M9 81M / 96M; game within 1 frame (early ≤ 1 tick) | server 108,000,000 / +91,800,000 (26 → 31), 142,200,000 / +120,870,000 (0 → 30), M9 81,000,000 / 96,000,000; client 7 / 7 timed events equal; game clip start 0 frames late, ready 0 to −1 frames, HUD ammo early by ≤ 3 frames = 0.0114 s (< 1 tick), fires at the finish, switch cancels with 28 kept; clip lengths printed (M16A3 2.333 / 2.900 s against 1.80 / 2.37) | yes | `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V`; `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock) |
| E6 | 4 distinct hashes; mismatch refused, `weapon_hash_mismatch` 1; KS D ≤ 0.0364; recoil within 1e-4°; 0 reconciliations | 4 distinct hashes; KS D 0.0158 (other disc 0.2605); recoil 0.230000° linear / 0.397755° exponential, diff ≤ 4.5e-9; 0 precondition failures; client refused "weapon table mismatch", mismatch 1, never spawned, 0 commands (kit hash refused too) | yes | `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V`; `ctest --preset dev -R "^host\.gunplay\.client\.unit$" -V` |
| E7 | 12 / 12 kit items bind; au's §10.2 rows pass on the game recording; short / late blocks and late commands 0; census, oracles, waves offline | 10 per burst everywhere; near − mid 45.28 dB, mid − far 13.54 dB, HPF mix 17.10 dB, noise tap 18.25 dB; pitch in [1.21, 1.262]; HPF at 100 m 140.60 Hz; M98B 325 m −93.0 dBFS, RPG-7 67.5 m −47.9, M1A2 32.5 m −67.3 / 90 m −96.7 in band; 12 items (10 drivers, 2 one-shots), 0 failures; short / late / late commands 0. Offline: census 835 graphs / 9,807 connections, oracles 6 / 6, waves 909 / 0 mismatches, loc 4,290 chunks / 0 mismatches, G3 0.1289 ms per channel-second (≤ 0.2), G6 629 resident 265.4 MiB, warm 48.6 ms | yes | `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock); `ctest --preset dev -R "^audio\." -V`; `ctest --preset dev-timing -R "^(audio\.decode\.perf\|audio\.bank\|audio\.game\.underruns)$" -V` (lock) |
| E8 | 0 compile failures; flash in [0.71, 0.79]; drawn = particles; oilfire 80 ± 1; named cells; atlas 52 / 52; cues | 72 preloaded, 0 failures; flash 759 / 1,003 = 0.757; drawn = particles on 29,405 / 29,405 frames; oilfire 80; concrete 500 / 500, brick 500 / 500, soldier 3 / 3 (no decal); Metro: no water (0 of 1,818 tags), Caspian water cell in `fx.install`; atlas 52 / 52, 0 fallbacks; tracers 1,003, shells 1,003, impact sounds 1,000 / 1,000, 0 double starts; `fx.unit` 18 cases | yes | `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock); `ctest --preset dev -R "^fx\.(install\|unit)" -V` |
| E9 | FX main p99 ≤ 1.0 ms; FX GPU ≤ 2.0 ms; lights ≤ 64; VFX2 16k ≤ 1.25 × reference | 64 bots, 2,913 bursts, 5,252 remote shots; FX main p99 0.5077 ms; FX GPU 0.195 ms (2.239 on − 2.044 off); ≤ 3 lights; ≤ 1,575 particles; vfx2 0.779 / reference lit + soft 0.761 = 1.024 | yes | `ctest --preset dev-window -R "^godot\.m2a_firefight$" -V`; `ctest --preset dev-window -R "^godot\.fx_ref$" -V` (lock) |
| E10 | 0 ammo mismatches at (1194, 669); gap ± 0.5 px; 12 / 12 names and icons; indicator per hit | 12 / 12 names, 12 / 12 icons; 1,119 frames, gap error 0 px over 1,118, ammo at x 1194.00, 0 mismatches; `ui.hud.feed.install` 12 rows, Ammo_01 (1194, 669); `godot.m2a_hud` pass | yes | `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock); `ctest --preset dev -R "^ui\.hud\." -V` |
| E11 | 8 / 8 loadouts; every item deploys, fires, reloads, server = predicted; M67 fuse 3.6 s | 276 / 276 rows, 0 ERROR; 8 / 8 loadouts; M67 216,000,000 units = 3.6 s, release to detonation 3.77 s; knife hits only enemies; events equal for every slot | yes | `ctest --preset dev-window -R "^godot\.m2a\.kits$" -V` (lock) |
| E12 | ≥ 99 % hit, ≥ 99 % same zone; control ≤ 50 %; sample equality (a) and (b) | `sim.lagcomp.unit` 8,900 rows, 0 differences, 1,780 rewound; Metro 500 / 500 = 100 %, zone 100 %, (b) 500 / 500 equal, late_sample 0; control 7 / 500 = 1.40 %; stress 33 / 33, 967 / 967 equal; residuals 0, window_extrapolated 0 of 27,018 | yes | `ctest --preset dev-timing -R "^host\.gunplay\.client$" -V` (lock); `ctest --preset dev -R "^sim\.lagcomp\.unit$" -V` |
| E13 | tick p99 ≤ 8.0 ms; rewind p99 ≤ 0.5 ms in both bench rows | tick p99 1.694 ms (mean 0.637), weapons + projectiles + rewind 7.5 %, 64 / 64 fired; pairs p99 0.2037 ms, long 0.1978 ms; 1,148 / 1,288 queries, 26.1 / 24.7 candidates, 0 / 2.04 cache hits | yes | `ctest --preset dev-timing -R "^host\.gunplay\.perf$" -V`; `b\dev\tools\bf3dump\bf3dump.exe lagcomp MP_Subway TeamDeathMatch0 --perf` (lock) |
| E14 | frame p99 ≤ 16.7 ms; M2a main p99 ≤ 3.0; crowd p99 ≤ 2.0; 0 underruns; 0 overflows | 6.06 ms; 0.906 ms; 0.216 ms; 0; 0 | yes | `ctest --preset dev-window -R "^godot\.m2a_firefight$" -V` (lock) |
| E15 | equal hashes for equal seeds, different otherwise; 0 weapon / motor reconciliations; same seeds for remote shots; late join same choices | 0 of 3,600 ticks differ (other seed 3,600 / 3,600); loopback 600 s: 0 motor, 0 weapon, 0 health rebases, 236 predicted / 422 server shots, 186 remote; two clients 212 / 212 seeds equal, origins within 1 cm; late join 87 starts over 25 shots identical | yes | `ctest --preset dev-timing -R "^host\.gunplay\.replay$" -V`; `set OPENBF3_CL_TIMING=1&& set OPENBF3_CL_LOOPBACK_S=600&& b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client` (lock); `ctest --preset dev -R "^fx\.triggers\.unit$" -V` |
| E16 | cold ≤ 15 s, warm ≤ 5 s; peak working set and VRAM printed | cold 5.38 s, warm 4.35 s; peak working set 1,561 MiB cold / 1,673 MiB warm; VRAM 1,668 MiB 60 frames after Done (textures 797.8, buffers 103.7) | yes | `ctest --preset dev-window -R "^godot\.m2a_load$" -V` (lock) |
| E17 | every malformed message refused, no crash; hostile client changes no other soldier, clamps counted, tick unaffected; install unchanged | fuzz 3,512 / 3,512 structured refused, 74,984 decoded stable, 739,166 refused, 0 unstable (dev and asan); hostile 0 / 900 ticks differ, slot clamped 900, 84 refused + 2 clamped of 86 shots, 90 / 90 KitSelects refused, tick p99 0.119 ms (control 0.121) | yes | `ctest --preset dev -R "^net\.proto\.fuzz$" -V`; `ctest --preset asan -R "^net\.proto\.fuzz$"`; `ctest --preset dev-timing -R "^host\.gunplay\.server$" -V` (lock) |
| E18 | steady ≤ 3 cm, transitions ≤ 5 cm; CrowdEvaluator ≤ 3 cm; 0 stand-ins | steady head / chest stand 0.013 / 0.002 cm, crouch 0.077 / 0.023, prone 0.179 / 0.044; transitions head 2.468, chest 3.090 cm; CrowdEvaluator stand 0.013 / 0.002, `anim.s3` capsules (b) the same; 10 / 10 rows, 0 stand-ins; run 13.5 cm reported | yes | `b\dev\tools\bf3dump\bf3dump.exe lagcomp MP_Subway TeamDeathMatch0 --deviation`; `ctest --preset dev -R "^anim\.(server_pose\|s3\.unit)$" -V` |
| E19 | FOV 55 / 40 within ±1 frame of 0.12 / 0.15 s; latency ≤ 0.1 s; recoil ± 1e-4°; springs settle; zoom refused deploying / sprinting | unit: sway 0.567, bob 0.833, kick 0.467, camera 0.500 s, FOV ±1 frame at 30 / 60 / 144 / 240 fps, recoil 1.6e-15°. Game: viewmodel FOV 55; ADS 40 in 0.1167 s, back in 0.150 s; latency 0.0167 s; recoil 1.36e-5°; springs 0.489 s; refused over 96 deploy frames and 145 sprint frames | yes | `ctest --preset dev -R "^anim\.viewmodel\.unit$" -V`; `ctest --preset dev-window -R "^godot\.m2a$" -V` (lock) |
| install unchanged | layout.toc, cas.cat and every .sb / .toc unchanged | 256 files unchanged in every install row of every phase; a read-only listing of the install afterwards: 1,918 files, newest dated 2026-04-25 | yes | the `install.unchanged` rows of each phase |
| branch pass | every row passes, no budget violation | 0d54af0: exit 0, 137 rows, 135 ran, 0 failed, 2 skipped (`core.cache_data`, `core.res`), 0 STRICT violations; 189 s | yes | `tools\build.cmd dev --strict` |
| merge pass | realtime, timing and window phases pass with `OPENBF3_TIMING_REQUIRE_RUN=1` | 6d2492a: realtime 10 / 10 (`host.server` passed on its rerun), timing 16 / 16, window 16 / 16; asan 12 / 12. 0d54af0: MERGE2 | yes | `main-timing.ps1 <wt> <tag>` (two lock tenures) |
| asan | listed rows pass under asan | 12 / 12 (net.proto.fuzz / unit, sim.lagcomp.unit, anim.viewmodel.unit, ui.hud.feed.unit, ui.hud.fuzz, m2a.contract, m2a.contract3, fx.render.unit, fx.triggers.unit, audio.router.unit, sim.weapons.fuzz) | yes | `tools\build.cmd asan` rows by `ctest --preset asan -R ...` |

## Playable check

**Command:** `tools\play.cmd` (Metro TDM, the assault kit on US, 24 firefight bots by default; `--bots=N` up to 32,
`--respawn=S`, `--kit=K`, `--team=T`).

**Scripted 5-minute play** (`ctest --preset dev-window -R "^godot\.m2a\.play$" -V`, under the lock): PASS, 300 s,
0 ERROR lines. 16 bots fired 8,769 remote shots; team kills US 67 : RU 56. The player fired 348 own shots, died 8
times (5 by enemies), respawned 8 times and switched teams; crowd, FX and audio built and mixed 300.9 s (12,444
impact cells); 0 event overflows. Screenshots 5 / 5 (hip, ADS, reload, firefight, death) plus the HUD and range
shots, copied to `b\shots\m2a-gate\scripted\` with E7's recording `gx_e7.wav`.

**Watched run** (`tools\play.cmd --bots=24 --respawn=3`, driven by real OS input through SendInput; captures at
1280 × 720 in `b\shots\m2a-gate\watched\`): the scene loaded in 5.2 s and ran at 180 fps with the HUD (ammo, weapon
name, crosshair, kill / death counters, team score) and the minimap with the bots on it. `01_spawn` shows the US
spawn; `02_firing` the M16A3 firing at a wall with impact decals and dust (31 → 13 rounds); `03_ads` the iron
sights; `04_reload` the reload clip with 3 rounds left shown red; `05_sidearm` the M9 (14 / 64); the loops show the
24 bots fighting (team score US 6 : RU 3 at the first shot, US 64 : RU 56 at the end) and the player emptying the
whole reserve (0 / 0 at the end). The scripted input moved the player little in that run (the US spawn room is
small and the walk path met its walls), and nobody reached the player there: 0 deaths. The first watched attempt
(kept in `watched\attempt1-stale-dialog\`) received no input because a stale `0xc06d007f` Application Error popup
from an earlier Godot process held the focus, but it shows the player killed by a bot (deaths 1) and respawned in
the subway hall. Moving, dying by enemy fire and respawning are covered by the scripted run above.

## What the user must judge (§17)

* **By ear:** the M16A3 near / mid / far bands and the distant high-pass through the game mixer
  (`b\shots\m2a-gate\scripted\gx_e7.wav`), the 1p vs 3p balance, the M98B, RPG-7 and M1A2 auditions, impact
  sounds, and the loudness of a 64-bot firefight without HDR (the `godot.m2a_firefight` scene; the playable scene caps
  its bots at 32: `tools\play.cmd --bots=32`).
* **Feel:** recoil kick and recovery springs, sway and bob, the ADS transition and both FOVs, the crosshair and hit
  indicator, the reload clips against `ReloadTime` (the authored clips are longer, for example M16A3 2.333 / 2.900 s
  against 1.80 / 2.37 s, so the clip is cut at the commit), deploy and switch timing, the key mapping, and how shots
  feel under "150/2".
* **Look:** muzzle flashes, tracers, impacts and decals against footage; 3p firing, reload, stance and death; the
  viewmodel per kit item (an M9 reload strip artefact was seen); the HUD shots in `scripted\` and the watched
  shots in `watched\`.
* **Decisions:** §16 Q1, Q2, Q3, Q4, Q12 and Q17 (Q17 is E9's relative GPU row replacing the plan's absolute
  0.3 ms) and Q14 (the inferred thresholds of E12's control row and E18's 3 cm / 5 cm gates); accepting 909 Metro
  waves against the plan's 908; whether a WARN on a noisy-machine row would block (none occurred here); the
  mechanics variant once S5 Session 1 is measured (UniformArea + Linear ships with `measured = false`). Also gx's
  wording change to E19 (b4a3899: the press-to-presented latency gated ≤ 0.1 s instead of frame-exact) and the
  R6 / R7 decisions recorded in 0009ec3.

## Open issues

* A stale `0xc06d007f` Application Error popup (owned by csrss, left by an earlier Godot process of unknown origin)
  was found on the desktop during the watched run (a dialog probe saw it at 0 s, before its own Godot had started).
  After it was dismissed it did not come back: a 60 s probe of the playable scene, the second watched run and every
  later windowed row ran without it.
* `host.server` (realtime lane) needed its rerun on 6d2492a (the 2-of-5 rerun rule in `realtime-reruns.tsv`).
* In the scripted play, shot ids are reused across a respawn, which gives 11 double starts of impact cues; 8 late
  audio commands and 1 weapon reconciliation are reported (not gated in the play row).
* The player made 0 kills in this play run (reported only; the gx branch run had 2).
* The play row's ADS screenshot was the unframed fallback (no bot in view at that moment).
* `godot.m2a` (391 s) and `godot.m2a.kits` (159 s) are over the 120 s window budget.
* `kMaxLagSlots`: lag-history slots 64 and above are never hit by the 64-bot runs.
* E13's 120 s run is simulated time (4.59 s of wall time), not a real-time server run.
* E16's warm load has a 0.65 s margin (4.35 s against 5 s); the branch's unlocked run read 5.42 s.
* `host.gunplay.server` is not in the asan row set.
* The E2 client case sits after the client suite's install-unchanged case (doctest runs cases in file order);
  it reads the install only through the shared fixture.
