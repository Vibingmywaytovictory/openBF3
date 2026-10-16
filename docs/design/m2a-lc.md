# M2a lc: lag compensation and hit capsules, as built

Track "M2a lc" of M2a wave 2 (docs/design/m2a.md §15, row lc), branch `m2a/lc` from the W2.0 contract (b8bef3f plus
the orchestrator's 7220da0). Scope: all of m2a.md §6, §13.3's lag-compensation row, wp's R4 (the query segment in
`ballistics.cpp`) and the §16 decisions Q1, Q19, Q22 and Q23. The README's "M2a lc" section repeats the gates.

Tags as in m2a.md: **[verified]** read from code, data or a measurement in this tree on 2026-10-16; **[inferred]** a
choice or estimate made here.

## What landed

| file | what |
|---|---|
| `src/bf3_net/src/other_sample.cpp` (+ header additions) | `sample_other`, `select_sample` (the selection rule alone; `sample_other` calls it, so the pose cache key and the answer cannot disagree), `rewind_time_q8`, `sim_time_q8`, `view_lag_q8`, `rebuild_track`, `same_bits` |
| `src/bf3_host/src/lag_comp.cpp` (+ header additions) | `LagHistory` (shared record store, per-client sent rings and acks, shot-view rings, `track`, `sample`, `shot_view`'s refusal rules) and `RewindProvider` (present set, time rule, swept-bound broad phase, pose cache, verdicts, stats, trace) |
| `src/bf3_core/src/gameplay/capsule_pose_table.cpp` | table checks, hash, the `capsules/` cache blob (encode / decode; hostile blobs refused with a reason) |
| `src/bf3_anim/src/server_pose.cpp` | `build_capsule_pose_table` from the install's 3p rig and the `ServerPoseClipNames` clips |
| `src/bf3_anim/include/bf3/anim/server_pose_reference.h` | E18's reference evaluation, written independently of the builder (double precision, own bind accumulation) |
| `src/bf3_sim/src/weapons/posed_target.cpp` | `posed_target`, `capsule_table_data` (the table's material and zone inputs from wp's stand-in) |
| `src/bf3_sim/src/weapons/ballistics.cpp` | R4: `first_target` and `blast` fill `TargetQuery::from / to / radius / has_segment` |
| `tools/bf3dump/lagcomp_commands.*` | `bf3dump lagcomp <LEVEL> <MODE> [--deviation] [--perf]` |
| `tests/sim_lagcomp_tests.cpp`, `tests/anim_server_pose_tests.cpp` | `sim.lagcomp.unit` (no install), `anim.server_pose` (install; exit 77 without it) |

nt's v2 wire types are not merged. The E12 rows encode what a client receives (main Snapshot and SnapshotOthers
records, acks, loss, reorder) with test-local types in `namespace m2a_fake` (`Datagram`, `ReceivedRecords`,
`ClientTracks`), built only from the contract headers.

## Contract rules that proved wrong (fixed, each in its own commit)

* **Time rule (ed4c77d).** m2a.md §6.4 said target time = time_start in q8 ticks − view_back. With the record scale
  (the state after server tick S sits at S × 256) the step of command tick T covers [(T − 1) × 256, T × 256), so that
  rule tested one tick before the render time the player saw. As built:
  `rewind_time_q8 = sim_time_q8(t) − clamp(view_back_q8 − 256, 0, max_rewind_ticks × 256)`, `sim_time_q8` rounding
  half up to 1/256 tick. A shot at the start of command tick T tests at exactly R = (T << 8) − view_back; a later
  shot of the tick or a later segment of its bullet tests as much later. `max_rewind_ticks` 0 (E12's control run)
  tests at the present and a negative lag never tests in the future. `sim.lagcomp.unit` case "time rule" pins it and
  the E12 rows check `time_q8 == render_q8` for every rewound command.
* **History size (d96333f).** 24 snapshots (800 ms) did not cover the client's 32-seq received window plus the view
  lag: after a respawn, E12's server track lists differed from the client's because records the client still held had
  left the store. `kHistorySnapshots` = 48 (1.6 s, ≈ 144 KiB shared records); m2a.md §6.3, §6.6 and §6.7 updated.
* **Transition fractions (cfb519a).** At 8 fractions the E18 transition rows missed the 5 cm gate on the prone
  transitions (at the mid-fractions: stand→prone head 8.06 cm, prone→crouch 8.61 cm, prone→stand 6.33 cm,
  crouch→prone 5.80 cm); the clips move the head faster than a 1/7 step lerps. `kCapsuleTransitionFractions` = 16:
  the table grows from 0.47 to 0.90 MiB and ≈ 1,700 to ≈ 3,300 pose evaluations (build ≈ 0.02 s [verified]); E18 now
  uses the 15 mid-fractions. m2a.md E18 and §6.5 updated. `kCapsuleTableVersion` 2, so cached v1 tables rebuild.

## §16 decisions taken

* **Q1** 350 ms = 21 ticks (`kDefaultMaxRewindTicks`), as the contract.
* **Q19** one pose per segment, at the segment start, to 1/256 tick.
* **Q22** a SnapshotOthers record newer than the client's newest Snapshot is left out of the rewind (the client's
  `view_seq8` names the Snapshot; the window is "at or before the view tick").
