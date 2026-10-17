# M2a cl: client prediction and events, as built

Track "M2a cl" of M2a wave 2 (docs/design/m2a.md §15, row cl), branch `m2a/cl` from `m2a/w2` f4bbc43 (the W2.0
contract with nt and lc merged). Scope: m2a.md §6.1–§6.3 and §7.2–§7.4 on the client side, §8, §13.3's client rows,
the named replacements R1 and R2, and the named insertions in `prediction.cpp` (`catch_up`) and `client_session.cpp`.
The README's "M2a cl" section repeats the gates.

Tags as in m2a.md: **[verified]** read from code or a measurement in this tree on 2026-10-17; **[inferred]** a choice
or estimate made here.

sv (the server gunplay) is not merged. Until it is, every end-to-end case runs against `m2a_fake::GunServer`
(`tests/host_gunplay_client_tests.cpp`), described below. sv's merge replaces it with the GameServer itself.

## What landed

| file | what |
|---|---|
| `src/bf3_host/include/bf3/host/presentation_events.h`, `src/bf3_host/src/presentation_events.cpp` | `PresentEvent` (64 B), `PresentationEvents` (SPSC ring, 2,048 entries, overflow counter), the timelines (Predicted / Render / Immediate), `effect_seed` and `impact_seed` (§4.4) |
| `src/bf3_host/include/bf3/host/presentation_weapon.h`, `src/bf3_host/src/presentation_weapon.cpp` | `PresentedWeapon` and `present_weapon(prev, curr, tick, fraction, table)`: phase and progress, ammo, spread and recoil lerped between the two predicted ticks, zoom; no lerp across a change of item; a hostile view (def outside the table, active slot out of range) gives `valid = false` |
| `src/bf3_host/include/bf3/host/client_projectiles.h`, `src/bf3_host/src/client_projectiles.cpp` | `ClientProjectiles` (§8.4): the own world on the predicted timeline against the rewound records, the remote world on the render timeline, Impact and SoldierContact events, the E12 log (`OwnTestSample`), `shot_event_of(RemoteShot)` |
| `src/bf3_sim/include/bf3/sim/prediction.h`, `src/bf3_sim/src/prediction.cpp` | `WeaponLane` (the weapons ring beside `PredictionRing`), `WeaponReplay`, `WeaponEmitSink`, `PredictionRing::attach_weapons`; the named insertion in `catch_up` (the weapons step after the motor step) |
| `src/bf3_host/include/bf3/host/client_session.h`, `src/bf3_host/src/client_session.cpp` | additive blocks (config, stats, frame fields, `ClientGunplayInputs`, `build_client_weapon_table`, `client_weapon_hash`, `present_others` with `PresentOthersCounts`, `set_view_sample`, `present_view_quality`), the `Impl` member block and the end block (`cl_*`), the named insertions, R1 and R2 |
| `tests/host_gunplay_client_tests.cpp` | `host.gunplay.client.unit` and `host.gunplay.client`, `m2a_fake::GunServer` |
| `tests/host_session_tests.cpp` (block at the end) | R1's equivalence against M1's `present_others` body |

## The client, step by step

* **MatchInfo (E6).** `ClientGunplayInputs` (the weapon table, the capsule pose table, the kit table, the shot
  quantisation frame, a `load_mechanics(path)` hook) come with the `ClientSessionConfig`; `mechanics_cfg` is parsed
  by the client itself through the hook. On MatchInfo with a `weapon_hash`, the client builds its table
  (`build_client_weapon_table`: a copy with the parsed `Mechanics`, `exact_aim` and the infinite-magazine flag from
  `weapon_flags`, rehashed) and compares `client_weapon_hash = weapon_table_hash ^ capsule table hash` with the
  server's. A missing table, an unreadable cfg or a different hash refuses with "weapon table mismatch" and
  `weapon_hash_mismatch` = 1; a different `kit_hash` (when the server sends one) with "kit table mismatch" and
  `kit_hash_mismatch` = 1. A refusal sends Bye straight on ENet, disconnects, publishes its stats (no tick runs after
  it) and never sends Ready. A matched client sets up the weapon lane, the projectiles, sends KitSelect (optional)
  and then Ready. A MatchInfo without a `weapon_hash` (an M1 server) sends Ready with gunplay off.
* **Prediction.** After the motor step of each first prediction, the lane steps `step_weapons_m2a` with the same
  command, the states before and after, the match seed and the life's spawn tick (a dead soldier before the tick
  carries its weapons with no events, the server's rule). Only the first prediction of a tick emits (the lane keeps
  `emitted_through`); `catch_up` replays through `WeaponReplay` and never emits again. Each emitted shot is spawned in
  the own projectile world with the view of its command.
