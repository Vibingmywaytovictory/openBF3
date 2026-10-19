# M2b wave 1 contract (W1.0), as built

The integrator's contract commits for M2b wave 1 (docs/design/m2b.md §11 "Wave 1", §11.1 step 1), on branch `m2b/w1`
from `main` 36eb5af. They declare the wave-1 headers and insertions of m2b.md §2.6 #1-#15 and #19-#22, the v3 codec
stubs, the `tests/tdm_fixture.h` declarations, the `m2b.contract` row and Q8's `retired` label. Bodies exist only where
the tree must link: the six v3 codecs and `add_tdm_bots`, each line marked `// M2b W1.0 stub`, and the inline
`net::restrict_command` (the body is the interface, below). Everything is additive: no existing line changed except
the `dev-window` preset's filter (one named replacement, commit 5e6fe64).

Where m2b.md and the code disagree, the code was followed; §3 lists each difference.

## 1. What the commits contain

| commit | what | m2b.md |
|---|---|---|
| 2c78473 | nt: protocol v3 declarations in `protocol.h`: `MessageType::MatchState` 25, `PlayerList` 26, `SpawnRequest` 27; `MatchInfo::logic_digest`, `rules_flags`; `Spawned::revived`; `DeathEvent::respawn_tick`, `revive_until`, `killer_health`; `ReviveEvent`, `CombatEvents::revives`; an end block with `rules_flags`, `restrict_bits`, `restrict_command` (inline), `MatchState`, `PlayerRow` (32 B), `PlayerList`, `SpawnRequest`, `kMaxPlayerRows` 64, `kMaxPlayerNameBytes` 16, `kPlayerListIntervalTicks` 15 and the encode / decode declarations; the six stub bodies at the end of `protocol.cpp` | #11, §3.7 |
| c020f89 | `PresentKind::Revive = 9` appended | #12 |
| 7f54508 | `PresentationFrame`: `match_valid`, `match`, `player_count`, `players[64]`, `own_respawn_tick`, `own_revive_until`, `own_unspawned`; `ClientSession::send_spawn_request(uint8_t)` | #12 |
| 7919f15 | `NativePortMeaning` 16-29 appended after `Activate` (15): Kills .. EnablePlayerMovement | #5 |
| 40f136f | `MotorParams::protect_abort_buttons` (u32, 0), `protect_abort_move` (false) | #19 |
| 666de9a | new `bf3/sim/logic/round_natives.h` (RoundNativeConfig, RoundPhase, RoundNatives, NativeCoverage); the end block of `spawn.h` (FriendlyDeath, SpawnQuery, `choose_spawn_scored`) | #4, #6 |
| 54e42ed | bb: new `bf3/sim/bots/perception.h`, `aim_model.h`; listed in bf3_sim's CMake block | #13, §5.1, §5.3 |
| 62c70e3 | `DownedSoldier` and `ServerView::downed`, `phase`, `input_restricted`, `kit` with default bodies (empty, InRound, false, 0) | #10 |
| 70c6090 | `kits.h`: `kKitGadgetSlot`, `KitTable::defib_def`, `KitTableOptions`, the 4-argument `make_kit_table`; `gunplay.h`: `CombatEventSink::revive` (empty default), `GunplayConfig::defib_def`, `GunplayLoadOptions`, the 4-argument `load_gunplay_level` | #7, #8, #9 |
| d31e651 | `SharedLevel::logic`, `logic_digest`, `seconds_logic`; `SharedLoadOptions::tdm`; the 3-argument `load_shared_gunplay` | #3 |
| 2147df9 | ru / bb: new `bf3/host/tdm_rules.h` (MatchRulesConfig, RulesOrder, SpawnRefusal, RulesStats, TdmRules) and `tdm_brain.h` (TdmBotStats, TdmBotConfig, TdmBrain, `add_tdm_bots`); the stub `src/tdm_brain.cpp` | #1, #2, #13 |
| a5a50b5 | `game_server.{h,cpp}`: `[cfg]` (`GameServerConfig::rules`, `tdm_bots`), `[tdm bots]` (the call after `add_firefight_bots`), `[sv]` (`Sv::rules`, `Impl::sv_policy`, `sv_revive`, `sv_unspawn`, `sv_orders`, declared only) | #1, #20 |
| 63dc343 | au: new `bf3/core/audio/hdr_setting.h`, `sound_scope.h`, `bf3/audio/hdr_window.h`, `voice_cull.h`; CMake blocks in bf3_core and bf3_audio | #14, #15 |
| 96c4f84 | `MixerConfig::hdr`, `max_voices` (128), `scopes`; the MixerStats A3 profiler fields | #15, §6.3 |
| 9184939 | new `tests/tdm_fixture.h` (declarations only) and `tests/m2b_contract_tests.cpp`; the `m2b.contract` row | #21, §11 |
| 5e6fe64 | Q8: `retired` on `godot.m2a`, `godot.m2a.kits`, `godot.m2a.play`; the `dev-window` preset excludes `^retired$` | §11, §13 Q8 |