* **Q23** the broad phase is a swept AABB per target: the bound of its recorded feet over the shared store and its
  present feet, grown by the table's capsule bound, the extrapolation limit at the largest recorded speed and the
  Hermite overshoot between two samples (≤ 8/27 of the span times the speed), tested against the segment's box grown
  by the query radius. It never removes a hit (`sim.lagcomp.unit` case "RewindProvider": the same hits with and
  without the cull on aimed segments).

## Ack and view conventions

* `snapshot_ack_bits` bit i acks seq newest − 1 − i; `others_ack_bits` bit i acks newest − i (m2a.md §7.2, the
  `m2a.contract` table). A bit for a seq or part never sent to that client is ignored and counted (`acks_ignored`).
* The shot view (16 B, per shot index) snapshots the shooter's received set when the command arrives. Refused, tested
  at the present and counted: a `view_seq8` that resolves to no sent seq, a negative lag, quality Held, or
  Extrapolated beyond 250 ms (m2a.md §13.4).
* `rebuild_track`: sorted by tick, duplicates once, the 8 newest, only the newest record's generation, nothing older
  than 60 ticks before the view tick. `select_sample` kinds: Interpolate (strictly between two samples), Extrapolate
  (past the newest, ≤ `extrapolate_max_ms`), Hold (one sample: before the oldest, exactly at a sample, or past the
  limit).
* Pose cache key: (slot, generation, time_q8, tick_a, tick_b, kind); 2,048 entries, open addressing, cleared per tick.

## Window rebuild vs the client's incremental track

The server rebuilds a target's track from the received window (`rebuild_track` over the records the client acked in
its last 32 seqs); the client keeps an incremental track (`add_others`). They are not the same function, and
`sim.lagcomp.unit` case "window vs track" classifies every difference. On the E12 run (2 clients, 6 players, 900
ticks, 12 % / 15 % loss, reorder, a respawn) the classes are cl's counters: a reordered older record of another
generation that reset the client's track (`generation_reorder`), a SnapshotOthers record newer than the newest
Snapshot (Q22, `others_ahead`), a track sample older than the 32-seq window, and the timeout measured from the
arrival (client) vs the record tick (window, `track_timeout`). Measured: 8,905 rows, 7,467 equal (84 %), 1,438 Q22
(16 %), 0 in every other class, 0 unexplained; the test checks that every row is equal or classified and that equal
rows exceed 3/4. **The server's answer is the window rebuild**, which is what E12's bit-equality rows compare against the
client's own rebuild from the same received records (that rebuild is what the client draws in M2a; m2a.md §6.3).

## Hit capsules (§6.5)

* **Placement [inferred].** The `HitCapsule` offset is read as a model-space vector of the bind pose (the rig's rest
  model pose; the `lagcomp.ebx` rows of `bf3dump lagcomp` check that it equals the EBX `veniceantske01` ModelPose):
  `a = bone.t + (bone.q · conj(bind.q)) · offset`, `b = a + bone.q · (±BoneAxis X · length)` with the sign towards the
  main child; length ≤ 0.01 is a sphere. Read as a bone-local point instead, the chest sphere sits at 0.94 m and 15 cm
  in front of the spine; the bind-model reading gives chest 1.234 m, head 1.536 m, belly ≈ 0.95 m on the stand idle.
  `bf3dump lagcomp MP_Subway TeamDeathMatch0` prints the three readings side by side (`lagcomp.offset` rows); the
  numbers for review are under `b/shots/lc/` (not in the repo). The user should judge this reading.
