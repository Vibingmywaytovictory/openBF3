# M2a wave 2 contract (W2.0), as built

The integrator's contract commit for M2a wave 2 (docs/design/m2a.md §15, §15.1 step 1), on branch `m2a/w2` from
`main` 1874933. It declares every interface the wave-2 tracks (sv, lc, nt, cl, vm, fx2, hud) compile against, and
writes almost no bodies. Everything below is additive. No existing line of an M1 or wp file changed: fields were
added at the end of structs or after a named field, and each library's `CMakeLists.txt` gained one `# M2a w2` block
at its end.

## What the commit contains

| track (owner of the bodies) | headers declared in W2.0 |
|---|---|
| sv | `bf3/host/gunplay.h`, `kits.h`, `target_bots.h`, `server_view.h`; `bf3/sim/weapons/mechanics_tuning.h`, `weapon_rules.h` (+ the forwarding stub `src/bf3_sim/src/weapons/weapon_rules.cpp`); `GameServer::place_soldier` (declared only); `WeaponTable` / `WeaponTableOptions::exact_aim` (fields only, neither hashed nor applied: that is R3) |
| lc | `bf3/core/gameplay/capsule_pose_table.h`, `bf3/anim/server_pose.h`, `bf3/sim/weapons/posed_target.h`, `bf3/net/other_sample.h`, `bf3/host/lag_comp.h`; `TargetQuery`'s segment fields `from`, `to`, `radius`, `has_segment` (defaults 0 / false; filled by R4) |
| nt | the v2 declarations in `bf3/net/protocol.h` (MessageType values, MatchInfo / Spawned / DebugRequest / DebugReport / CommandPacket / OtherSoldier / Snapshot fields, `SnapshotOthers`, `ShotEvents`, `CombatEvents`, `KitSelect`, `snapshot_acked`, `others_acked`), `bf3/net/weapons_wire.h`; `kProtocolVersion` stays 1 |
| cl | `bf3/host/presentation_events.h`, `presentation_weapon.h`, `client_projectiles.h`; `ClientSessionConfig::mechanics_cfg`, `PresentedSoldier::target_pose` / `transition`, `PresentationFrame::newest_seq` |
| sv (by the task of W2.0) | the `Command` view sample: `view_back_q8` (u16), `view_quality` (u8, `sim::ViewQuality`), `view_seq8` (u8). `Command` is 24 B with no padding |
| vm | `bf3/render/viewmodel_set.h`, `bf3/anim/viewmodel_anim.h` |
| fx2 | `bf3/fx/render_prep.h`, `bf3/fx/atlas_pixels.h` |
| hud | `bf3/host/hud_feed.h` |
| sv, cl, bt (tests) | `tests/gunplay_fixture.h` |

Only `weapon_rules.cpp` has a body. It forwards to `step_soldier_weapons` and applies neither §5.2 rule; sv replaces it.
Every other function is declared only. A caller links once the owner's body has landed.

## m2a.contract

`ctest -R m2a.contract` (`tests/m2a_contract_tests.cpp`, suite `m2a-contract`, test target `test_m2a_contract`). No
install. It compiles every wave-2 header in one translation unit with the fixture, and checks:

* `static_assert`s: `sizeof(Command) == 24` with every field offset, `ShotView` 16 B, `PresentEvent` 64 B,
  `OtherPose` 60 B, `CapsuleLocal` 32 B, `FxInstance` 80 B; `weapon_buttons::kAll` inside `buttons::kWireMask` and
  disjoint from the motor bits; the shared enum values (`ViewQuality`, `weapon_flags`, `SampleKind`, `RewindVerdict`,
  `KitRole`, `kKitSlots`); the §16 defaults (Q1 `kDefaultMaxRewindTicks` 21, Q7 0.3 s at level 1).
* Every weapon input round-trips through the buttons, and the new MessageType values do not collide with M1's.
* The defaults of every field that W2.0 added.
* The §7.2 ack table: five rows of (newest, `snapshot_ack_bits`, `others_ack_bits`) and the seqs each field acks,
  including the wrap at 2^16, all 32 bits, no bits and only the top bit. The rows run against test-local reference
  predicates in `namespace m2a_fake`. M1's `snapshot_ack_bits` also round-trips through M1's command codec.
* exact_aim, on the synthetic M16A3 with infinite magazine. Three arms run 300 ticks each into a `ProjectileWorld`
  over walls at 40 and 80 m, with three capsule targets at 20 m:
  - flag off, with the range post-process;
  - flag on, without the post-process;
  - plain (neither).
  The run must fire at least 25 shots (31 now) and produce damage. The post-process may change only events, never
  state, and the plain directions must differ from the view.
* The three `step_weapons_m2a` rows of §5.2:
  - zoom held from spawn through the deploy;
  - zoom held while sprinting;
  - a counter of 2 in the first command after spawn, then a change of 1.
* 600 weapon states generated for the wire round-trip.
* A fixture smoke case: a loopback session on the synthetic floor spawns, and a Teleport is answered and seen.

**Pending rows.** A row whose subject is a body still to land checks nothing until its owner sets the switch at the top
of the test file, in the commit that lands the body:

