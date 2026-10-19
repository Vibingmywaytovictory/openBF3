# M2b results: exit criteria E1-E14

The measured result of every M2b exit criterion (docs/design/m2b.md §1), after both waves, on `integ/m2b-w2` (`m2b/w2`
4a7e9f4 with ui, cam and gx merged; `main` 545640c was already in it). Each row names its threshold, the value
measured and the log it comes from. Logs are in the session scratchpad (not in git):

* `m2b-integ-w1\` (wave 1, on `integ/m2b-w1`): `final-branch-pass.log` (the functional rows), `timing.log` (the
  wave-1 timing phase), `e8-remeasure2.log` (E8 after `[ray any]`), `e11\timing-e11.log` (E11 after the four mix
  insertions), `e13-diag3.log` (E13 alone).
* `m2b-ru-verify\asan.log`: ru's acceptance asan run (E7's asan half).
* `m2b-integ-w2\` (wave 2, this branch): `branch-pass.log` and `ctest-dev-branch.xml` (the functional rows, the junit
  holds each row's first 1,024 bytes of output), `window-phase.log` and `ctest-dev-window.xml` (the wave-2 merge pass:
  the window phase), `e12.log` (E12 with its whole output, see the note under the table), `play-2.log` (the play check
  through `tools\play.cmd`) and `play-1-deploys-fail.log` (its first run, which found a script bug, fixed below).

The functional rows (E1-E7, E10) ran in this branch's dev pass, which holds every merged track (`tools\build.cmd dev`:
151 rows, 149 passed, `core.cache_data` and `core.res` skipped as before, 0 failed, 199.8 s ctest wall). Wave 2
changes no server, bot, net or audio code, so their values equal wave 1's where both printed them (E1's RoundOver
tick 17,634, E6's trace hashes).

| id | criterion | threshold | measured | met | log |
|---|---|---|---|---|---|
| E1 | TDM round to 100 through the interpreter, 1 peer + 31 bots | `RoundOver` once at 100 with the peer's MatchState equal; no runtime fault; `RoundNatives` coverage 0 `Recorded`; within 21,600 ticks; restart in place at 0 / 0 | RoundOver once at tick 17,634, winner team 1 (100 / 95, wave 1's print; the same tick here); 21,473 ticks run incl. the 60 s round end and the restart; coverage check passed; revives 5, stuck 0, sight 0, stale aim 0, spawns with an enemy within 10 m 0. Row passed, 33.2 s | yes | m2b-integ-w2\ctest-dev-branch.xml (`host.tdm.round`) |
| E2 | bots-only rounds, K/D, no wall-hacks, no stuck bots | 3 seeds to 50: all reach RoundOver; pooled K/D in [0.75, 1.33]; 0 sight / stale-aim violations; 0 stuck; 0 scored spawns with an enemy within 10 m; revives >= 1 | row passed (24.5 s); seed 1: RoundOver at tick 8,235, 50 / 42 (K/D 1.190), sight 0, stale aim 0, stuck 0, enemy-near spawns 0; pooled 149 / 131 = 1.137 (wave 1, the same bot code) | yes | m2b-integ-w2\ctest-dev-branch.xml (`host.tdm.bots`); README "Sixth integration" |
| E3 | health, regen, spawn immunity | no regen before T + 600, then 1/12 hp per tick ± 1; immunity 120 ticks, Fire / move end it; server = local SoldierMotor | first regen at T + 600, 61 steps of 0.08334 hp; immunity +120 idle, +30 on Fire and on move; 0 protect_ticks mismatches (tick counts, identical every run). Row passed, 8.2 s | yes | m2b-integ-w2\branch-pass.log (`host.tdm.server`); values: m2b-ru.md §7 |
| E4 | death and respawn flow | respawn_tick = death + 300; deploy-screen Ready spawns nothing; SpawnRequest refused before, accepted at respawn_tick, Spawned within 1 tick with the kit; M2a hashes unchanged without rules | +300; R10 0 spawns after Ready; request at 736 refused TooEarly, at 737 accepted, Spawned in the same tick with kit 3; M2a kit / weapon / motor hashes equal | yes | as E3 (`host.tdm.server`, `respawn*`) |
| E5 | revive | a teammate's defib in the window: new life at the corpse, 20 hp, `Spawned.revived = 1` (bot and peer); enemy count -1; reviver +100; nothing after the window, after deploy, from an enemy, at a living soldier | bot and peer revived +78 / +72 ticks after death, 20.00 hp, 0.000 m from the corpse, enemy kills 1 -> 0, medic 0 -> 100; the four refusals hold | yes | as E3 (`host.tdm.server`, `revive*`) |
| E6 | determinism | same seed twice: 0 differing ticks (trace, world, MatchState / PlayerList bytes); another seed differs | 0 / 3,179 ticks differ; trace 4d83d6b06167a76c twice, 207d9dbf501507b8 for the other seed (3,179 ticks differ); match bytes 1aa9f093a3bfb417 twice. Row passed, 21.5 s | yes | m2b-integ-w2\ctest-dev-branch.xml (`host.tdm.replay`) |
| E7 | hostile input | protocol fuzz with the v3 messages: 0 crashes, 0 asan reports; the server refuses and counts the hostile SpawnRequests; a v2 client gets `Kick` | `net.proto.fuzz` passed (dev); `host.tdm.server` `hostile*` passed: 10 requests, 2 accepted, every refusal counted by kind, 300 hostile datagrams 298 counted bad + 2 judged, v2 Hello kicked; under asan `net.proto.fuzz` (25.1 s) and `host.tdm.server` (154.7 s) passed with 0 reports | yes | m2b-integ-w2\branch-pass.log; m2b-ru-verify\asan.log |
| E8 | perception and bot budget, 64 bots, 120 s | perception <= 0.2 ms, brains <= 1.5 ms, tick <= 4 ms at p99 | 0.180 / 0.298 / 1.319 ms (after `[ray any]`; the first timing phase measured 0.257 ms perception and failed) | yes | m2b-integ-w1\e8-remeasure2.log |
| E9 | client screens fed from the game | `--m2b-check=play --kill-limit=15 --preround=0` in a window: spawn screen at join and after each own death, Deploy spawns the kit; feed lines = deaths; scoreboard = MatchState; kill card names killer and weapon; round end on RoundOver; 0 missing lookups; 0 ERROR lines | `godot.m2b.play` passed in the window phase, 59.7 s (M2B CHECK: PASS, 0 ERROR lines, no "waiting for" row under `OPENBF3_TIMING_REQUIRE_RUN=1`; its row values were cut by ctest, see the note). The play check through `tools\play.cmd` (the same check, pre-round 10 s): 14 / 14 rows, 2 own deaths (killers 8 and 0), 1 revive by a bot, 2 deploys with the chosen kit (assault, engineer), 22 == 22 feed lines, 0 of 151 Tab samples off, 2 kill cards 0 bad, 3 spawn screens, round end at 45.9 s (US 15 - RU 5), 0 missing lookups, 2 death views 0 bad inputs, install unchanged | yes | m2b-integ-w2\window-phase.log; m2b-integ-w2\play-2.log |
| E10 | HDR window oracle | 71 dB alone: 80 / 35 (± 0.5 dB); 110 dB shot: top 110 within 0.05 s ± one 512-frame block, bottom 65, a 63 dB source suspended; release on the spline; the scope rows | attack 80 -> 110 dB in 5 blocks = 53.3 ms (limit 50 + 10.7 ms); release 110 -> 80 in 49 blocks = 522.7 ms; every case passed | yes | m2b-integ-w2\ctest-dev-branch.xml (`audio.hdr.unit`) |
| E11 | 64 bots firing: audio thread | mix CPU / block <= 0.10 at p99; 0 late blocks; live voices <= 128 | p99 < 0.08 (p50 < 0.08), 0 late blocks, 128 voices at most, max block 1.505 ms of 10.667 ms (after the four mix insertions; the first timing phase measured p99 < 0.20 and failed) | yes | m2b-integ-w1\e11\timing-e11.log |
| E12 | load | warm load of `m2b_metro.tscn` <= 5 s | warm 4.27 s from process start to the first controllable frame (cold 5.46 s); the row runs `--bots=0 --deploy-screen=0` (the M2a E16 precedent). With the play defaults (31 bots, spawn screen) the load to the spawn screen is 5.84-5.99 s (two play runs, warm user cache): over the limit, see the open items | yes, as the row measures it | m2b-integ-w2\e12.log; m2b-integ-w2\play-2.log (`[m1] done:`) |
| E13 | the real client path | ServerHost + ClientSession, deploy screen, freeze, death stamps, RoundOver, <= 30 s | passed in 26.16 s (timing phase) and 26.24 s alone: respawn = death + 300, RoundOver after 11.3 s of round; 31 bots instead of 7 (m2b.md `## M2b integ-w1`) | yes | m2b-integ-w1\timing.log; m2b-integ-w1\e13-diag3.log |
| E14 | (M2a's) 0 underruns with the A3 mixer in the stream | `godot.m2a_firefight`: 0 audio underruns, 0 event overflows (and M2a's frame gates, not re-gated by M2b) | row passed in the window phase, 82.9 s: every gate it checks held, so 0 underruns and 0 overflows (the exact frame numbers were cut by ctest, see the note) | yes | m2b-integ-w2\window-phase.log |

