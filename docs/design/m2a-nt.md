# M2a protocol v2 (nt), as built

Track nt of M2a wave 2 (docs/design/m2a.md §7, §13 E17 wire side, §15), on branch `m2a/nt` from the W2.0 contract
(`m2a/w2` b8bef3f). It writes the bodies of the v2 declarations in `bf3/net/protocol.h` and `bf3/net/weapons_wire.h`
and sets `kProtocolVersion` to 2. The server's handling of the new messages is sv's and the client's is cl's. This
branch only defines the wire.

## What changed

| file | change |
|---|---|
| `src/bf3_net/include/bf3/net/protocol.h` | `kProtocolVersion = 2`; the bounds `kMaxKits` (4), `kMaxTeam` (2), `kMaxWireDefs` (255), `kMaxTargetRows` (128), `kMaxDebugReportBytes` (2 MiB), `kMaxShotTicksBack` (7); `OtherSoldier::operator==`; `set_other_weapon`, `quantize_remote_shot`, `remote_shot_of`; `encode_combat_events(..., r_clamped)`; as-built layout comments |
| `src/bf3_net/src/protocol.cpp` | the v2 codecs (below) |
| `src/bf3_net/include/bf3/net/weapons_wire.h` | `kWeaponsWireTimeZero`; `WeaponsWire::idle` (additive: the timed state of the other non-empty slots) |
| `src/bf3_net/src/weapons_wire.cpp` | new: `weapons_wire_view`, `weapons_from_wire`, `write_weapons_wire`, `read_weapons_wire` |
| `src/bf3_net/CMakeLists.txt` | `# M2a nt` block: `weapons_wire.cpp` |
| `tests/net_proto_tests.cpp` | `M2a nt` block at the end (v2 rows in `net.proto.unit`). The M1 rows that pin the v1 wire were updated (see "M1 rows") |
| `tests/net_proto_fuzz.cpp`, `tests/CMakeLists.txt` | new `net.proto.fuzz` (`# M2a nt` block) |
| `tests/m2a_contract_tests.cpp` | `kNtLanded = true`; `M2a nt` block pins `kMaxKits == host::kKitCount` and `kMaxTeam == host::kKitTeams` |

## Wire formats (protocol v2)

The message numbers are W2.0's: `SnapshotOthers` 3, `ShotEvents` 4, `CombatEvents` 23, `KitSelect` 24. Bit order,
varints and f32 bit patterns are M1's (`bit_stream.h`).

* **MatchInfo** gets, after `motor_hash`: u64 `match_seed`, u64 `weapon_hash`, u8 `weapon_flags`, u64 `kit_hash`
  and u8 `max_rewind_ticks`. A `weapon_flags` bit outside `kExactAim | kInfiniteMagazine` is refused. That adds 25 B.
* **Spawned** gets u8 kit (< 4) and u8 team (0..2) after the state. It is 85 B, which is M1's 83 B + 2.
* **KitSelect** carries u8 kit (< 4) and u8 team (0 keep, 1, 2). It is 3 B.
* **Debug** gets u8 slot (< 128) and u8 pose (valid) after `ticks`. The kinds run 1..7 (6 `SpawnTarget`,
  7 `TargetInfo`), and 0 or 8 and up are refused.
* **DebugReport** gets three fields after M1's trace:
  - varint `targets` (≤ 128). Each row is u8 slot, u8 pose, feet f32×3, u16 yaw, then the `HitTarget`: u32 id,
    u32 team, bound centre and radius, a spawn-protected bit, a 5-bit capsule count ≤ `kMaxZoneCapsules`, and the
    capsules at full precision.
  - varint gunplay ticks (≤ 1,200). Each tick has u32 tick and the counts 4/4/4/2/3 bits, bounded by
    `kMaxTrace*`. Then come the `ShotEvent`s, the projectiles (position and shot index), the `DamageEvent`s, the
    `DetonationEvent`s and the deaths, all at full precision.
  - u32 `trace_dropped`. The encoder adds what it cut beyond the per-tick bounds to this count.

  The writer cap is `kMaxDebugReportBytes`. M1's 256 KiB cap still holds for the other reliable messages.