* **Reconciliation (§8.2).** On a Snapshot whose motor state matches (M1's epsilons, health left out by R2), the
  client compares `weapons_wire_view(predicted at T)` with the own weapons block: a difference rebases the lane from
  the wire view (`weapons_from_wire`, validated against the table) and replays (`weapon_reconciliations`); no
  predicted weapons at T rebase without a compare (`weapon_ring_misses`). Only the health differing is R2's health
  rebase: the ring's health from T on moves by the difference, no replay (`health_rebases`). A motor mismatch
  reconciles as in M1 and the weapons are rebased from the wire view first.
* **Spawn.** Spawned carries kit and team; the life's loadout comes from `kits.loadouts[team - 1][kit]` through
  `spawn_soldier_weapons` (the shot index carried over from the previous life). A life that arrives in a Snapshot
  before its Spawned adopts the wire view. A Spawn event goes out on the predicted timeline.
* **The view sample (§6.2).** The main thread puts the drawn frame's render time, quality and `newest_seq` into the
  `InputSample` (`set_view_sample`); the command made from it carries `view_back_q8 = (tick << 8) −
  round(render × 256)`, clamped to 0..65,535, `view_quality` and `view_seq8`. With no drawn sample the client's newest
  frame at sampling is used. `fire_presses` adds `kFire` to the next command once per press (a tap shorter than a
  tick is not lost).
* **Received records and acks (§6.3, §7.2).** `ReceivedRecords` keeps the records of the last 64 seqs as they arrived
  (main Snapshot and SnapshotOthers parts, per slot). `SnapshotOthers` merges into the drawn tracks by M1's
  `add_others` rules, and `others_ack_bits` (bit i = newest − i) acks the SnapshotOthers parts. Each command stores the
  received state (main and others masks) at first send; the own projectile world reads its candidates from it, so
  the client tests a target from exactly the records the server will use.
* **Shots and combat (§7.4).** ShotEvents become RemoteShot events (render timeline, effect seed from the match seed)
  and remote projectiles; CombatEvents become Hit (Immediate when the own soldier is victim or shooter), Death and
  Detonation events. A decoded value naming a slot ≥ `kMaxSlots` or a def outside the table is dropped and counted
  (`bad_gunplay_events`); a datagram that fails to decode is counted (`bad_gunplay_datagrams`); a refused own weapons
  view is counted (`bad_weapon_wires`).
* **The residual counters (§6.1, §6.3).** `late_sample` (an arrival at or below a sent firing command's view seq),
  `generation_reorder` (a reordered older generation reset a drawn track), `others_ahead` (a drawn SnapshotOthers
  record newer than `newest_seq`'s tick), `track_timeout`, and `window_extrapolated` in `present_others`'
  `PresentOthersCounts`.

## Contract rules that proved wrong or incomplete (fixed, each in its own commit)

* **The named insertion list (ee3bc8f).** §15 named `handle_datagram` and `publish` in `client_session.cpp`. The
  client also needs one-line calls in `authoritative` (the spawn's weapons, the ring-miss and reconcile rebases, the
  weapons compare and R2's health rebase in the match branch), in `client_tick` (the view sample, `fire_presses` and
  the received state after `sample_command`; the weapons after the motor step; `others_ack_bits`) and in `start`
  (Ready moves to MatchInfo so a refused client never sends it, E6), plus one member block in `ClientSession::Impl`
  and `PredictionRing::attach_weapons`. §15 now lists them. Insertion commits: 32b7dd3 (catch_up), 6c8365b
  (handle_datagram), d381d53 (publish), a6ebaaa (authoritative, client_tick), dabc46d (start); R1 7eda6dc, R2
  0cc492d.
* **A refused client's stats (7ecb98f).** `stats()` reads the published frame, and only a client tick publishes. A
  refusal disconnects before the first tick, so it now publishes its stats itself.

## m2a_fake::GunServer (until sv)

M1's GameServer (motor, jitter buffer, spawns, snapshots, clock reports) on its own ENet loop at 60 Hz, wrapped with
the gunplay a v2 server adds:

* the applied command per slot mirrored from GameServer's rules (receive_commands' window and sequence rule, the
  repeat of the last command, hold at the spawn), with lc's `AckState` at the command's first arrival;
* `step_weapons_m2a` after every motor step with the before/after states (dead skip as the lane), spawns detected by
  a life change (loadout kit 0 of the soldier's team), one `ProjectileWorld` against lc's real `LagHistory` /
  `RewindProvider` (the trace rows E12 (b) compares, `shot_view` per shot);
* MatchInfo rewritten with the match fields (seed, `weapon_hash` from the same `build_client_weapon_table`, flags,
  kit hash, max rewind), Spawned with kit and team, the Snapshot with the own weapons block, an optional split into
  SnapshotOthers, `record_tick` / `sent` for the history, ShotEvents and CombatEvents per client since its last
  snapshot, a virtual health loss (R2) and an optional hostile mode (mutated copies and out-of-range values);
* bots through `add_local_soldier`: a target strafing ± 4 m at the stand speed across its spawn heading, and bots
  firing bursts.

Its MatchInfo hash is built by the client's own function from the same table, so E6's "matched" row only shows that
the client accepts an equal hash and refuses a different one. The mechanics cfg files are a fake loader
(`server.cfg`, `variant.cfg` with the other disc distribution, `missing.cfg` unreadable) until sv's
`parse_mechanics_tuning` is the hook.

## Gates (measured 2026-10-17, dev preset)

The wall-clock rows run under the quiet lock and are **noisy** on this shared machine. `host.gunplay.client` skips
(77) unless `OPENBF3_CL_TIMING=1`, because `tools\build.cmd`'s TIMING regex (not cl's file) does not list it; the
unit row is short and runs in the default test step.

| gate | command | threshold | measured | met |
|---|---|---|---|---|
| E1 (client side) | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E1 + E5*"` (quiet lock) | predicted `ShotEvent`s equal the server's field for field 31/31; 30 gaps of 4,500,000; first shot at the press tick's start; one OwnShot event per predicted shot | 31/31 equal (time, spawn time, seed, ids, origin, aim, direction, spread, cone, recoil, tracer, power by bits; and the tick); 30/30 gaps; first at the press tick's start; 36 OwnShot events for 36 predicted shots (the 31-round magazine, then 5 after the reload); 0 motor and 0 weapon reconciliations | yes |
| E5 (client side) | same case | server phase events = predicted phase events; M16A3 tactical 108,000,000 with commit at +91,800,000 → 31, empty 142,200,000 → 30 | 7 / 7 timed events equal (time, event, weapon slot, tick); empty 142,200,000 → 30 rounds, tactical 108,000,000, commit +91,800,000 → 31 rounds (from the server's own weapons blocks) | yes |
| E6 (client side) | `ctest --preset dev -R host.gunplay.client.unit` (case "E6") | a mismatched client disconnects on MatchInfo with "weapon table mismatch", `weapon_hash_mismatch` = 1, never sends Ready, never predicts | variant.cfg (other disc distribution): refused "weapon table mismatch", `weapon_hash_mismatch` 1, Ready 0, commands 0, no frame; missing.cfg and a client without a table: the same with the reason appended; a different kit hash: "kit table mismatch", `kit_hash_mismatch` 1; the matched client: Ready 1, gunplay on; an M1 server (no `weapon_hash`): Ready 1, gunplay off | yes |
| E12 (b) lagcomp | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E12 lagcomp*"` (quiet lock) | 150/2, a target strafing at 4 m/s over 8 m at 20 m, 500 single shots: the server hits ≥ 99 % of the shots the client's own test hit, same zone for ≥ 99 % of those; every sampled target's `OtherPose` equal to the server's Trace bit for bit except `late_sample`; residuals reported | 500 shots, the client's test hit 500, the server 500 (100 %), same zone 500 (100 %); (b) 500 / 500 samples bit-equal, 0 mismatched, `late_sample` 0 (another 500 client samples have no server row: the server's swept-bound broad phase culled those segments before posing; the comparison covers every row both ends posed); server verdicts 500 rewound, 0 clamped, 0 refused; residuals `generation_reorder` 0, `others_ahead` 0, `track_timeout` 0, `window_extrapolated` 0 of 26,582 draws | yes |
| E12 control | same case | the server without rewind (`max_rewind_ticks` 0) hits ≤ 50 % of the client's test hits | 8 of 500 (1.6 %), all clamped to the present | yes |
| E12 stress | same case | reported | "lat=150-165,loss=2" (RTT 321 ms): client test hit 35, server 35 (100 %), same zone 100 %; (b) 965 / 965 equal; every shot clamped to 21 ticks on both ends (view lag > 350 ms) | reported |
| E15 loopback | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E15 loopback*"` (quiet lock; `OPENBF3_CL_LOOPBACK_S`, default 600) | 0 weapon and 0 motor reconciliations in 10 min of scripted play without netsim; health rebases reported | 600 s (walk, turn, bursts and taps, zoom, reload, pistol and back, crouch, jump, sprint; a strafing and a firing bot): 0 motor, 0 weapon reconciliations, 0 weapon ring misses, 0 step precondition failures, 0 lane gaps, 0 event overflows; 236 predicted shots, 234 timed events, 36,003 lane steps; health rebases 15 for 60 virtual hits (the health floors at 1 after 15, so later hits change nothing) | yes |
| E15 two clients | `... --test-case="*two clients*"` (quiet lock) | the second client joins after 30 s (every bot slot has fired); both derive the same effect seed for every remote shot from the join on | B saw 216 remote shots after its join, A saw all 216 (as remote or own), 216 / 216 equal effect seeds, origins within 1 cm; both bots fired before the join | yes |
| R1 | `ctest --preset dev -R host.session.unit` (case "M2a cl R1") | `present_others` through `net::sample_other` draws what M1's body drew | 150,196 drawn soldiers on 50,000 random frames, 150,196 bit-equal in every M1 field (11,237 bracketed, 16,292 before the oldest, 122,667 extrapolated of which 54,964 held past 250 ms) | yes |
| R2 | `ctest --preset dev -R host.gunplay.client.unit` (case "R2") | a server hit that changes only the health is a health rebase, not a reconciliation | 8 health rebases for 8 virtual hits, 0 motor and 0 weapon reconciliations, Hit events Immediate with `kOwnVictim`; SnapshotOthers 121 received of 121, 0 acks ignored by `LagHistory` | yes |
| events and presented weapon (vm, hud, fx3, au3) | `ctest --preset dev -R host.gunplay.client.unit` | the ring keeps order and counts a full ring; `present_weapon` lerps and refuses hostile views; the frame carries the two predicted wire views | 1,000,000 events in order across two threads; 5 overflows counted on a full ring; present_weapon rows (deploy progress, ammo, spread / recoil lerp, zoom, no lerp across items, hostile def and slot refused) pass; events seen end to end: OwnShot, OwnTimed, RemoteShot, Impact, SoldierContact, Hit, Spawn (E1, R2, E12, loopback) | yes |
| hostile gunplay data | `ctest --preset dev -R host.gunplay.client.unit` (case "hostile") and the asan preset | mutated and out-of-range gunplay datagrams refused and counted; the session keeps predicting | 390 mutated copies: 288 refused by the decoders (`bad_gunplay_datagrams`), 15 out-of-range shots dropped (`bad_gunplay_events`), 15 own weapons blocks with a def outside the table refused (`bad_weapon_wires`), still connected and predicting, 0 step precondition failures | yes |
| M1 host.session | `ctest --preset dev -R "^host\.session"` (quiet lock) | the M1 rows still pass | host.session.unit passed (70.4 s), host.session passed (191.9 s) | yes |

Other rows on this branch:

* Full ctest without the timing rows (`tools\build.cmd dev`, which excludes godot.*, host.session*, host.server,
  host.gunplay.perf, sim.nav.bake, sim.weapons.perf, tools.fx_bench, the audio and logic timing rows): 94 rows, 91
  pass, `core.cache_data` / `core.res` skipped as before, `host.gunplay.client` skipped (77) without
  `OPENBF3_CL_TIMING`. `host.gunplay.client.unit` takes 12-17 s (ENet loopback, wall clock but not timed).
* Timing rows under this track's quiet lock, one run each: `host.gunplay.client` (all four cases, 987 s; the
  two-clients case first missed 4 of 220 shots because the test compared only A's events after the join and stopped
  draining A one second early, a test fault fixed in 2cc2660 and rerun: 216 / 216), `host.session.unit` (70.4 s) and
  `host.session` (191.9 s) pass.
* asan preset: `host.gunplay.client.unit` (with the hostile case), `host.session.unit` (with R1's case) and
  `m2a.contract` pass with no AddressSanitizer report.
* The install is not read by any cl case (synthetic floor); `host.session`'s Metro rows end with `install.unchanged`
  clean.

## Open limits

* **No `exact_aim` yet.** `WeaponTable::exact_aim` has no effect until sv's R3, so E12's shots carry the M16A3's
  spread and recoil: the test aims down the sights (`kZoom`) and fires once per 12 ticks to keep the spread small.
  The gate compares the client's cosmetic test with the server's verdict for the same seeded shot, so spread does
  not weaken it; it only lowers the share of shots that hit.
* **Stress clamps.** At "lat=150-165,loss=2" the view lag (RTT ≈ 322 ms plus the interpolation delay) is past 350 ms,
  so every shot is clamped to 21 ticks on both ends (the client's own test uses the same clamp). The hit agreement
  is therefore expected; the row is reported, not gated.
* **E12 control** uses the client's own test with the default 21-tick rewind (`own_test_max_rewind_ticks`) against a
  server with `max_rewind_ticks` 0, which is what "without rewind" means for the hit rate.
* **Kits.** `kit_table_hash` and `KitTable::loadout` are sv's; the client checks `KitTable::hash` when the server
  sends one and indexes `loadouts[team - 1][kit]`. The fake kit table gives every kit the M16A3 and the M9.
* **E1 / E5 from a predicted client only.** The server rows of E1 and E5 (`host.gunplay.server`) and the
  viewmodel / HUD rows (`godot.m2a`) are sv's, vm's and hud's.
* **ClientGunplayInputs supply.** Until sv lands, the caller fills `ClientGunplayInputs` (the tests do); sv's
  `SharedLevel` additions should supply the table, capsules and kits for the Godot client.
