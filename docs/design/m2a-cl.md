# M2a cl: client prediction and events, as built

Track "M2a cl" of M2a wave 2 (docs/design/m2a.md §15, row cl), branch `m2a/cl` from `m2a/w2` f4bbc43 (the W2.0
contract with nt and lc merged). Scope: m2a.md §6.1–§6.3 and §7.2–§7.4 on the client side, §8, §13.3's client rows,
the named replacements R1 and R2, and the named insertions in `prediction.cpp` (`catch_up`) and `client_session.cpp`.
The README's "M2a cl" section repeats the gates.

Tags as in m2a.md: **[verified]** read from code or a measurement in this tree on 2026-10-17; **[inferred]** a choice
or estimate made here.

sv (the server gunplay) is merged (6e32749, `--no-ff`). Every end-to-end case now runs against the real server:
`host::ServerHost` running the GameServer with sv's `Gunplay` (described under "The end-to-end harness" below).
`m2a_fake::GunServer` is deleted.

## What landed

| file | what |
|---|---|
| `src/bf3_host/include/bf3/host/presentation_events.h`, `src/bf3_host/src/presentation_events.cpp` | `PresentEvent` (64 B), `PresentationEvents` (SPSC ring, 2,048 entries, overflow counter), the timelines (Predicted / Render / Immediate), `effect_seed` and `impact_seed` (§4.4) |
| `src/bf3_host/include/bf3/host/presentation_weapon.h`, `src/bf3_host/src/presentation_weapon.cpp` | `PresentedWeapon` and `present_weapon(prev, curr, tick, fraction, table)`: phase and progress, ammo, spread and recoil lerped between the two predicted ticks, zoom; no lerp across a change of item; a hostile view (def outside the table, active slot out of range) gives `valid = false` |
| `src/bf3_host/include/bf3/host/client_projectiles.h`, `src/bf3_host/src/client_projectiles.cpp` | `ClientProjectiles` (§8.4): the own world on the predicted timeline against the rewound records, the remote world on the render timeline, Impact and SoldierContact events, the E12 log (`OwnTestSample`), `shot_event_of(RemoteShot)` |
| `src/bf3_sim/include/bf3/sim/prediction.h`, `src/bf3_sim/src/prediction.cpp` | `WeaponLane` (the weapons ring beside `PredictionRing`), `WeaponReplay`, `WeaponEmitSink`, `PredictionRing::attach_weapons`; the named insertion in `catch_up` (the weapons step after the motor step) |
| `src/bf3_host/include/bf3/host/client_session.h`, `src/bf3_host/src/client_session.cpp` | additive blocks (config, stats, frame fields, `ClientGunplayInputs`, `build_client_weapon_table`, `client_weapon_hash`, `present_others` with `PresentOthersCounts`, `set_view_sample`, `present_view_quality`), the `Impl` member block and the end block (`cl_*`), the named insertions, R1 and R2 |
| `tests/host_gunplay_client_tests.cpp` | `host.gunplay.client.unit` and `host.gunplay.client` against the real ServerHost (Metro for the gates, gunplay_floor for the install-free variants; ServerLog, Server, Bot, HostileRelay, the E12 site finder) |
| `src/bf3_host/include/bf3/host/gunplay.h`, `src/bf3_host/src/gunplay.cpp` (sv's; contract fix 7ea1331) | `GunplayObserver::wants_rewind_rows` / `rewind_rows`: the rewound queries' trace rows for E12 (b), dev only |
| `tests/host_session_tests.cpp` (block at the end) | R1's equivalence against M1's `present_others` body |

## The client, step by step

* **MatchInfo (E6).** `ClientGunplayInputs` (the weapon table, the capsule pose table, the kit table, the shot
  quantisation frame, a `load_mechanics(path)` hook) come from the level by default: `client_gunplay_inputs(level)`
  builds them from sv's `SharedLevel::gunplay` (`make_match_table` with wp's defaults, `load_mechanics_cfg`, the
  level's capsule pose table, kits and `shot_frame`) on MatchInfo, unless the config supplies its own
  (`ClientSessionConfig::gunplay`) or turns it off (`gunplay_from_level = false`). `mechanics_cfg` is parsed by the
  client itself through the hook. A table's defs do not depend on the mechanics, so the client's copy with the
  match's mechanics and flags equals the server's `make_match_table` bit for bit. On MatchInfo with a `weapon_hash`, the client builds its table
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
  predicted weapons at T rebase without a compare (`weapon_ring_misses`). A difference only in what a hit writes
  (wp's `apply_damage`: the health, the tick's `damage`, `kDamaged`) is R2's health rebase: the ring's health from T
  on moves by the difference, the hit tick takes the server's `damage` and `kDamaged`, no replay (`health_rebases`). A motor mismatch
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
* **R2's "only health" (24fea82, code 026c15d).** Against sv's real server every hit reconciled the motor: wp's
  `apply_damage` also adds the tick's `damage` and sets `kDamaged`. `states_match` now leaves out all three (the
  motor's own fall damage follows from `kLanded`, `impact_speed` and `protect_ticks`, which stay compared); m2a.md
  §8.2 says so.
* **E12 (b) had no server source (7ea1331).** E12 (b) compares the server's rewound `OtherPose` with the client's
  sample, but the Trace wire (`TraceGunplayTick`) does not carry it and sv's observer had no callback for it. A
  dev-only `GunplayObserver::rewind_rows` (with `wants_rewind_rows`, read once in `Gunplay::create`, which then calls
  lc's `RewindProvider::set_trace`) hands the rows of each projectile step to the observer. Off otherwise. This is
  an edit in sv's files (gunplay.h's observer and three guarded lines in gunplay.cpp), each line marked `M2a cl`.
* **Clients without inputs on a gunplay server (f5ba4dc).** sv's `host.gunplay.server` starts its ClientSessions
  without `ClientGunplayInputs`; after the merge cl's client refused them ("weapon table mismatch"). The client now
  builds the inputs from `SharedLevel::gunplay` itself (above); sv's cases pass unchanged.

## The end-to-end harness (after sv's merge)

`tests/host_gunplay_client_tests.cpp`:

* **The levels.** The gates (E1 + E5, E12, E15 loopback and two clients in `host.gunplay.client`) play on **Metro**
  as m2a.md §1 defines for the headless gunplay binaries: `tg::metro_gunplay()` (MP_Subway / TeamDeathMatch0 with
  `SharedLoadOptions::gunplay`: the install's weapons and kits, the capsule pose table from the retail clips, Metro's
  static world with its collision), read-only on the install, scratch cache root; without an install those cases are
  skipped (exit 77), and the suite ends with `install.unchanged`. The same runs, shorter, are install-free variants
  in `host.gunplay.client.unit` on `gunplay_floor()`: the fixture's synthetic floor (240 m, twelve spawn points, the
  synthetic soldier) with a `GunplayLevel`: the synthetic gate weapons (`weapons_support.h`) with an empty material
  table (every zone multiplier 1: the M16A3 does 25 per hit at R2's 5 m), lc's synthetic capsule pose table, and a
  kit table giving every kit of both teams the M16A3 and the M9 (`kit_table_hash`). Server and clients share the
  level object. The unit cases (E6, R2, SnapshotOthers, hostile and the rest) stay on the floor.
* **E12's site** (`find_lagcomp_site`): the first spawn point and heading (level spawn order, 64 headings,
  deterministic) where the shooter's eye has a clear projectile ray 23 m ahead, the floor 20 m ahead is level within
  0.1 m over +-5 m across the line, a standing capsule sweeps the whole +-5 m free, and rays from the eye to 0.3, 1.0
  and 1.6 m above every 0.5 m of it are clear (falls back to 16 and 12 m; not needed). On Metro it is spawn 10:
  shooter feet (-261.38, 60.03, -545.16) facing yaw 30720, strafe centre (-257.48, 60.05, -564.78) 20 m ahead. The
  target bot teleports itself there (`Bot::teleport`, sent from its own thread) and strafes +-4 m across the line
  (`StrafeSpec`; a leg also turns back after 3 s, for a wall in the way); the shooter teleports to the spawn point.