* **CommandPacket** gets u32 `others_ack_bits` after `snapshot_ack_bits`. Every full command gets the view sample
  after its pose: 16 `view_back_q8`, 2 `view_quality` and 8 `view_seq8`, so the command fields are 98 bits. The
  "same as the newer" comparison covers the view sample too. Sizes: 33 B for one command or 4 equal ones, 70 B
  when all 4 differ (M1: 26 / 53, §7.2's "+17 B").
* **Snapshot**:
  - After the own state comes 1 bit for "own weapons present", then the weapons wire. The bit is only there when
    the own state is.
  - Each other gains u8 weapon, u3 phase, u1 firing, u1 ADS, u2 reload seq and u4 burst seq, for 178 bits.
  - The header with the own state stays 97 B (771 bits).
* **SnapshotOthers** carries u16 seq, u32 server tick and a u8 count (≤ 127), then the others as in the Snapshot.
  Duplicate slots are refused, and so is a datagram over 1,200 B.
* **Weapons wire** (`weapons_wire.cpp` header comment):
  - The header is u32 `next_shot_index`, u8 active, u8 pending, u16 events and a time for
    `sprint_recover_until`.
  - Each of the 8 slots starts with 1 bit "non-empty". An empty slot then carries u2 `mode_counter`, because the
    step writes the counter into every slot.
  - A non-empty slot carries: varint def, u2 mode, u2 counter, zigzag varints for magazine and reserve, u3 phase,
    u6 burst, u7 flags, then phase start / end, next shot, f32 spread, spread time, recoil (1 bit "both +0", else
    2×f32) and hold start.
  - A time is 1 bit "absolute 0", else an i32 offset from `tick_start(server tick)` with |offset| ≤ 35 s.
* **ShotEvents** carries u32 server tick and a u8 count (≤ 128), then per shot:
  - u7 slot, then the shot index. A shooter's first shot in the datagram sends the full u32. Later shots send a
    u4 delta of 1..15, or 0 as an escape followed by the u32.
  - u8 def (< 255), u3 ticks back (≤ 7) and u16 fraction of the tick.
  - 3×u20 origin (1/256 m from `ShotQuantization::origin_min`) and the direction as octahedral 2×u16.
  - u6 pellets − 1 and u1 tracer.
  - With the tracer bit set: aim as octahedral 2×u12. With pellets > 1: u13 cone in 1/64°.
  - u1 power present. When set, u12 power over [0, `kMaxPower`].

  A shot costs 138 bits, 166 for a shooter's first shot, plus 24 with a tracer. The encoder orders shooters
  nearest first (a shooter's distance is its nearest shot's), keeps each shooter's shots in caller order, and cuts
  where the next shot would not fit. Duplicates and shots that cannot travel are dropped and counted in
  `r_dropped`. The decoder refuses a datagram over 1,200 B, a shot named twice, def 255, and a time before tick 1.
* **CombatEvents** carries u32 server tick and three varint counts (sum ≤ 256), then the events:
  - Hit: u7 victim, u7 shooter, u32 shot, u6 pellet, u2 zone, u24 damage, u24 health after (1/100 hp, ≤ 100,000),
    3×u20 position, octahedral 2×u8 normal and a time.
  - Death: u7 victim, u7 killer, u8 def (255 = `kNoWeapon`), u2 zone, u32 shot, u1 headshot and a time.
  - Detonation: u7 shooter, u32 shot, u8 def (< 255), position, normal, u1 dud and a time.

  A time is a varint of ticks back plus u16 fraction. The message is reliable, so nothing is dropped silently.
  Values out of range are clamped, and events beyond 256 or with a slot ≥ 128 are cut. Both kinds are counted in
  `r_clamped`.

## Measured (dev preset, 2026-10-16, `net.proto.unit` MESSAGE lines)

| what | measured |
|---|---|
| command packet | 33 B for 1 or 4 equal commands, 70 B for 4 different ones, 58 B when the last two differ |
| Snapshot header + own state | 97 B; with a 4-slot kit (active slot firing) 172 B, so the kit's weapons wire is 75 B; a random 8-slot wire is up to 341 B |
| others per datagram (178 bits) | Snapshot with own state: 49; without own state: 53; SnapshotOthers: 53 |
| 64 players, with own state + kit | Snapshot 1,195 B with 46 others + SnapshotOthers 387 B with 17 others; 0 dropped; both ≤ 1,200 B |
| 128 players, with own state + kit | Snapshot 1,195 B / 46 + SnapshotOthers 1,188 B / 53; 28 dropped and counted (M3's priorities) |
| ShotEvents | 69 untraced single shots per datagram (1,200 B), 58 traced (1,184 B); 50 mixed shots 1,023 B |
| shot precision (max error) | time 8 units (1/125,000 tick), origin 0.00195 m, direction 0.0031°, aim 0.044°, cone 0.0071°, power 0.00105 |
| CombatEvents | empty 8 B; +26 B per hit, +11 B per death, +19 B per detonation; 20 / 6 / 4 events 648 B |
| combat precision | hp 0.0049, position 0.00195 m, normal 0.92°, time 8 units |
| DebugReport | 1 target row + 3 gunplay ticks: 916 B; every bound full (1,200 ticks × every per-tick maximum + 128 target rows): 1,712,384 B (cap 2,097,152) |
| net.proto.fuzz | 3,501 / 3,501 structured malformed cases refused; 900,000 mutated or random datagrams: 75,092 decoded and stable, 739,134 refused, 0 unstable (the rest had an unknown type byte), about 1.5 s on dev |
| asan preset | `net.bits.unit`, `net.proto.unit`, `net.netsim.unit`, `net.proto.fuzz` (16-36 s), `m2a.contract` and `m1.contract` pass, with no AddressSanitizer report |
| dev ctest | 90 / 90 non-timing rows pass (`core.cache_data` and `core.res` skipped as before) |
| timing rows | see "Timing rows on the shared machine" below: every row passes, but `godot.s3_crowd` only in a quiet window |

## Timing rows on the shared machine (acceptance-fix round, 2026-10-16 21:00-22:53, HEAD 7df0568)

Each row ran on its own, under this track's quiet lock. The machine is shared, so these timings are noisy (m2a.md §1
noise policy). nt changes only `bf3_net` and its tests. No code the s3 crowd bench runs changed (Skeleton3D / rs
skinning, the pose lanes, the renderer).

| row | runs | result |
|---|---|---|
| `host.server`, `host.session`, `host.scale`, `godot.m1`, `godot.m1_load` | 1 each | pass |
| `host.session.unit` | 4 | 4 / 4 pass (70 s each; loopback RTT mean 0.89-1.73 ms, 0 reconciliations, 0 hash mismatches). The earlier acceptance run's single failure in 3 did not repeat |
| `godot.s3_crowd`, 21:15-21:33 | 2 (each with its built-in retry) | **fail**. The failing gate changed from run to run: G1b 2.111 ms (≤ 2.0), then G1a / G1b / G2-off on an attempt where the backend bench picked skeleton3d over rs, then G2-on p99 20.28 ms, then G2-off p99 20.22 ms. This is the same picture as the earlier acceptance run's 0 / 3 |
| A/B `godot.s3_crowd`, 22:13-22:31: w2 (7220da0, nt's base) / nt / w2 | 3 | **3 / 3 pass**, nt on its first attempt |
| `godot.s3_crowd` nt, 22:40-22:49 | 2 | **2 / 2 pass**: G1a p99 0.401 / 0.338 ms, G1b 1.758 / 1.706 ms (≤ 2.0), G2-off p99 11.65 / 11.39 ms, G2-on p99 12.61 / 12.65 ms (≤ 16.7); w2 in the A/B: G1b 1.735 ms, G2-off p99 11.31 ms |

So nt and its base give the same s3 numbers in the same window (G1b 1.71-1.76 against 1.74 ms), and the failures
follow the load on the machine, not the branch. G1b has about 0.25 ms of headroom on a quiet machine. Any other
process using the CPU or GPU fails it.

Quiet lock: the earlier acceptance run's A/B script was killed while it held the shared lock, and its
`finally` never ran. The lock stayed orphaned from 17:51 to about 19:05 and blocked every agent. This round's runner
(scratchpad `nt\locked.ps1`, not in the repo) takes the lock without `-Force` and tags it with its PID. It
releases the lock in `finally` and on `PowerShell.Exiting`. A detached watchdog removes the lock (only while it
still holds that exact tag) once the runner's PID is gone, which was tested by hard-killing a fake owner. The
runner also stops itself after 40 minutes.

## Deviations from the m2a.md sketch (§7)

* **The pellets, cone and power fields are wider than the sketch.**
  - pellets: 6 bits (pellets − 1) instead of 5, so 1..`kMaxPellets` (64) fits.
  - pellet cone: 13 bits instead of 12, so the 1/64° steps reach `kMaxAngle` (90°).
  - power: a presence bit plus 12 bits over [0, `kMaxPower` = 10] instead of 8 bits at 1/255, because wp's power
    range is 0..10, not 0..1. Power 1 travels as the absent bit.
* **Shot time is 3 bits of ticks back plus 16 bits of the tick.** The sketch had only the 16 bits. A snapshot
  covers two ticks, and the 8-tick window lets a held-back shot travel. Combat event times use a varint of ticks
  back for the same reason, because their events may come from older ticks.
* **Shots that cannot travel are dropped and counted, not clamped.** That covers an origin outside the 4,096 m
  frame, def ≥ 255, slot ≥ 128, a time outside the window, 0 pellets, and a duplicate. A clamped origin or time
  would draw a tracer from the wrong place.
* **The CombatEvents sizes are larger than the sketch.** A hit is 26 B against ≈ 22, a death 11 B against 8, a
  detonation 19 B against 16. The tick-relative time and the full 32-bit shot index make up the difference.
  Health and damage are capped at 100,000 hp, so a decoded float re-encodes to the same code.
* **The weapons wire is wider than the W2.0 struct.** `WeaponsWire::idle` was added, and the wire carries every
  non-empty slot's timed state and every empty slot's mode counter. The step changes idle slots (recoil recovers,
  `kFirstShotArmed` re-arms, spread decays from `spread_time`) and writes the fire-mode counter into every slot, so
  the active and pending timers alone could not make `read(write(view)) == view` and equal steps (m2a.contract's
  600-tick row). The kit block is 75 B, against the sketch's ≈ 110 B.
* **Every time offset saturates at ±35 s.** This is documented in `weapons_wire.h`. An idle slot's `spread_time`
  after a long time holstered saturates, and the view then differs from the state. The spread has long decayed
  by then. The 600-tick contract row never reaches it.
* **The Trace's per-tick projectile bound is 8** (`kMaxTraceProjectiles`, W2.0's header), against the "≤ 64" in
  §7.4. **The DebugReport cap is 2 MiB**, against "≈ 0.5 MiB". A real 20 s trace is far smaller (one shooter's
  shots), but the cap must hold every bound at once (1.7 MB measured).
* **Spawned accepts team 0..2.** 0 means no team (a bot or a dev dummy). KitSelect's 0 means keep.
* **The others per datagram are 49 (own state, no weapons) and 46 with a 4-slot kit.** §7.3 says ≈ 49 with the
  own block. The 64-player split always fits the pair.

## M1 rows

The v2 wire changes sizes and layouts that M1 tests pinned. Those rows were updated in commit b982b95, which names
each one. No check was weakened, and each row still checks the exact new value:

| row | v1 value | v2 value |
|---|---|---|
| `peek_type` known types | 11 | 15 |
| Spawned size | 83 B | 85 B |
| command packet sizes | 26 / 53 / 25 B | 33 / 70 / 33 B |
| snapshot others with / without own state | 55 / 59 | 49 / 53 |
| the ENet loopback snapshot | 55 others | 49 others |
| refused Debug kind | 6 | 8 |

The hand-crafted hostile command, Spawned, Debug and MatchInfo bytes carry the v2 fields, and the `same_*`
helpers compare them.

## Gates (one command each, from the worktree in a vcvars shell with `OPENBF3_GODOT` set)

| gate | threshold | command |
|---|---|---|
| v2 round trips, sizes, split, acks, weapons wire | all `net-proto` cases pass; every v2 datagram ≤ 1,200 B; the 64-player split drops 0 | `ctest --preset dev -R "^net\.proto\.unit$" -V` |
| hostile input (E17 wire side) | every structured malformed case refused; 0 unstable; no crash; asan clean | `ctest --preset dev -R "^net\.proto\.fuzz$" -V` (and `--preset asan`) |
| m2a.contract nt rows | no `PENDING (nt)`; ack table against nt's predicates and codec; 600-tick weapons wire row | `ctest --preset dev -R "^m2a\.contract$" -V` |
| M1 net and host | pass | `ctest --preset dev -R "^(net\.|host\.server$|host\.session)"` (the host rows are timing rows: run them under the quiet lock) |

Timing rows (host.session*, host.server, godot.*) are wall-clock and noisy on this shared machine. Run them one
at a time under the quiet lock, e.g. `ctest --preset dev -R "^godot\.s3_crowd$" -V` (threshold: `S3 TIMING: PASS`,
G1b ≤ 2.0 ms, G2 p99 ≤ 16.7 ms).