**Note on the window phase's numbers.** ctest keeps only the first 1,024 bytes of a passed row's output, in its
junit file too, so the window phase kept the verdicts but not the printed numbers of E9 and E14. `godot.m2b_load` was
rerun once with `-V` for E12's value (`e12.log`), and the play check gives E9's values. tools/timing_pass.cmake now
passes `--test-output-size-passed` (the named replacement `[passed output]`), so later merge passes keep them.

**No timing phase in the wave-2 pass.** m2b.md §11.1 step 5: it runs only if the wave-2 diff changed a source file
that a timing row links and executes. The diff's non-Godot sources are `crowd_eval.{h,cpp}` (bf3_anim),
`triggers.cpp` (bf3_fx, R13), `hud_feed.h` / `hud_feed_tdm.cpp` (bf3_host), `hud_model.h` (bf3_core) and
`viewmodel_set.{h,cpp}` (bf3_render). No timing row's code calls `CrowdEvaluator`, `FxTriggers`, `fill_hud*` or the
viewmodel set (the timing rows are host.*, sim.*, audio.*, logic.perf and four bf3dump commands: prepare, ebxload,
shapes, fx bench), so none ran.

**All 15 window rows** passed in one tenure under the quiet lock, user away (15:09-15:25): s3_crowd 270.6 s, l1_look
118.6 s, fx_ref 182.0 s, m2b.play 59.7 s, m2a_firefight 82.9 s, m2a_load 55.0 s, m2b_load 55.3 s, m1_load 49.2 s,
audio 33.2 s, fx3_demo 24.9 s, fx_demo 27.4 s, ui_fonts_shots 2.8 s, ui2 4.0 s, m2a_hud 5.2 s (and the setup
fixture). The three retired M2a rows were not run.

