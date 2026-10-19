# M2b nt: protocol v3 and the client session, as built

Track nt of M2b wave 1 (docs/design/m2b.md §11, the nt row of §10; the contract: docs/design/m2b-w1.md), on branch
`m2b/nt` from the W1.0 contract head 01a26e8. nt owns R9 (the v3 codecs, #11), the client half of #12 in additive
blocks of `client_session.{h,cpp}`, the v3 fields of `presentation_events.h`, the shot-id reuse fix of the M2a gate
(m2a-gate.md:108) with its row, the v3 block of `net_proto_tests.cpp` and the v3 cases of `net_proto_fuzz.cpp` (the
wire half of E7). No game_server edit.

Where m2b.md or the W1.0 comments and the code disagree, the code was followed; §6 lists each difference.

## 1. Commits

| commit | what |
|---|---|
| 0606ead | R9: the v3 codecs replace the six W1.0 stub bodies in `protocol.cpp`; `MatchInfo`, `Spawned` and `CombatEvents` gain their v3 fields on the wire; `kProtocolVersion = 3`; the as-built wire table in `protocol.h`; `set_player_name` / `player_name`; the v3 block of `net_proto_tests.cpp` and the v2 rows that pinned the v2 layout; the v3 types and samples of `net_proto_fuzz.cpp` |
| e76f510 | named replacements: `m2a_contract_tests.cpp` (`kProtocolVersion == 2` -> 3) and `m2b_contract_tests.cpp` (`kNtLanded = true`) |
| 2399ff5 | the `// M2b nt` blocks: end of `client_session.h` (OwnShotIds, present_own_shot_id, PlayerTable, own_unspawned_at, rules_refusal), end of `client_session.cpp` (their bodies, the Impl hooks, `send_spawn_request`), end of `presentation_events.h` (the v3 fields, `present_flags::kRevived`); the new row `host.client.m2b.unit` (`tests/client_session_m2b_tests.cpp`, a `# M2b nt` block in `tests/CMakeLists.txt`) |
| 755edb4 | the named insertions of §3.1 |
| (docs) | this file and the README section |

## 2. The wire (protocol v3)

The bodies after the type byte, as `protocol.h`'s table at the end of the file has them. Every message is on ch1.

| message | body | bytes |
|---|---|---|
| `MatchState` (25) | u8 phase (<= RoundOver), u32 round_start_tick, u32 countdown_end_tick, i32 kills[0], i32 kills[1], i32 kill_limit (>= 0), u2 winner (<= 2), bool killer_camera, u32 round_end_tick, u32 unspawn_tick, u32 restart_tick, u32 restrict_until_tick, 13 bits restrict_mask | 1 + 39 |
| `PlayerList` (26) | u32 server_tick, bool full, u7 rows (<= 64), u7 removed (<= 64, 0 when full); the rows in strictly ascending slot; the removed slots (u6), strictly ascending, none a row's. A row: u6 slot, u2 team (<= 2), u2 kit, bool bot, bool alive, u5 name_bytes (<= 16), u16 ping_ms, u16 kills, u16 deaths, i32 score, the name bytes (valid UTF-8) | 13-29 a row; 64 rows of the test's names 1,075 |
| `SpawnRequest` (27) | u8 kit, u8 spawn_choice; any value decodes (the server validates and counts, `SpawnRefusal::BadKit`) | 1 + 2 |
| `MatchInfo` | + u64 logic_digest, u8 rules_flags (an unknown bit is refused) | + 9 |
| `Spawned` | + bool revived | + 1 |
| `CombatEvents` | + a varint revive count after the detonation count (all four within `kMaxCombatEventsPerMessage`, revives cut last); a death + bool stamped, then (stamped) u32 respawn_tick, u32 revive_until, 24 bits killer_health in 1/100 hp; a revive: u7 reviver, u7 revived, varint ticks back from server_tick | empty 9; a revive + 3; a stamped death + 11 over an unstamped one (1 bit) |

The decoders bound every count against the bytes left before allocating (PlayerList: 97 bits a row, 6 a removed
slot; CombatEvents: 22 bits a revive, the death minimum + 1), check every enum (phase, winner, team, the rules flags,
a killer health code above 100,000 hp) and refuse trailing bytes. The encoders write the canonical form and count every
change in `r_clamped`: PlayerList drops a second row of a slot and a slot >= 64, writes team > 2 as 0 and kit >= 4 as 3,
cuts a name to its longest valid UTF-8 prefix of at most 16 bytes, drops duplicate removed slots, removed slots that
are a row's and every removed slot of a full list; CombatEvents clamps a revive after the message's tick to it and
cuts a revive naming a slot >= 128. MatchState writes an out-of-range phase, winner or negative kill limit as the
nearest legal value. So decode -> encode -> decode -> encode gives equal bytes for every decodable input (the fuzz's
stability rule). `set_player_name` is the server's truncation of a Hello name (§9.3); `player_name` reads the used bytes.

## 3. The client session

* **MatchState, PlayerList** (`m2b_datagram`, from `handle_datagram`'s default branch): the newest MatchState goes to
  `PresentationFrame::match` (`match_valid` from the first); PlayerList goes through `apply_player_list` (a full list
  replaces every row, a changed-rows list updates its rows and removes its removed slots) into `players` /
  `player_count` in ascending slot. A datagram that fails to decode counts `bad_m2b_datagrams`.
* **The death stamp** (`m2b_combat`): the own DeathEvent's `respawn_tick` / `revive_until` become `own_respawn_tick` /
  `own_revive_until` while the life that died is the current one (a new life, a revive included, clears them). Every
  Death event carries `value` = killer_health, `extra` = respawn_tick, `seed` = revive_until.
* **Revives**: each ReviveEvent becomes a `PresentKind::Revive` event (slot = reviver, aux = revived, id = the revive
  tick, time = `tick_end(tick)`, timeline Render, `kOwnShooter` / `kOwnVictim` for the own soldier); a slot >= 128
  counts `bad_gunplay_events`.
* **The revived spawn**: `Spawned::revived` is remembered per life; the own Spawn event of that life gets
  `present_flags::kRevived`. The new life is taken as any spawn (`authoritative`).
* **The restriction** (`m2b_command`, right after `cl_command_made`): `net::restrict_command(cmd,
  restrict_until_tick, restrict_mask)` on every predicted command, so the pre-round freeze switches on the same tick at
  both ends; a changed command counts `restricted_commands`. The stored commands the catch-up replays are the
  restricted ones.
* **Unspawn** (`m2b_unspawned`, in `client_tick`'s prediction condition): once `MatchState::unspawn_tick` is set, a
  life that spawned before it is not predicted (and sends no commands) from that tick on; the frame then has
  `valid = false` and `own_unspawned = true`, and `unspawned_ticks` counts the ticks. A life spawned at or after the
  unspawn (the restart) predicts again.
* **The rules check** (`m2b_rules`, first in `cl_match_info`): a server with `rules_flags::kTdm` needs a client load
  with a logic graph (`SharedLevel::logic`) whose digest equals `MatchInfo::logic_digest` when the server sends one;
  otherwise the client is refused before Ready like a kit-hash mismatch ("rules mismatch (...)", `rules_mismatch`).
* **SpawnRequest**: `send_spawn_request(kit)` queues (the newest 16, under `debug_mutex`) and the client thread sends
  `SpawnRequest{kit, 0}` reliably at its next loop (`spawn_requests`).

### 3.1 The named insertions

All in `client_session.{h,cpp}`, each line marked `// M2b nt`:

* `ClientStats`: the M2b counters at the end (`match_states`, `player_lists`, `revives`, `bad_m2b_datagrams`,
  `restricted_commands`, `unspawned_ticks`, `spawn_requests`, `own_shot_id_resets`, `rules_mismatch`).
* `ClientSession::Impl`: the M2b members and hook declarations at the end of the struct.
* `handle_datagram`: `m2b_spawned` after `cl_spawned`; `m2b_datagram` before `cl_datagram` in the default branch.
* `client_tick`: `&& !m2b_unspawned(c)` in the prediction condition (a named replacement); `m2b_command` after
  `cl_command_made`.
* `publish`: `m2b_publish` after `cl_publish_end`. `run`: `m2b_send` before `release_out`.
* `cl_push`: the event goes through `m2b_push` (the presented ids, the revived flag) before the ring (a named
  replacement of the push's argument).
* `cl_match_info`: `m2b_rules` first. `cl_datagram`: the three Death fields; `m2b_combat` after the detonations.
* `cl_predict_weapons`: `step_own` writes into `m2b_own_events`, which `m2b_drain_own` drains through `cl_push` (a
  named replacement of the ring argument).

## 4. The shot-id reuse (m2a-gate.md:108)

**Root cause.** The server keeps `next_shot_index` over lives (gunplay.cpp:407). The own client emits an OwnShot at
the first prediction of each tick. When the server's soldier dies at tick D, the client learns it about an RTT later
and replays from the Dead state; the shots it predicted after D were emitted, the server never fired them, and the
replay (which emits nothing) leaves the lane at the server's index. The next life (`cl_weapons_spawn` spawns the
loadout over the lane's newest state, client_session.cpp:1801-1817) starts at that index, so its real shots carry the
ids of the never-fired ones. The effect triggers start once per (kind, slot, id, pellet, ordinal) and drop them as
double starts (the gate's double impact / muzzle cues). A weapon rebase that lowers the index within a life (a shot
predicted that the server did not fire) does the same on a smaller scale.

**Fix.** `OwnShotIds` (client_session.h) gives the own shots presented ids. `emit()` takes each OwnShot's real index;
one not above the last one emitted opens a segment whose offset puts it above every presented id so far, and within a
segment presented ids follow the real ones (bursts stay consecutive for weapon_audio.cpp's id arithmetic). `lookup()`
maps the real index of every later event about an own shot: the newest emission of that index among the last 256,
else the newest segment starting at or below it. `cl_push` applies it (`present_own_shot_id`) to the own OwnShot, the
own world's Impact and SoldierContact (now routed through `cl_push`), and a Hit, a weapon Death or a Detonation of the
own slot. Without a reuse every presented id equals the real index; the effect seeds stay computed from the real index
(the own world's impact seeds and ordinals are ClientProjectiles' own, unchanged).

**Row.** `host.client.m2b.unit` reproduces the path with the client's own pieces (synthetic floor, `sim::WeaponLane`
on a `PredictionRing`, the spawn as `cl_weapons_spawn` does it): life 1 emits 12 shots from tick 51 to 140, 9 of them
after the server's death at tick 100; the replay from the Dead state emits nothing and leaves the lane at the server's
index; life 2 from tick 200 reuses 5 ids. Through OwnShotIds: 0 duplicates, 1 segment reset, and the server's Hit of
life 2's first shot maps to that shot's presented id.

**Not covered.** A Hit of a shot the client emitted before a reuse and the server fired after it (a within-life
misprediction of the order) maps to the newer emission; the hit indicator may then point at the wrong round. The fx
seeds of a soldier hit or a detonation by an own shot use the presented id after a reuse (cosmetic: a different
variant than other clients see).

## 5. Gates and measured results

Measured 2026-10-19 on the branch head, on a shared machine (other agents' builds running): every time below is
indicative only.

| gate | threshold | measured | command |
|---|---|---|---|
| net.proto.unit (v3 round trips, hostile bytes, canonical counts) | pass, <= 2 s | 50 / 50 cases (both suites of the binary), 1.2 s alone | `ctest --test-dir b\dev -R "^net\.proto\.unit$"` |
| E7 wire half: net.proto.fuzz on the v3 messages | 0 unstable, every structured case refused, 0 crashes | 18 types; 4,586 / 4,586 structured cases refused; 826,354 typed datagrams: 77,523 decoded and stable, 748,831 refused, 0 unstable; 2.6 s alone (busy machine; M2a measured 1.5 s) | `ctest --test-dir b\dev -R "^net\.proto\.fuzz$"` |
| m2b.contract with kNtLanded | the nt PENDING gone, pass | 5 / 5 cases, no nt PENDING (ru, bb, au remain) | `ctest --test-dir b\dev -R "^m2b\.contract$"` |
| m2a.contract pins v3 | pass | 7 / 7 cases | `ctest --test-dir b\dev -R "^m2a\.contract$"` |
| the shot-id reuse row | reproduced without the fix, 0 duplicates with it | 5 reused ids raw, 0 presented, 1 reset | `ctest --test-dir b\dev -R "^host\.client\.m2b\.unit$"` |
| branch pass | 0 failed rows | 139 rows: 137 passed, 2 skipped (core.cache_data, core.res), 0 failed; 209 s; exit 0. Budget warnings only (indicative only on a branch) | `tools\build.cmd dev` |
| asan: the new parsers | 0 AddressSanitizer reports | 0 reports: net.bits.unit, net.proto.unit (1.58 s), net.proto.fuzz (10.5 s, asan), host.client.m2b.unit all passed | `cmake --preset asan && cmake --build b\asan --target test_net_proto test_net_proto_fuzz test_client_m2b && ctest --test-dir b\asan -R "^(net\.proto\.unit|net\.bits\.unit|net\.proto\.fuzz|host\.client\.m2b\.unit)$"` |
| install.unchanged | the install rows' install.unchanged cases pass | passed (every install row passed in the branch pass; nothing written to the install) | `tools\build.cmd dev` (the install rows of the branch pass) |

E7's server half (`host.tdm.server` `hostile*`) and E13 (`host.tdm.client`) are ru's rows.

## 6. Differences from m2b.md and the W1.0 comments (the code was followed)

* **Revive's id** is the revive tick, not "the revived slot's new life" (presentation_events.h's W1.0 line): the wire
  does not carry another slot's new life; that slot's next generation in the snapshots is it. The own revived life is
  the Spawn event with `kRevived`.
* **MatchState is 39 bytes** after the type byte (the W1.0 sketch had no size).
* **rules_refusal** checks one direction, as §3.7 says (the server has TDM, the client's load cannot follow). A client
  with a TDM load on a server without rules is not refused here; its kit hash (the assault gadget, #9) differs and
  refuses it when gunplay is on.

## 7. Open issues and handoff

* `fx::FxTriggers::sane` accepts kinds up to `Spawn`, so a `Revive` event is counted in `rejected` (no effect starts
  either way). The consumer of Revive (cam's revive stand-up, gx) should accept it.
* E13 (`host.tdm.client`, ru, timing) is the end-to-end proof of the client wiring against the real rules: no Spawned
  before the first `send_spawn_request`, the restriction predicted, the death stamp and the revived life in the frame.