* **Aim limits.** Stand 78.1° up / 60.7° down (S3). Crouch 47.46 / 51.83°, prone 36.26 / 17.66° (measured from the
  clips' LocalPost rule [verified: install]).
* **Rows.** 3 stance rows × 33 pitch buckets and 6 transitions × 16 fractions × 33 buckets, 9 capsules each; every
  row tagged clip-derived or stand-in; the retail install gives 0 stand-in rows. The table hash goes into
  `weapon_hash`.

## Determinism (§13.3)

Both ends call one sampler (`sample_other` / `select_sample`) on bit-identical quantised samples; the server's
`LagConfig` carries the client's other inputs (`pose_eye_heights`, `extrapolate_max_ms`). E12's rows compare by bit
pattern (`same_bits`). The pose cache does not change an answer (cache on == off, every HitTarget field).

## Gates (measured 2026-10-16, dev preset)

Each gate has one command. Wall-clock rows (E13) are **noisy** on this shared machine: they run under the quiet lock,
and a failure is repeated once and reported as WARN with the baseline.

| gate | command | threshold | measured | met |
|---|---|---|---|---|
| E18 (a) | `ctest -R anim.server_pose` (install); also `bf3dump lagcomp MP_Subway TeamDeathMatch0 --deviation` | head and chest centres ≤ 3 cm of the reference at the 32 mid-pitches (steady), ≤ 5 cm at 15 mid-fractions × 32 mid-pitches (transitions); stand ≤ 3 cm of `CrowdEvaluator`; legs and run reported; 0 stand-in rows | steady head / chest max: stand 0.013 / 0.002 cm, crouch 0.077 / 0.023, prone 0.179 / 0.044; transitions (480 queries each) stand→crouch 1.01 / 0.63, stand→prone 2.45 / 3.09, crouch→stand 0.79 / 0.46, crouch→prone 1.69 / 0.99, prone→stand 1.56 / 0.86, prone→crouch 2.47 / 1.20; CrowdEvaluator stand 0.013 / 0.002; on-grid entries 0.0000; legs worst 4.61 (reported); run vs stand head 13.5 (reported); 10/10 gated rows, 0 stand-in rows | yes |
| E12 (a) | `ctest -R sim.lagcomp.unit` | server track lists and samples equal to the client's bit for bit; provider answers equal the client's samples; cache on == off; every window/track difference classified | rebuild: 1,780 commands rewound (1 clamped, not compared), 8,900 (command, target) rows (6,156 interpolated, 2,678 extrapolated, 62 held; 1,323 after the respawn), 0 list and 0 pose differences over 1,800 datagrams, 250 lost, 117 reordered. Provider: 7,124 queries, 17,636 trace rows equal to the client's samples, 0 cache on/off differences (35,380 candidates, 17,646 cache hits). Window vs track: 8,905 rows, 7,467 equal, 1,438 Q22, 0 other classes, 0 unexplained | yes |
| E13 | `bf3dump lagcomp MP_Subway TeamDeathMatch0 --perf` (rows pairs, long; under the quiet lock) | provider p99 ≤ 0.5 ms per tick per row | pairs p99 0.338 ms (mean 0.193, max 0.587), per tick 1,148 queries, 26.1 candidates, 0 cache hits, 72,312 culled; long p99 0.354 ms (mean 0.197, max 0.529), 1,289 queries, 24.7 candidates, 2.0 cache hits; 17,920 shots each, all rewound, 0 clamped / refused / history misses. One run under the lock, no repeat needed | yes |
| R4 | `bf3dump weapon-sim --check`; `ctest -R "^sim\.weapons" -E perf` | 38/38; sim.weapons* bit-equal to the build without R4 | 38/38; the `weapon-sim --check` and `--all` outputs are byte-equal (bar the "opened in" timing lines) between this branch and the same tree with `ballistics.cpp` reverted to 7220da0; sim.weapons, .unit, .range, .fuzz pass in both. Only `RewindProvider` reads the new fields | yes |
| install unchanged | every install row | `install.unchanged yes` | yes (256 files) in `anim.server_pose`, `bf3dump lagcomp` and `weapon-sim` | yes |

Other rows on this branch (2026-10-16):

* Full ctest without the timing rows (`-j 6 -E` godot.*, host.session*, host.server, host.scale*, sim.nav.bake,
  sim.weapons.perf, tools.fx_bench, logic.perf): 93 rows, 89 pass, `core.cache_data` / `core.res` skipped as before.
  `core.ui.swf.unit` (a hostile case over its 1 s limit) and `core.ui.font` (cold 1.41 s, limit 1 s) failed under the
  parallel load, as on W2.0, and pass alone.
* Timing rows one at a time under the quiet lock: 19 of 20 pass. `host.session.unit` failed its E2 clock-target row
  twice (7 and 4 target changes, limit 2) and passed on the third run (2); the base build (`wt/w2`, 7220da0) gave a
  pass with 0 changes in the same window. No lc code is on the session path (nothing outside lc's files calls
  `sample_other` or `LagHistory`), so this is reported as WARN (wall-clock noise, the W2.0 note saw 4 and 5), not
  fixed here.
* asan preset: `sim.lagcomp.unit` (hostile blobs, acks of unsent seqs, refused views), `anim.server_pose`, `m2a.contract`,
  `sim.weapons.unit`, `sim.weapons.fuzz` pass with no AddressSanitizer report.

## Open limits

* **Long flights.** Every segment of a bullet uses the shot's stored view, i.e. the received set at fire time (32
  seqs back from the view seq). The store keeps 48 snapshots (96 ticks); a late segment of a long flight asks for
  records that have left it. Those are counted as `history_misses` and skipped. The rebuilt list only changes when a
  missing record would have been among the 8 newest the client held (a loss streak just before the shot), so the
  sample stays the client's in practice; E12 runs single-tick queries and E13 counted 0 misses, so long-flight
  equality is not measured.
* **Client slot == player slot.** The provider takes the shooter's player slot as its client slot. A server with
  spectators or slots that differ needs a map (sv / nt).
* **Seq 0.** An ack of seq 0 before anything was sent is ignored and counted like any other unsent seq; the first
  Snapshot a client receives is never seq 0 in the E12 runs (seqs start at 65,500 and 100, and wrap).
* **Locomotion** is not posed: legs stay in the idle stance (E18 reports run vs stand: head 13.5 cm).
* nt's codecs replace `m2a_fake` once they land; the E12 rows then run on the real messages.