## The play check (user away, one scripted round through tools\play.cmd)

`tools\play.cmd --m2b-check=play "--m2b-shots=<openBF3>\b\shots\integm2bw2\play2" --kill-limit=15` (the play
defaults otherwise: 31 bots, the spawn screen, the 10 s pre-round). The first run failed `play.deploys` (1, want 2):
a bot revived the player 4.5 s after its death, the script stopped seeking after the first death, and the player
stood to RoundOver without dying again. The named insertion `[revived seek]` keeps the seek on until a death is
followed by a deploy. The second run passed 14 / 14. What the captures (`b\shots\integm2bw2\play2\`, gitignored) show:

* `gx_m2b_join`: the spawn screen over the level with the four kit cards (ASSAULT selected, the loadout icons), the
  DEPLOY button, the pre-round box "ROUND WILL START IN 9" and the team counter RU 0 / 15 0 US.
* `gx_m2b_scoreboard`: two teams of 16, every bot marked BOT, the player's row highlighted, K / D / score / ping.
* `gx_m2b_killcard`: the death view pulled back from the body with the killer in frame behind a bin, the kill card
  "KILLED BY bot", ENGINEER, the M4A1 icon, health 100, 6 M; the kill feed top right.
* `gx_m2b_spawn`: the spawn screen after the death with "REVIVE POSSIBLE 9" and "TIME TO DEPLOY 1"; the own corpse
  lies prone in the street with teammates crouched around it.
* `gx_m2b_round_end`: "YOUR TEAM LOST!", RU 5 / 15 US 15 WINNER, the top three (310, 310, 210).

Visible issues, for the user's playtest: the minimap covers the "DEPLOY AS" label and the left part of the ASSAULT
card; the M1 debug overlay (four lines at the top left) still shows and the team counter is drawn over its second
line; the Tab scoreboard is drawn over the pre-round box.

## Open items

* **E12 with the play defaults is over 5 s** (5.84-5.99 s warm, 31 bots and the spawn screen; the row's `--bots=0
  --deploy-screen=0` load is 4.27 s). The play runs' stage times show the scene lanes (meshes 4.7-4.8 s against
  3.2 s in the row) take the extra time, likely the 31 bots' setup running beside them (not profiled). Not fixed here; the M2a E16 precedent times the load without bots.
* The minimap / spawn-screen overlap, the M1 debug overlay and the `[m2a] firefight bots: 31 (free)` line in an M2b
  session (m2b-gx.md §3).
* Every mechanic m2b.md marks `measured = false` stays a named default (no VU sessions).

## The user's playtest (m2b.md §1.1)

From the main checkout once this is merged (or from `wt\integm2bw2` now):

```bat
tools\play.cmd
```

The level loads to the spawn screen with the 10 s pre-round countdown. Pick a kit (1-4 or a click), Enter or Deploy;
fight; die once (kill card, death view, then the spawn screen with the timer and "revive possible"); hold Tab; play
assault and use the defibrillator on 3; let the round end. `tools\play.cmd --kill-limit=20` gives a shorter round,
`--preround=0` skips the countdown, `tools\play.cmd --help` lists the keys. The named defaults are in
`config/tdm_tuning.cfg` and `config/bot_tuning.cfg`.