### 1.1 Who writes which body

| owner | bodies |
|---|---|
| nt | the v3 codecs (R9, replacing the six stub lines) and `kProtocolVersion = 3` with the two pins that hold 2 today; the client half of #12 (decode into the frame, `send_spawn_request`, `restrict_command` on predicted commands, `Revive` events) |
| ru | `round_natives.cpp`, `choose_spawn_scored`, R4's binding rows in `logic_native.cpp`, `[abort]` and `[abort hash]` (#19), R11 / R12 (#22), `tdm_rules.cpp`, the `[kits]` insertion and the 4-argument overloads (#3, #9), the `[sv]` member bodies and the other tags of §3.2, the `ServerView` overrides in `[view]`, the bodies of `tests/tdm_fixture.h` |
| bb | `perception.cpp`, `aim_model.cpp`, `tdm_brain.cpp` (replaces the stub), counters in `tdm_fixture.h` (a `// M2b bb` block) |
| au | `hdr_setting.cpp`, `sound_scope.cpp`, `hdr_window.cpp`, `voice_cull.cpp`, the MixerCore blocks |

Nothing in the tree calls a declared-only function: the `[sv]` members, `TdmRules`, `RoundNatives`, the perception
and aim functions, the audio readers and the fixture are referenced only in `m2b.contract`'s `if constexpr` branches.

## 2. The shared rule written in W1.0: `net::restrict_command`

m2b.md §3.7 and #12 say that the server (`[cmd]`) and the client apply "the same rule" to commands below
`restrict_until_tick`. So that both ends cannot diverge, the rule is one inline `constexpr` function in `protocol.h`:
`restrict_command(Command, restrict_until_tick, allowed_mask)`. It is active while `restrict_until_tick != 0` and the
command's tick is below it; a refused `kThrottle` / `kStrafe` axis reads 0, and a refused Jump, Fire, Zoom, Reload or
Sprint button reads as released. Yaw, Pitch, Crouch and Prone are not applied (holding them needs the previous
command; Metro allows them). Melee and ThrowGrenade need Fire in M2b, so Fire covers them [inferred, measured =
false]. `m2b.contract` pins its rows with `static_assert`s.

## 3. Differences from m2b.md (the code was followed)

* **`kProtocolVersion` stays 2 in W1.0** (#11 lists `= 3`). Two existing rows pin 2 (`net.proto.unit`,
  `m2a.contract`). Also, a tree whose v3 codecs are stubs cannot speak v3. nt sets 3 in its codec commit, updates both
  pins and sets `kNtLanded`.