* **The server.** `Server`: `host::ServerHost` with the GameServer, `dev_commands`, 64 slots, `GunplayConfig` (the
  shipped `weapon_mechanics.cfg` or a scratch variant, match seed, `exact_aim`, `infinite_magazine`,
  `lag.max_rewind_ticks`, sv's stationary target bots, the observer). `ServerLog` (a `GunplayObserver`) keeps the
  shots and timed events with their command tick, the active magazine at each change, the Target impacts, the
  damage decisions and, for E12, the rewind trace rows. `game_stats()` gives the gunplay counters.
* **The other soldiers** are real clients: `Bot` runs a `ClientSession` on its own thread with a scripted input (a
  target strafing ± 4 m at the stand speed across its spawn heading; a shooter firing bursts at the floor; R2's
  shooter aiming at the victim's presented chest), on a team by KitSelect (dev). Same-team soldiers do no damage
  (friendly fire off), so the loopback and the two-clients runs have no deaths.
* **E12's target** must not die over 500 hits: both ends parse a scratch copy of the cfg with
  `bullet_damage_modifier = 0` (the impacts and the rewound tests are unchanged, the hash differs from the shipped
  cfg on both ends alike).
* **The hostile case** puts `HostileRelay` (two ENet hosts on one thread) between the client and the server: it
  forwards both ways and adds two mutated copies (bit flips, truncation, trailing bytes) of every SnapshotOthers,
  ShotEvents and CombatEvents, and every 20th Snapshot names def 0xFFF0 in the own weapons block, followed by a
  ShotEvents with an unknown def and a CombatEvents hit of 1e30 naming slot 250. 60 target bots make the server
  split its snapshots, so SnapshotOthers travel.

## Gates (measured 2026-10-17, dev preset, against the real ServerHost after sv's merge)

The wall-clock rows run under the quiet lock and are **noisy** on this shared machine. `host.gunplay.client` skips
(77) unless `OPENBF3_CL_TIMING=1` (`tools\build.cmd`'s TIMING regex, not cl's file, does not list it) and without
an install. Every row's server is `host::ServerHost` with sv's `Gunplay` (`m2a_fake` is gone). The gated end-to-end
rows are on **Metro**: one run of the five-case timing suite under this track's quiet lock, 16.5 min, 5 / 5 cases,
96 / 96 assertions (log `scratchpad\cl\metro_timing1.txt`).

| gate | command | threshold | measured (Metro) | met |
|---|---|---|---|---|
| E1 (client side) | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E1 + E5*"` (quiet lock) | predicted `ShotEvent`s equal the server's field for field 31/31; 30 gaps of 4,500,000; first shot at the press tick's start; one OwnShot event per predicted shot | the install's `weapons/m16a4/u_m16a4` (team 1 kit 0): 31/31 equal against sv's `Gunplay` observer (time, spawn time, seed, ids, origin, aim, direction, spread, cone, recoil, tracer, power by bits; and the command tick); 30/30 gaps; first at the press tick's start; 36 predicted, 36 server shots, 36 OwnShot events; 0 motor and 0 weapon reconciliations | yes |
| E5 (client side) | same case | server phase events = predicted phase events; M16A3 tactical 108,000,000 with commit at +91,800,000 -> 31, empty 142,200,000 -> 30 | 7 / 7 timed events equal (time, event, weapon slot, tick); empty 142,200,000 -> 30 rounds, tactical 108,000,000, commit +91,800,000 -> 31 rounds (the server's active magazine at the last change at or before each finish) | yes |
| E6 (client side) | `ctest --preset dev -R host.gunplay.client.unit` (case "E6") | a mismatched client disconnects on MatchInfo with "weapon table mismatch", `weapon_hash_mismatch` = 1, never sends Ready, never predicts | against a real gunplay server (floor): variant.cfg (other disc distribution): refused "weapon table mismatch", `weapon_hash_mismatch` 1, Ready 0, commands 0, never spawned on the server; missing.cfg and a client without inputs (`gunplay_from_level = false`): the same with the reason appended; a different kit hash: "kit table mismatch", `kit_hash_mismatch` 1; the matched client (inputs from the level): spawned 1, gunplay on; an M1 server (no `weapon_hash`): gunplay off | yes |
| E12 (b) lagcomp | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E12 lagcomp*"` (quiet lock) | 150/2, a target strafing at 4 m/s over 8 m at 20 m, 500 single shots: the server hits >= 99 % of the shots the client's own test hit, same zone for >= 99 % of those; every sampled target's `OtherPose` equal to the server's rewound pose bit for bit except `late_sample`; every shot the server hit has a compared sample (unless clamped); residuals reported | Metro's site above and clip capsule table: 500 shots (sv's `exact_aim`, `infinite_magazine`, a damage-free cfg on both ends), client test hit 500, server 500 (100 %), same zone 500 (100 %); (b) 500 / 500 samples bit-equal to the server's `rewind_rows`, 0 mismatched, `late_sample` 0; all 500 server rows matched; all 500 server-hit shots have a compared sample; 504 client samples without a server row (segments the server's swept-bound broad phase culled before posing); server verdicts 497 rewound, 3 clamped, 0 refused; residuals `generation_reorder` 0, `others_ahead` 0, `track_timeout` 0, `window_extrapolated` 5 of 25,490 draws | yes |
| E12 control | same case | the server without rewind (`max_rewind_ticks` 0) hits <= 50 % of the client's test hits | 12 of 500 (2.40 %), all 500 clamped to the present | yes |
| E12 stress | same case | reported | "lat=150-165,loss=2": client test hit 29, server 29 (100 %), same zone 100 %; (b) 971 / 971 equal; every shot clamped on both ends (view lag > 350 ms) | reported |
| E15 loopback | `set OPENBF3_CL_TIMING=1` then `b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client --test-case="*E15 loopback*"` (quiet lock; `OPENBF3_CL_LOOPBACK_S`, default 600) | 0 weapon and 0 motor reconciliations in 10 min of scripted play without netsim; health rebases reported | 600 s on Metro (walk, turn, bursts and taps, zoom, reload, pistol and back, crouch, jump, sprint; a strafing and a firing bot, real clients; the player's feet ranged over x -118.3 .. -86.0, z -570.8 .. -539.0, y 59.9 .. 60.8, against Metro's collision): 0 motor, 0 weapon reconciliations, 0 weapon ring misses, 0 step precondition failures, 0 lane gaps, 0 event overflows; 236 predicted shots, 234 timed events, 185 remote shots seen, 422 server shots (405 rewound, 17 clamped), 36,018 lane steps, 0 replayed; health rebases 0 (the bots are on the player's team, friendly fire off; R2 covers the rebase against a real enemy) | yes |
| E15 two clients | `... --test-case="*two clients*"` (quiet lock) | the second client joins after 30 s (every bot slot has fired); both derive the same effect seed for every remote shot from the join on | B saw 212 remote shots after its join, A saw all 212, 212 / 212 equal effect seeds, origins within 1 cm; both bots fired before the join; 0 motor reconciliations on A and B | yes |
| install unchanged | same suite, last case | layout.toc, cas.cat and the .sb / .toc files unchanged | `install.unchanged: clean (256 files)` | yes |
| install-free variants | `ctest --preset dev -R host.gunplay.client.unit` (cases "synthetic floor variant") | the gates' checks on shorter runs on the floor (E12: 60 shots, >= 12 client hits; E15: 24 s loopback, two clients 5 s + 10 s) | E1 31/31, 30/30 gaps, 36 = 36 shots; E5 7/7, 142,200,000 -> 30, 108,000,000 (+91,800,000) -> 31; E12 60/60 (100 %), zone 100 %, (b) 60/60 equal; loopback 0 motor / 0 weapon reconciliations, 38 predicted and 61 remote shots; two clients 68/68 seeds (68 s for the three cases) | yes |
| R1 | `ctest --preset dev -R host.session.unit` (case "M2a cl R1") | `present_others` through `net::sample_other` draws what M1's body drew | 150,196 drawn soldiers on 50,000 random frames, 150,196 bit-equal in every M1 field (11,237 bracketed, 16,292 before the oldest, 122,667 extrapolated of which 54,964 held past 250 ms) | yes |
| R2 | `ctest --preset dev -R host.gunplay.client.unit` (case "R2") | a server hit that changes only what a hit writes is a health rebase, not a reconciliation | an enemy client (team 2, `exact_aim`) taps 3 shots at the victim from 5 m: 3 server hits, 3 health rebases, health 25.0, 0 motor and 0 weapon reconciliations after the teleport, 3 Hit events Immediate with `kOwnVictim` | yes |
| SnapshotOthers | `ctest --preset dev -R host.gunplay.client.unit` (case "SnapshotOthers") | the server's split snapshots arrive and draw every other | 60 sv target bots and a firing bot: 91 SnapshotOthers received, 61 others drawn, 0 acks ignored by `LagHistory`, 0 others dropped on the server | yes |
| events and presented weapon (vm, hud, fx3, au3) | `ctest --preset dev -R host.gunplay.client.unit` | the ring keeps order and counts a full ring; `present_weapon` lerps and refuses hostile views; the frame carries the two predicted wire views | 1,000,000 events in order across two threads; 5 overflows counted on a full ring; present_weapon rows pass; events seen end to end: OwnShot, OwnTimed, RemoteShot, Impact, SoldierContact, Hit, Spawn | yes |
| hostile gunplay data | `ctest --preset dev -R host.gunplay.client.unit` (case "hostile") and the asan preset | mutated and out-of-range gunplay datagrams refused and counted; the session keeps predicting | through `HostileRelay` against the real server: 427 hostile datagrams sent (356 forwarded), 301 refused by the decoders (`bad_gunplay_datagrams`), 8 out-of-range events dropped (`bad_gunplay_events`), 7 own weapons blocks with a def outside the table refused (`bad_weapon_wires`), still connected and predicting, 0 weapon reconciliations, 0 step precondition failures | yes |
| sv's server rows | `ctest --preset dev -R "^host\.gunplay\.server"` (quiet lock) | sv's rows still pass after cl's contract fixes | host.gunplay.server passed (165.8 s, Metro) | yes |
| M1 host.session | `ctest --preset dev -R "^host\.session"` (quiet lock) | the M1 rows still pass | host.session.unit passed (71.3 s), host.session passed (197.0 s) | yes |

Other rows on this branch:

* Full ctest without the timing rows (`toolsuild.cmd dev`, which excludes godot.*, host.session*, host.server,
  host.gunplay.perf, sim.nav.bake, sim.weapons.perf, tools.fx_bench, the audio and logic timing rows), after the move
  of the gates to Metro: 95 rows, 92 pass, `core.cache_data` / `core.res` skipped as before, `host.gunplay.client`
  skipped (77) without `OPENBF3_CL_TIMING` (526 s; log `scratchpad\clull_ctest5.txt`). `host.gunplay.client.unit`
  now takes 91 s (ENet loopback, wall clock but not timed; 68 s of it the three floor variants); `host.gunplay.server`
  127 s. An earlier run failed `sim.vehicle.unit`, `sim.vehicle` (timeouts) and `core.ui.swf.unit` (a 1 s budget)
  because cl's `b\dev` cache had lost `CMAKE_CXX_FLAGS_RELEASE` (an unoptimised build, from a configure without a
  working vcvars); a fresh cache (/O2) passes them all, and every timing case above ran on that build.
* Timing rows under this track's quiet lock, one run each: `host.gunplay.client` on Metro (the five cases, 16.5 min,
  all pass; before the move to Metro the four cases passed on the synthetic floor in 986 s), `host.gunplay.server`,
  `host.session.unit` and `host.session` pass.
* asan preset (rebuilt after the merge): `host.gunplay.client.unit` (27.3 s; the real server, the hostile relay, E6, R2,
  SnapshotOthers), `host.session.unit` (72.6 s, with R1's case) and `m2a.contract` pass with no AddressSanitizer report.
* `host.gunplay.client` reads the install read-only (Metro) and ends with `install.unchanged` clean, as
  `host.gunplay.server`'s Metro rows and `host.session`'s do; `host.gunplay.client.unit` reads no install.

## Open limits

* **Stress clamps.** At "lat=150-165,loss=2" the view lag (RTT ≈ 323 ms plus the interpolation delay) is past 350 ms,
  so every shot is clamped on both ends (the client's own test uses the same clamp). The hit agreement is therefore
  expected; the row is reported, not gated.
* **E12 control** uses the client's own test with the default 21-tick rewind (`own_test_max_rewind_ticks`) against a
  server with `max_rewind_ticks` 0, which is what "without rewind" means for the hit rate.
* **E12 (b) covers what both ends posed.** The server's swept-bound broad phase culls segments before posing, so about
  500 client samples have no server row; the bit compare covers the 500 that do, and the row also checks that every
  shot the server hit has a compared sample and reports the server rows no client sample matched (0 on Metro).
* **The E12 site and the loopback are Metro's open ground.** The site finder picks a level stretch in clear view
  (spawn 10, y about 60); the loopback player stays within about 32 m x 32 m of its spawn with under 1 m of height
  change (it walks into whatever is there, but the script does not seek out stairs or props). Both run against
  Metro's collision world and clip capsule table; stairs and multi-level play are not separately gated.
* **Pose eye heights.** The server's `LagCompConfig::pose_eye_heights` stay at the defaults {1.6, 1.0, 0.3}, which
  equal the synthetic soldier's and Metro's; a level with other heights would need sv to set them from the soldier.
* **No health rebase in the loopback.** The loopback's bots are on the player's team (friendly fire off), so E15
  sees no hits; R2 measures the rebase against an enemy client.
* **Kits.** `kit_table_hash` and `KitTable::loadout` are sv's; the client checks `KitTable::hash` when the server
  sends one and indexes `loadouts[team - 1][kit]`. On Metro the kits are the install's (team 1 kit 0: the M16A3);
  the floor's kit table gives every kit the M16A3 and the M9.
* **E1 / E5 from a predicted client only.** The server rows of E1 and E5 (`host.gunplay.server`) and the
  viewmodel / HUD rows (`godot.m2a`) are sv's, vm's and hud's.
* **ClientGunplayInputs on the Godot client** come from `SharedLevel::gunplay`, which sv's loader fills only when
  `SharedLoadOptions::gunplay` is set; a level loaded without it leaves the client without gunplay (refused by a
  gunplay server, as E6's no-inputs row).
* **The timing regex.** `tools\build.cmd` does not list `host.gunplay.client` in its TIMING regex, so the full ctest
  skips it (77) unless `OPENBF3_CL_TIMING=1`; the fix is a one-line edit in a shared file, left to the owner.