| switch | owner | turns on |
|---|---|---|
| `kSvRulesLanded` | sv | `weapon_table_hash` differs with `exact_aim`; flag-on ShotEvents equal the post-processed ones field for field (floats by bit pattern), plus the same impacts, damage and state hashes; `make_weapon_table` carries the option; the three `step_weapons_m2a` rows |
| `kNtLanded` | nt | `kProtocolVersion == 2`; `net::snapshot_acked` / `others_acked` against the reference on every row; `others_ack_bits` through the v2 codec; the `WeaponsWire` round trip (view, bits, view, state, then one step from each must give equal views) |

A pending row prints `m2a.contract: PENDING (<key>): <row> -- <reason>` and a doctest MESSAGE. It does not mark the case
skipped, so the ctest passes. Its arms still run, and the log reports what the stub gives today, for example:

```
rows 1-3 with the current weapon_rules.cpp: fail / fail / fail
```

Calls of declared-only functions sit in `if constexpr` branches. Outside a template, a discarded statement does not
odr-use them, so the test links before their bodies exist.

**Deviation from §4.2.** The exact_aim equality is not a W3 run through `run_range`. `run_range` needs the install's
`SoldierDef` and material table: its stand-in targets fail on an empty table. The row is the same loop written out on
`weapons_support.h`'s synthetic table, using `step_soldier_weapons`, the post-process copied from `shooting_range.cpp`,
`ProjectileWorld` and `StaticTargets`.

## gunplay_fixture.h

The fixture lives in `namespace bf3::test::gunplay`. Everything in it is inline, and later tracks extend it with blocks
headed `// M2a <key>` at the end. The levels need `OPENBF3_MOTOR_TUNING_CFG`, which the test target defines (as
host.session does).

* `synthetic_floor()`: a 240 m floor with 12 spawn points, the synthetic mpsoldier and the shipped tuning.
* `metro()`: MP_Subway / TeamDeathMatch0 through `load_shared_level`. It reads the install through the repo's readers,
  uses a fresh scratch cache root under the temp directory, and skips (exit 77) without an install.
* `check_install_unchanged(Metro&)`: the install-unchanged check.
* `SessionOptions`: netsim, slots, walkers, dev commands, seed, clock offset and the client mechanics cfg, plus the
  hooks `configure_server` and `configure_client` that sv and cl use to set their configs.
* `start_session`, `poll`, `run_for`, `wait_spawned`, `wait_report`, `own_eye`.
* Dev requests `teleport`, `spawn_at`, `trace`, `spawn_target` and `target_info`. The last two are answered once sv
  handles those kinds; until then an M1 server drops them, and the helpers return nullopt after their timeout.

doctest runs with `DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`, so a failed REQUIRE does not leave the case.
`start_session` returns null on failure, and callers check for it.

## Deviations from the design sketches (for the owners)

* `build_capsule_pose_table(const Skeleton &, const ServerPoseClips &, const CapsuleTableInputs &)` takes the capsule
  definitions as `CapsuleTableInputs`, because `bf3_anim` links `bf3_core` only, not `bf3_sim`. lc may reshape it.
* `fill_hud` takes the presented weapon (`PresentedWeapon`) and the drained events, not a whole `PresentationFrame`,
  so hud can test it without a session.
* `ServerView` is an abstract interface (`server_view.h`). sv's `GameServer` implements it.
* The MessageType values (`SnapshotOthers` 3, `ShotEvents` 4, `CombatEvents` 23, `KitSelect` 24) and the DebugKinds
  (`SpawnTarget` 6, `TargetInfo` 7) are the contract's choice; nt may renumber them before its merge. m2a.contract
  pins only that they are distinct from M1's.
* `PresentationEvents`' private members carry no `alignas`: MSVC's /W4 warns (C4324) on padding. cl owns the
  layout.

## Notes for the tracks

* **sv:** no M1 test pins `sizeof(Command)` or a command hash. A grep of `src/`, `tests/` and `tools/` found none, and
  the full ctest passed with the 24-byte Command. No M1 pin was updated. `Command::operator==` is defaulted, so it now
  compares the view sample too; M1 senders leave the fields 0.
* **cl:** `host::OtherSample` is not yet aliased to `net::TimedOther` (that is cl's switch, with R1). `OtherSoldier`
  grew by 6 bytes, so `PresentationFrame` grew by about 3 KB (19,784 B now). M1's client resets its
  `snapshot_ack_bits` to 0 when the newest seq jumps by 32 or more; this is minor, and noted for nt and cl. The
  `InputSample` fields (`fire_presses`, the render time, `view_seq8`) and the frame's weapon views are cl's to add.
* **lc / sv:** `bf3_host` does not link `bf3_anim` yet. Whoever first calls `build_capsule_pose_table` from `bf3_host`
  adds the link in their own block.

## Gates (one command each, from the worktree)

```
tools\build.cmd dev --no-test
set OPENBF3_GODOT=C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe
ctest --preset dev -E "^(godot\.|host\.session|host\.server|host\.scale|sim\.nav\.bake|sim\.weapons\.perf|tools\.fx_bench)"
ctest --preset dev -R <one timing row>        (each separately, under the quiet lock rules)
tools\build.cmd asan --no-test
ctest --preset asan -R m2a.contract
```

The results of this commit are in the README section "M2a wave 2 contract (W2.0)". Timing rows are wall-clock and
noisy on this shared machine.