* **`Sv::rules` is `std::unique_ptr<TdmRules, void (*)(TdmRules *)>`** (#20 says `std::unique_ptr<TdmRules>`). With the
  default deleter, `Sv`'s destructor in game_server.cpp instantiates `delete` and so needs `~TdmRules`, which has no
  body until ru lands: the tree would not link. ru sets the deleter where it creates the rules in `[init]` (the pattern
  of `Impl::sv`), or switches back to the default deleter in the commit that defines `~TdmRules`.
* **`TdmRules::apply_input_restriction(const sim::Command &) const` returns a `sim::Command`.** The sketch
  (§3.2) has `uint32_t apply_input_restriction(command_tick, buttons, int8_t &forward, int8_t &right)`. The Command
  form lets `[cmd]` and the client call the same `net::restrict_command` (§2), and covers bits beyond buttons and the
  two axes without changing the signature.
* **`TdmRules::request_spawn(slot, kit, spawn_choice, tick)`** takes `SpawnRequest::spawn_choice` (#11 declares it
  "reserved for M4's flag spawns"). Without it, `[recv]` would drop the field. In M2b, ru refuses nothing on it and
  treats any value as 0.
* **`run_tdm(const TdmLevel &, const TdmRunConfig &)`** takes the loaded level (#21 writes `run_tdm(const
  TdmRunConfig &)`). `run_firefight` (firefight_run.h:159) takes its level the same way, and the level is loaded once
  per process.
* **`MixerStats::blocks`** (#15) already exists in M2a (mixer_core.h:103). W1.0 does not add it again; the A3 fields
  start after it.
* **Line numbers.** The `[tdm bots]` line is after `add_firefight_bots` in `GameServer::init`. That is :811 on main
  and :821 after the `[sv]` and include insertions. The Q8 labels are an appended `set_property(... APPEND PROPERTY
  LABELS retired)` block at the end of tests/CMakeLists.txt, not an edit of the `add_test` lines m2b.md cites
  (tests/CMakeLists.txt:1333-1336). The rows stay runnable by name through the build directory:
  `ctest --test-dir b\dev -R "^godot\.m2a$"` (the `dev-window` preset now excludes them).
* **Not written in W1.0, as m2b.md assigns them:** R4's binding rows (#5), `[abort]` / `[abort hash]` (#19), R11 /
  R12 and the defib case (#22), the `[kits]` insertion and the 4-argument overloads' bodies (#3, #9). The contract
  states each rule, and ru writes it. Until then, an M2a load cannot reach the new overloads, so every M2a row is
  unchanged.

## 4. m2b.contract

`tests/m2b_contract_tests.cpp`, suite `m2b-contract`, functional, parallel-safe, no install. The file holds
namespace-scope `static_assert`s and then five cases:

* **Shapes, enums and the restriction** (the `static_assert`s, checked at compile time): `PresentEvent` 64 B;
  `net::PlayerRow` 32 B without padding; MatchState / PlayerRow / ReviveEvent / SpawnRequest / DownedSoldier /
  RulesStats / PresentationFrame are trivially copyable; NativePortMeaning 16-29; MessageType 25-27;
  `PresentKind::Revive` 9; RoundPhase, SpawnRefusal, `RulesOrder::Kind`, `rules_flags`, `restrict_bits`;
  `kKitGadgetSlot` 2, `kKeepKit` 0xFF, `kMaxPlayerRows` 64, `kMaxPlayerNameBytes` 16, `kKitSlots` unchanged; the
  `restrict_command` rows.

The cases:

1. **The defaults of every W1.0 field keep M2a** (§11.1 "Without rules").
2. **The hashes an M2a load keeps**, recorded at W1.0 on main 36eb5af's motor_tuning.cpp and kits.cpp:
   * `motor_params_hash(MotorParams{})` = `0x346282435cd97947`
   * `kit_table_hash(KitTable{})` = `0xa23fb7a00909ad6d`
   ru's `[abort hash]` must leave the first one unchanged (it hashes the two fields only when they are not the
   default). The rows checking that set fields change both hashes wait for `kRuLanded`.
3. **The v3 stubs**: each encoder returns an empty vector, and each decoder returns the error `v3 codec: nt`
   (switched by `kNtLanded`, after which the case checks version 3 and the three round trips).
4. **GameServer::init with the W1.0 hooks** on `tg::synthetic_floor()`: without rules or TDM bots it is unchanged,
   and ServerView's defaults hold. A `TdmBotConfig` with count 0 adds nothing. With count 4 before bb lands, init
   fails with the stub's "W1.0 stub" error. ru's row (rules without a logic graph refused) waits for `kRuLanded`.
5. **The HDR window**: pending (au), E10's numbers once `kAuLanded`.

Pending switches: `kNtLanded`, `kRuLanded`, `kBbLanded`, `kAuLanded`. Each owner sets theirs in the commit that lands
the body. Until then, the row prints `PENDING (<key>)` and checks only what the contract alone defines.

## 5. Gates, one command each

* `tools\build.cmd dev`: the branch pass (no timing, realtime or window rows).
* `ctest --test-dir b\dev -R "^m2b\.contract$"`.
* `ctest --preset dev-window -N`: the window set without the three retired rows.

Measured results: README "M2b wave 1 contract (W1.0)".

## 6. Track worktrees

From `m2b/w1` at the last W1.0 commit:
* nt, au, ru and bb, each on branch `m2b/<key>` at `C:\Users\olku\BF3 reimplementation\wt\m2b-<key>`;
* submodules initialised with `--reference` to the main checkout's modules;
* `bf3-opensource/.godot/extension_list.cfg` written as in the other worktrees.
