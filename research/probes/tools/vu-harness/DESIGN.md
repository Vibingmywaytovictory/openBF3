# S5 measurement harness (`mharness`): design

**Date** 2026-10-16 (third design pass, after review). **Branch** `s5/harness` (from main `7b3e8fa`). **Status**
design + skeleton; two tools are built and run on the skeleton (`luaload.py`, `py/mhlib/vudocs.py`, §16). Nothing has
run in VU. The user runs Phase 0 and Session 1 tonight from [RUNBOOK.md](RUNBOOK.md) once `check_all.py` passes (or,
for Phase 0 and the attended items only, `check_all.py --phase0`, §14).

**Specification.** `research/topics/gap-r1-measurement-harness.md` (cited **R** with its section, e.g. R §3.5). Plan
context: `research/PORTING-RESEARCH.md` §3.13, D7, D12, D18, R3, R5 and the oracles in §7.1-§7.3 (cited **P**).
Consumers of the results: `src/bf3_sim/include/bf3/sim/weapons/mechanics.h` and `docs/design/m2a-weapons.md` §5.6,
§5.9, §6.9 (branch `m2a/weapons`, not on main yet); `src/bf3_sim/include/bf3/sim/soldier_motor.h` `MotorTuning`,
`bf3-opensource/config/motor_tuning.cfg`, `docs/design/m1.md` §6.4, §6.6, §6.8, §6.11;
`src/bf3_core/include/bf3/core/gameplay/soldier_def.h` (regeneration fields) and `mode_def.h` `CapturePointDef` (main).

**Tags.** **[doc]**: the name, signature, enum value or read-only flag is in VU-Docs (`C:\Users\olku\VU-Docs`, commit
`f3cb9f19`), confirmed today by `py/mhlib/vudocs.py` for all 308 inventory entries (pass 3); page paths are relative to
`content\vext\ref\` unless they start with `content\`. **[research]**: cited from R §2 / §3.3. **[live:Lnn]**: VU
behaviour that Phase 0 check Lnn must confirm (§8). **[mock]**: an assumption the mock VU runtime makes (§13.8 lists
them; each maps to a live check). **[inferred]**: my design choice or reading. VU-Docs is an API reference only: no
text, type yaml or example from it is copied into this directory; Lua sources cite the page path in a comment, and
the inventory holds names, kinds, realms, page paths, enum values and read-only booleans (API facts).

---

## 0. Summary

* One VU server mod, `mharness`, in a separate clean instance (`Documents\Battlefield 3\ServerMeasure`), loaded alone.
  It prints one row per tick or event as `MH1 <json>` into the instance log (server rows) and the VU client log
  (client rows). Rows hold numbers, enum values, data-asset names and small integer ids only.
* Observe-only: hooks pass through; the only writes are to **our own test bots** (create, spawn, place, pose, aim,
  input, kill, remove, seat), each logged as an `fx` row. A static checker (§12.2) and the mock's write journal
  (§13.4) enforce it.
* Scenarios are coroutines resumed once per sim tick: state machines with `mark` rows at phase changes, a timeout on
  every wait, chat progress, and `prompt` rows for every instruction to the user. `!mh` chat commands start, stop,
  queue and report them; every command is safe to repeat.
* Verification without VU, in three layers: (a) `luaload.py` loads every mod file in a real **Lua 5.4** runtime
  (`lupa.lua54`; VU runs Lua 5.4, `content\vext\_index.md`) inside a VU-like sandbox with strict globals; (b)
  `apicheck.py` checks every VU call, event, hook, property and enum against `schema/api_inventory.json` and, at check
  time, against VU-Docs; (c) a **mock VU runtime** (`py/mockvu`, Lua under lupa + a Python host) runs every scenario end
  to end with physics and game laws chosen by switches, writes VU-format logs, and the extractor and analysis must
  recover the law the mock was set to. The mock is a test double for our assumptions, not VU (§13.8).
* Python tools (`python -I`, numpy only) extract rows into `research/measurements/<run>/`, validate them against
  `schema/rows_v1.json`, and apply the R §3.6 decision rules; each verdict names the exact switch or constant it
  changes in our code and drafts a `research/measurements/SOURCES.md` row.
* PowerShell: `install-manifest.ps1` / `install-compare.ps1` (R §4.2, read-only on the install) and
  `setup-instance.ps1` (the user runs it; refuses the normal instance and the install).
* Tickets are a budget (§6.12): bots are spawned once and kept on a bench instead of being respawned, targets are
  respawned only when the next hit could kill them, the queue forecasts the ticket cost of every item and refuses what
  cannot finish, and the overnight queue starts with a round restart.
* Tonight: Phase 0 (30-45 min attended), the attended Session 1 items, an unattended queue overnight (`rr WA:tier1 SB
  MB WA:rest rr MA nextmap MA`: W-A first, the bleeding M-A last on each level), and a `-high60` repeat in the morning
  (§6.12).

---

## 1. Hard rules and how each is enforced

| rule | enforcement |
|---|---|
| BF3 install read-only; agents never launch VU or inject | Only the user starts VU (RUNBOOK). `install-manifest.ps1` reads metadata and hashes and refuses an `-Out` that resolves inside the install (full path + junction/symlink resolution, §11.1). `setup-instance.ps1` and `extract.py` refuse any path under the install. Tests use scratch trees; one read-only real-install manifest run prints only its summary line. |
| Clean room | No Rime, fun-bots, unfun-bots, IceBloc, AssetBankPlugin, FrostyToolsuite or probe-ant material is opened. Bot fixtures and the mock are written from the VU API reference and R only. `vudocs.py` reads VU-Docs at check time and returns booleans and integers; nothing from it is stored except page paths and those facts. |
| Observe-only (R §3.2) | `apicheck.py` rejects the `forbidden_symbols` of the inventory (`MakeWritable`, instance/partition load handlers, bundle mounts, `hookCtx:Pass`/`Return`, `SetTicketCount`, `ApplyDamage`/`Impulse`/`Force`, `FireEvent`, `PropertyChanged`, `SetExternalInput`, kicks/bans/fades, `RegisterCommand` …) and any assignment of a `forbidden_assign` field (`teamId squadId health primaryAmmo team ticketCount flagLocation transform velocity …`) anywhere; engine-field assignment is allowed only in `mh_bots.lua` and only for `bot_assign` fields (`input`, EntryInput fields `deltaTime flags authoritativeAimingYaw/Pitch zoomLevel`, `selectedKit`); `fireLogic` is in `forbidden_assign` (fire mode changes go through the bot's own `EIACycleFireMode` input). Fixture calls only in `mh_bots.lua`, each in a function that emits `fx`. `RCON:SendCommand` only with a literal from the inventory's `rcon_read_whitelist` and no arguments, except the two admin commands below (apicheck rule 11). At runtime `mh_bots` refuses any player it did not create, and the mock journals every engine write and every RCON command and fails a test on a write to a non-bot object or a non-whitelisted command (§13.4). |
| Admin actions (accepted deviation) | `mapList.restartRound` and `mapList.runNextRound` are server-admin actions outside the R §3.2 fixture list (create, spawn, place, pose, aim, input, kill, remove, seat). They change the round, not game data or measured behaviour, and the user commands them: by chat with `confirm`, or as `rr` / `nextmap` items in a queue the user typed (no second confirm, so a 4 am level change does not stall). They are sent only from `mh_cmd.lua`/`mh_runner.lua`, each followed by `fx act=admin`. The RUNBOOK states this deviation in one line. |
| No other mods; vanilla kits | `setup-instance.ps1` writes `ModList.txt` with exactly `mharness`. `meta.mods` carries `modList.ListRunning` [live:L05]. Bots use vanilla unlock assets (no attachments) named in `scenarios/wa_cfg.lua`; the names go into `dict` rows. |
| No personal data in rows | The mod never subscribes to `Player:Joining` (its arguments are name, GUID, IP, account GUID) and never reads `name`, `guid`, `accountGuid`, `onlineId`, `ip` of anyone. Humans get ids 1-99 in order of first sight, bots 101+ (names `mh101` are ours). Weapons, materials, flags are small integers with `dict` rows. The `meta` var list is a whitelist without `vars.gamePassword`. `extract.py` drops and counts any string that looks like a GUID, an IPv4 address or a non-whitelisted key (§9). The server GUID needed to join is printed as a plain log line (not `MH1`) that the extractor never copies. |
| Never touch the normal instance; never read `server.key` | `setup-instance.ps1` refuses `...\Battlefield 3\Server` and anything inside it as a target (separator-aware, so `ServerMeasure` is not `Server`). Its only contact with the normal instance is copying the one file the user names with `-CopyKeyFrom`, bytes only, never printed or hashed to output. `extract.py` refuses the normal instance's `logs`. |
| No downloads or installs | Python stdlib + numpy + lupa 2.8 (already installed with the user's approval); PowerShell 7; git. |
| `python -I`, defensive parsing | Entry scripts add only their own `py\` directory to `sys.path` (isolated mode drops the script directory). Log lines capped at 64 KiB, JSON depth ≤ 6, `parse_constant` rejects NaN/Infinity, duplicate keys rejected, every failure counted by reason, never `eval` (§9). lupa runtimes are created with `register_eval=False, register_builtins=False`; the mod's environment never sees the `python` bridge. |

---

## 2. Files

```
research/probes/tools/vu-harness/
  DESIGN.md  RUNBOOK.md
  check_all.py                 one command: layout, JSON, luaload, apicheck, unittests, mock e2e, PowerShell tests
  luaload.py                   [built] every mod file compiled + loaded in lupa.lua54 under a VU-like sandbox
  apicheck.py                  static API / observe-only checker over the mod (+ --vudocs confirmation)
  extract.py                   logs -> research/measurements/<run>/raw.jsonl + meta.json
  analyze.py                   decision rules W1-W5, S1, S3, M1, M2, T1 (+ W6, W7, S2, W8-W10 reports)
  synth.py                     synthetic VU logs with known laws (both variants per rule) + malformed lines
  mockrun.py                   CLI: run one scenario in the mock VU runtime with chosen laws (debugging)
  schema/rows_v1.json          row schema (envelope + payload keys/types): extractor, apicheck, mock tests
  schema/api_inventory.json    every VU symbol the mod may use: realm, kind, doc page, enum value, RO flag, live check;
                               inheritance map; forbidden symbols / assignments; fixture and print modules
  data/weapon_constants.json   data values the rules compare against (P §7.1-§7.3, topics/01), with source
  ps/install-manifest.ps1  ps/install-compare.ps1  ps/setup-instance.ps1
  py/mhlib/                    vudocs [built], lualex (tokens for apicheck), logparse, schema, runs, stats (KS,
                               bootstrap), fits, rules_weapons, rules_soldier, rules_modes, rules_tick, sources
  py/mockvu/                   mock VU runtime: host.py (lupa runtimes, clock, logs, NetEvents bridge), laws.py
                               (switches + presets), user.py (scripted human), levels.py (mock level layouts)
  py/mockvu/lua/               vu_core.lua (object model, inventory-driven classes, events, hooks, journal),
                               vu_math.lua (Vec3, LinearTransform, Quat), vu_server.lua, vu_client.lua (libraries),
                               vu_world.lua (geometry, raycasts), vu_soldier.lua (motor, health, poses),
                               vu_weapons.lua (fire, GunSway, recoil, bullets, damage), vu_conquest.lua (flags, tickets)
  sims/w1crit.py               design-time simulation of the W1 rule (critical values, power, outliers; §10.1)
  tests/                       unittest modules (§14); tests/ps/run-ps-tests.ps1; tests/fixtures/
  mod/mharness/                master copy of the mod (copied into the instance by setup-instance.ps1)
    mod.json
    ext/shared/__init__.lua        requires the shared modules; nothing else
    ext/shared/mh_row.lua          JSON encoder, rounding, envelope, size cap, rate limiter, the only print()
    ext/shared/mh_clock.lua        tick counter k, sim time t, wall ms, run id
    ext/shared/mh_vec.lua          plain-number vector math (no VU operator overloads)
    ext/shared/mh_gs.lua           GunSway -> gs payload (server and client use the same field list)
    ext/shared/mh_cert.lua         generated by check_all.py (certified scenarios; git-ignored; §14)
    ext/server/__init__.lua        wiring: events, hooks, NetEvents, command registration
    ext/server/mh_ids.lua          small integer ids (players, weapons, materials, flags), dict rows
    ext/server/mh_chat.lua         replies, progress and prompts through ChatManager (+ prompt rows)
    ext/server/mh_cmd.lua          `!mh` parser and command table
    ext/server/mh_meta.lua         meta, census, RCON var reads
    ext/server/mh_bots.lua         the only fixture module (fx rows)
    ext/server/mh_sites.lua        recorded poses: wall, range, flat, ring centre (site rows)
    ext/server/mh_samplers.lua     hp, tk, cp, wf samplers and the watched set
    ext/server/mh_hooks.lua        EntityFactory:Create, BulletEntity:Collision, Soldier:Damage (pass-through)
    ext/server/mh_events.lua       ev rows
    ext/server/mh_runner.lua       coroutine scheduler, queue, stop/pause, timeouts, ticket guard
    ext/server/scenarios/registry.lua  scenario table; extension point for S2/S3 (V-*, A-*, M-C, N-A)
    ext/server/scenarios/p0.lua wa.lua wa_cfg.lua sb.lua wb.lua sa.lua ma.lua mb.lua wc.lua wd.lua
    ext/client/__init__.lua        client wiring
    ext/client/mh_cgs.lua          client gs rows (predicted / corrected)
    ext/client/mh_fov.lua          FOV probe
    ext/client/mh_cam.lua          camera transform replies for `!mh wall` / `range` / `ring` / `p0 aim`
research/measurements/             (created by extract.py)
  session-YYYYMMDD/manifest-before.csv manifest-after.csv compare.txt
  <run>/raw.jsonl[.gz] meta.json summary.json sources-draft.md
  SOURCES.md                       one row per accepted finding (written by an agent after review)
```

The mod and the mock are research tooling and are never shipped. The skeleton holds module headers and stubs.

---

## 3. Runtime architecture of the mod

### 3.1 Realms, Lua and loading

* **Lua 5.4** with the libraries `table string math os coroutine package utf8`; not available: `dofile load loadfile
  string.dump`, `package.config/cpath/loadlib/path/searchpath`, `os.execute/exit/getenv/remove/rename/setlocale/
  tmpname`; added: `string.starts`, `string.ends`, `json.encode/decode` (`content\vext\_index.md`) [doc]. Lua 5.4
  specifics the code must respect: `string.format('%d', x)` errors on a float with a fraction (the encoder converts
  with `math.floor`/`math.tointeger` first); `//` is floor division; integer and float keys are distinct only when not
  integral; `<const>` locals are allowed (lupa.lua54 and VU are both 5.4) [inferred].
* `require('x')` resolves inside the realm's `ext\<realm>` folder, `require('__shared/x')` inside `ext\shared`, paths
  are always relative to the realm root (not to the requiring file), `.lua` optional, other extensions refused, each
  file runs once and its return value is cached (`content\vext\guides\code-splitting.md`) [doc]. `luaload.py`
  implements exactly these rules (§12.1).
* Only `ext\<realm>\__init__.lua` runs at start; `ext\shared` scripts run on both server and client
  (`content\modding\structure.md`) [doc]. Whether `ext\shared\__init__.lua` and `ext\server\__init__.lua` share one
  Lua state (globals) and which runs first is **not documented** [live:L27]. The mod does not depend on either: each
  realm entry requires the shared modules itself; the only global is the table `MH`; the census reports `vm`
  (`same` if `MH._shared_entry` set by the shared entry is visible to the realm entry, else `separate`). `luaload.py`
  and the mock run both entry orders; the mock also runs the "separate states" variant.
* The VU log prefix is `[<time>] [info] [VeniceEXT] [<module>] <text>`: R verified it on the 2026-06-28 client log
  (module = mod folder name). The server log format is assumed identical [live:L01]; the extractor accepts any prefix
  ending in `[mharness] MH1 {` and a bare `MH1 {` line.
* Presence probes (the census, optional fields) are wrapped in `pcall`: reading a member that does not exist on an
  engine userdata may raise instead of returning nil [inferred; the mock raises, §13.2].
* Scenario coroutines never yield inside a VU C callback (`TraverseAllEntities`, an iterator loop body passed as a
  callback, an event handler): Lua 5.4 cannot yield across a C-call boundary. `ctx:wait*` is called only from the
  scenario's own coroutine body; callbacks collect into a table and return. apicheck rule 12 flags `ctx:wait` inside a
  function literal passed as an argument.
* Server mods' client scripts are sent to joining clients (R §3.3, client log "Received VeniceEXT module") [research],
  so client rows land in `%LOCALAPPDATA%\VeniceUnleashed\vu_*.log`.

### 3.2 Tick pipeline (server)

`UpdateManager:Update(deltaTime, updatePass)` (`shared/event/updatemanager_update.md`; `fb/updatepass.md`: PreSim 0,
PostSim 1, PostFrame 2, FrameInterpolation 3, PreInput 4, PreFrame 5) [doc].

| pass | work |
|---|---|
| `UpdatePass_PreSim` (0) | `mh_bots.apply_inputs(dt)`: write each bot's prepared `EntryInput` (fire, zoom, throttle, aim), with `input.deltaTime = dt` (the PostSim `deltaTime`, else `1/GetTickrate()`) set on every pass: a fresh `EntryInput()` presumably starts at 0 and VU or Frostbite may scale input by it; setting it is harmless if ignored (`shared/type/entryinput.md` lists it writable) [live:L08]. PreSim vs "write at the end of PostSim" is decided by L08: whichever gives the shorter, constant input-to-shot latency is used and recorded in `meta.aimcal.lat`, with `meta.aimcal.dt` |
| `UpdatePass_PostSim` (1) | `k += 1`, `t += deltaTime`; samplers (hp, tk, cp, wf) for watched ids; `mh_runner.step()` resumes the active scenario coroutine; limiter bookkeeping |

* Hooks and other events (`GunSway:Update`, `EntityFactory:Create`, `BulletEntity:Collision`, `Soldier:Damage`,
  `Player:*`) emit rows with the current `k` (the tick in progress). Rows inside one tick are ordered by `q`.
* If PostSim does not fire exactly once per sim tick [live:L03], `k`/`t` come from `Engine:Update(deltaTime,
  simulationDeltaTime)` accumulating the sim delta (`shared/event/engine_update.md`) [doc]; `meta.clk` records which.

### 3.3 Time base and run ids

* `k`: sim ticks since the run started; `t`: sum of PostSim `deltaTime` since the run started (s); `ms`:
  `SharedUtils:GetTimeMS()` (int, `shared/library/sharedutils.md`) [doc]; `q`: per-realm row sequence since mod load
  (gap detection).
* Run id `YYYYMMDD-<SC>-<HZ>-<HHMMSS>` from `os.date` at run start [live:L02] and `SharedUtils:GetTickrate()` (float)
  rounded. R §3.4 says `<n>`; the mod cannot persist a counter (no `io`), so the start time keeps ids unique across
  server restarts [inferred]. Rows outside a scenario belong to the session pseudo-run `YYYYMMDD-S-<HZ>-<HHMMSS>`.
* Client rows carry the server's run id (sent in `mh:ctl`), their own `k` (client frames since `mh:ctl`), `t`, `ms`,
  and `r:"C"`. A ping pair (`mh:ping` / `mh:pong`) records the server-client `ms` offset in a `p0` row [live:L19].

### 3.4 Ids (no personal data)

| id space | rule |
|---|---|
| players | humans 1-99 in order of first sight (event or command), bots 101-999; key = `Player.id` (RO int, an engine slot number, never logged). `dict kind=bot` rows give a bot's team, kit and weapon. |
| weapons `wid` | 1.. per distinct `SoldierWeapon.name` / unlock asset name; `dict kind=wpn` gives the data name (e.g. `Weapons/M16A4/U_M16A4`); the dict rows of every known wid are re-emitted right after each `meta` row, so each run carries the full wid → name map (pass 7). |
| materials `mat` | `RayCastHit.material` is `DataContainer | nil` (`shared/type/raycasthit.md`) [doc]; after a nil check it is cast `MaterialContainerPair(hit.material)` (a read) and logged as `[flagsAndIndex, physicsMaterialIndex, physicsPropertyIndex]` (`fb/materialcontainerpair.md`) [doc]. Which of these keys our material grid (P §3.2; probably the index bits of `flagsAndIndex`) is **unverified**: decided offline from the logged triples [inferred]. No GUID is logged (deviation from R §3.4 `mat` GUID and `dmg.wfire` GUID). `DamageGiverInfo.weaponFiring` is a `DataContainer` (WeaponFiringData, `server/type/damagegiverinfo.md`), not the runtime `WeaponFiring`: it is never used for instance matching; `dmg.wfd` carries its type name only. |
| flags `cp` | 1.. in iterator order per level; `dict kind=cp` gives `CapturePointEntity.name` (a data label). HQs are capture points too (our `CapturePointDef.is_hq()` = `CTUnableToChangeTeam`) [inferred for VU's iterator; live:L15]. |
| entities `uid` | `Entity.uniqueId` (RO, `shared/type/entity.md`) [doc]: links `shot` to `hit`. |

### 3.5 Rows, encoder, limits

* `mh_row.row(e, payload, prio)` adds the envelope (§4) and prints `MH1 <json>`. Own JSON encoder (deterministic,
  compact): integers with `%d` (after `math.tointeger`), floats with **`%.9g`** (pass 3; R §3.3 said 6 digits, which
  quantises world positions to 1 cm at |x| ≥ 1000 m against an ADS disc radius of 4-9 cm at 25 m, and cumulative `t`
  to 0.1 s past 10,000 s: a loss that cannot be recovered after tonight), Vec3 as `[x,y,z]`, quaternions `[x,y,z,w]`,
  booleans, labels. Non-finite numbers become `null` and count in `lim.nf`. Rows grow by about 30 % (§15). The
  analysis times everything from the integer `k` divided by `hz` (sub-tick order from `q` and `nup`), never from `t`.
  `json.encode` (VU) is not used for rows because its float and Vec3 output formats are not specified
  (`content\vext\_index.md` lists supported types only); P0 prints one `json.encode(Vec3(1,2,3))` sample [live:L02].
* Size cap 1000 B per row. An oversize row is replaced by `trunc {of, len}` (R §4.6: line limits unknown) [live:L20].
  Large payloads are chunked instead of truncated: `census` is emitted as several rows, each with one of `types`, `api`,
  `res` and `part`/`of`, ≤ 40 keys per row (a level has 100+ entity types; the API list has ~300 symbols); the mock
  test asserts no `trunc` row from any emitter. `meta` carries wids only (names in the `dict` rows that follow it),
  and `mh_meta.emit_meta` keeps it under the cap: it budgets 170 B for the envelope, leaves out `args` (`argsx`
  true; the `cmd` row has them) and then shortens `wpn` if the payload would still not fit (pass 7; the mock case
  `meta_many_weapons` sees 8 weapons in one Lua state: largest meta row 832 B).
* Rate limiter: token bucket 240 rows/s, burst 480. Priority 1 rows (`meta census dict fx mark site shot hit dmg ev cmd
  err p0 prog lim trunc prompt`) are never dropped; priority 2 (`gs wf hp tk cp veh fov`) are dropped first, `hp` of
  non-primary ids first. Each second with drops emits `lim {drop:{gs:n,...}}`. Expected peaks: W-A 1 shooter gs + wf
  at 30 Hz ≈ 35 rows/s; M-A tk 30/s + cp ≤ 10/s; W-C ring hp 8 × 30 = 240/s for 2 s (burst).
* Sampling: per tick only for watched ids (`mh_samplers.watch(id, what, until_k)`), else 1 Hz (`hp`) or on change
  (`cp`, `wf`).

### 3.6 Error containment

Every handler runs through `mh_row.safe(name, fn, ...)` (`pcall`). An error emits `err {where, msg, n}` (message
sanitised to the label character set, ≤ 120 chars). After 20 errors one handler disables itself and the chat says so.
A hook handler that errs before its `Call()` simply returns: the engine then runs the original, because when a handler
uses neither `Call` nor `Return` the engine proceeds itself, and when it uses `Call` without `Return` the engine
returns `Call`'s value (`content\vext\guides\hooks.md`) [doc]. `Call` also stops lower-priority handlers of that hook
(same page) [doc]; with no other mod loaded this changes nothing, and the mod installs its hooks at priority 1. A
scenario coroutine error aborts that scenario (cleanup runs, §3.8) and the queue continues.

### 3.7 Commands and chat

* `Player:Chat(player | nil, recipientMask, message)` (`server/event/player_chat.md`) [doc]. Messages starting `!mh`
  from a human player are parsed (bots never chat); tokens are `[A-Za-z0-9_.,:=+-]{1,32}`, at most 12. Unknown verbs
  get a one-line help.
* Replies: `ChatManager:SendMessage(message, toPlayer)` (`server/library/chatmanager.md`, returns bool; the message is
  limited to 127 characters) [doc]: ≤ 120 chars per message **including** the `[mh] ` prefix, ≤ 6 messages per reply.
  Phase changes optionally `Yell(message, duration, toPlayer)`. Every instruction to the user is also a `prompt` row
  (kind, n/of, weapon, zone, distance), so the extractor can time user actions and the mock user (§13.6) can follow
  them. Every `cfg-done` is echoed to chat (`[mh] WA cfg 3/17 done`), so a crash leaves the resume index on screen.
* Weapon names in arguments are matched case-insensitively against `wa_cfg.lua` keys and aliases (`870` = `870MCS`,
  `AK74M` = `AK-74M`, `M16` = `M16A3`); an unknown name is refused at parse time with the list of valid names.
* Fallback input path: the client registers a console command `mh` (`Console:Register(name, description, callback)` →
  `ConsoleCommand`, `client/library/console.md`) [doc] that forwards its text by `NetEvents:Send('mh:cmd', text)`
  (`client/library/netevents.md`) [doc]; the server treats it like chat from that player [live:L19]. `ConsoleCommand`
  has a read-only `groupName` (`client/type/consolecommand.md`) [doc], so mod commands are probably typed as
  `<group>.<name>` (likely `mharness.mh`): at load the client prints the full name `cmd.groupName .. '.' .. cmd.name`
  in a plain log line and in the greeting, and the RUNBOOK tells the user to try `mh status`, then `mharness.mh
  status`.
* Every command emits a `cmd {who, verb, args, ok}` row. Command reference §5.

### 3.8 Scheduler (`mh_runner`)

* One active scenario at a time plus a FIFO queue. A scenario is `{id, title, who, run(ctx, args), cleanup(ctx),
  estimate(args)}`. `run` executes as a coroutine resumed once per PostSim tick.
* Context API: `ctx:wait(s)`, `ctx:wait_ticks(n)`, `ctx:wait_until(pred, timeout_s, label)` (false on timeout, with
  `mark lbl=timeout:<label>`), `ctx:mark(lbl, fields)`, `ctx:progress(done, total, cfg)` (a `prog` row and a chat line
  at most every 30 s), `ctx:say(text)`, `ctx:prompt(kind, fields, text)`, `ctx:need_site(name)`, `ctx:guard()` (raises
  `stop` on `!mh stop`, round over, level destroy or ticket guard).
* **Stuck handling.** Every wait has a timeout. A config whose setup times out is skipped (`mark cfg-skip`); three
  consecutive skips abort the scenario (`mark lbl=aborted`, chat reason) and the queue moves on. A watchdog marks
  `stalled` if the scenario coroutine was not resumed for 120 s of sim time while not held (as built this is a
  last-resort check that practically never fires: every yield refreshes it). The real protection against a stuck
  scenario is that every wait has its own timeout and three skipped configs abort the scenario; a loop that never
  yields would hang the server (none exists: every scenario loop goes through the ctx waits).
* `!mh stop` sets a flag; on the next tick the runner closes the coroutine, runs `cleanup` (release all inputs, remove
  the scenario's bots, unwatch) and emits `mark lbl=stopped`. Repeating `stop` when idle answers "nothing running".
* **Ticket budget** (§6.12). Every scenario has `cost(args) → {t1, t2}`: tickets it will take from each team, from
  its planned spawns (`spawn` fixtures × `tcost.spawn`), kills and removals (× `tcost.kill`, `tcost.remove`) and the
  worst-case bleed (M-A: rule B, twice rule A). `tcost` defaults to 1 ticket per spawn, kill and removal (conservative)
  until P0.13 measures it (`meta.tcost`) [live:L16]. `!mh queue` prints, per item, the ETA and the forecast tickets of
  both teams after it (`T1 312 T2 288`); an item whose forecast drops a team below the guard (60) is **refused at
  queue time** with the reason and the suggestion to put `rr` in front of it. `!mh status` and every `prog` row carry
  the current tickets (`tk1`, `tk2`) and the forecast at the end of the running item (`fc1`, `fc2`); the chat warns
  once when a team is within 40 tickets of the guard.
* Ticket guard: before each config and every 10 s, if either team's `TicketManager:GetTicketCount` is below 60 the
  runner pauses the queue and says so (a round end would destroy the level and the run) [inferred].
* **Queue-time validation**: an item is refused (chat reason, nothing queued) if a site it needs is missing (`WA`:
  wall + aimcal; `SB`: flat; `WB:B`, `SA:B`: range; `WB:B`/`SA:B` also need P0.9 = YES), if its level is not the current
  one or reached by a `nextmap` before it, or if the ticket forecast fails. Nothing fails hours later for a reason
  known at queue time.
* `rr` (restart round) and `nextmap` queue items run without the interactive `confirm` (the user typed the queue). They
  send the admin command (§1), wait for `Level:Destroy` → `Level:Loaded` + 15 s, emit `fx act=admin`, re-bench the
  bots (§3.9) and continue. Sites are world coordinates on the same level and stay valid across `rr`; `nextmap`
  invalidates them (M-A needs none).
* Level change: `Level:Destroy` (shared) marks every bot soldier invalid and pauses; after `Level:Loaded` + 15 s the
  queue continues if its next item is level-independent or requires the new level (M-A on Bazaar) [live:L22]. L22 is
  tested early in Phase 0 by the round restart of P0.15 (§6.1): if the Lua state or the bot players do not
  survive a round restart, `rr`/`nextmap` are removed from the queue grammar for the night, the queue holds only what
  fits in one round's tickets, and the RUNBOOK says so.
* No human: `Player:Left` of the last human emits `mark lbl=human-left` and pauses the queue (whether the server even
  ticks without a human is unknown, R §9; the user stays connected) [live:L23]; a human joining resumes it.
* Resume: scenarios take `from=<config index>`; the `mark cfg-done` rows tell which configs finished.

### 3.9 Fixtures (`mh_bots`, the only writer)

| fixture | API (all server) | row |
|---|---|---|
| create | `PlayerManager:CreatePlayer(name, team, squad)` → `Player` or nil (`server/library/playermanager.md`) [doc] | `fx act=create id team` |
| equip | `ResourceManager:SearchForDataContainer(name)` (`shared/library/resourcemanager.md`) [doc] for the weapon unlock, kit and soldier blueprint; `player:SelectWeapon(slot, unlock, {})` with the weapon's own slot from `wa_cfg.lua` (primaries `WeaponSlot_0`, the M9 `WeaponSlot_1`, gadgets/grenades their slot, `fb/weaponslot.md`) [doc], `player:SelectUnlockAssets(customization, {})`, `player.selectedKit = kit` (writable) (`server/type/player.md`) [doc] [live:L07, L29, L31] | `fx act=equip id wid slot` |
| select slot | after spawn, if `weaponsComponent.currentWeaponSlot` (RO, `server/type/soldierweaponscomponent.md`) is not the weapon's slot: `EIASelectWeapon<n>` input for 1 tick (values 16-24, `fb/entryinputactionenum.md`), verified by `currentWeaponSlot` and `currentWeapon.name` within 2 s [live:L31] | `fx act=input arg={sel}` |
| fire mode | after equip and every respawn: read `currentWeapon.fireLogic` (`server/type/soldierweapon.md`; `FireLogicType` 0 single, 1 bolt, 2 auto, 3 burst, `fb/firelogictype.md`) [doc]; if a protocol needs auto and it is not 2: `EIACycleFireMode` (34) for 1 tick, wait 0.3 s, re-read, ≤ 4 presses; never assigned (forbidden_assign) [live:L30] | `fx act=input arg={cyc, fl}` |
| bench | a scenario's bots are spawned once and parked at a bench (their own HQ area, or 15 m behind the site, out of every flag and line of fire); moving a bot into or out of play is `place`, not a respawn, so it costs no ticket | `fx act=bench` |
| spawn / respawn | `player:CreateSoldier(blueprint, transform)` → `SoldierEntity` or nil, then `player:SpawnSoldierAt(soldier, transform, pose)` [doc] [live:L07] | `fx act=spawn id pos yaw pose` |
| place | `soldier:SetPosition(Vec3)` / `SetTransform(LinearTransform)` (`server/type/soldierentity.md`) [doc] [live:L09] | `fx act=place` |
| pose | `soldier:SetPose(pose, immediately, overridePending)` [doc] (`pose` itself is read-only), or the inputs `EIAChangePose` 13 / `EIAProne` 14 | `fx act=pose` |
| aim / input | own `EntryInput()` (`shared/type/entryinput.md`) [doc]: `deltaTime = dt` (every pass), `flags = EntryInputFlags.AuthoritativeAiming` (1) (`shared/type/entryinputflags.md`) [doc], `authoritativeAimingYaw/Pitch`, `SetLevel(EntryInputActionEnum.*, level)`; assigned to `player.input` (writable) [doc] each input pass [live:L08] | `fx act=input` at profile start/stop only (not per tick), with `dt` |
| kill / remove | `soldier:Kill()`; `PlayerManager:DeletePlayer(player)` [doc] [live:L24] | `fx act=kill/remove` |
| seat (S2/S3) | `player:EnterVehicle(vehicle, entryId)`, `ExitVehicle(force, exitOnEntry)` [doc]; entry from `ControllableEntity:GetDataForEntry` [doc] | `fx act=seat` |

* Aim convention (yaw zero direction, sign, radians vs degrees, pitch sign) is unknown [live:L08]. `!mh p0 aim`
  derives it **from a bot alone** (no human input needed): a bot on open ground is given yaw ∈ {0, 0.5, 1.0, 1.5} and
  pitch −0.1 in turn; the yaw convention comes from the soldier's `worldTransform` forward (RO) after each setting, the
  pitch convention from one shot per setting (the `shot` direction, else origin→hit on the ground). The candidate
  mapping (yaw = s·atan2(±x, ±z)·u + n·π/2 with u ∈ {rad, deg}, pitch = ±asin(y)·u) with the smallest error is kept and
  verified with five shots at the wall. Cross-check, logged only: the client sends its camera forward
  (`ClientUtils:GetCameraTransform`) with the local `AimingSimulation.yaw/pitch` (`client/type/aimingsimulation.md`,
  read) [doc]; the human's server-side `player.input` (`EntryInput | nil`) is read only if non-nil. The result goes
  into `meta.aimcal`, and every later bot aim uses it.
* Factions [inferred, L29]: the M16A3 is in the US Assault kit, the AK-74M in the RU one, the M416 in both (topics/01
  §1 kit table). Bots get the team whose kit lists the weapon (`wa_cfg.lua` `team` field), so no cross-faction unlock
  is needed; whether `SelectWeapon` would accept one is recorded by P0.6 but not relied on.
* Ammo: magazine refills by input (`EIAReload` 15, 1 tick) when the next protocol unit would not fit; a respawn when
  the reserve (`SoldierWeapon.secondaryAmmo` [inferred meaning]) is empty. Writing `primaryAmmo` is forbidden (R §3.2.3).
* Ownership: `mh_bots` keeps the set of `Player.id`s it created; every fixture call checks membership and refuses
  others (`err` row).

### 3.10 Samplers and hooks (observers)

| observer | source | row | rate |
|---|---|---|---|
| weapon sway | event `GunSway:Update(gunSway, weapon | nil, weaponFiring | nil, deltaTime)` (`shared/event/gunsway_update.md`) [doc]; match `weaponFiring.instanceId` (RO) against each watched soldier's `weaponsComponent.currentWeapon.weaponFiring.instanceId` (refreshed per tick) | `gs` `src=srv` | every update while watched; `nup` counts updates per tick [live:L11] |
| weapon sway, poll | PostSim read of each watched soldier's `weaponsComponent.currentWeapon.weaponFiring.gunSway` (`WeaponFiring.gunSway` RO, `GunSway | nil`, `shared/type/weaponfiring.md`) [doc]: the same field list. Runs always for the shooter of W-A/P0.7 (it also cross-checks the event cadence) and is the W2-W4 source if `GunSway:Update` never fires server-side for bot weapons (L11) | `gs` `src=poll` | once per tick while watched |
| firing state | watched weapons' `WeaponFiring` fields (`weaponState lastWeaponState nextWeaponState timeToWait recoilTimer recoilAngleX/Y/Z`) and `SoldierWeapon.fireLogic`, `weaponsComponent.currentWeaponSlot`, compared per tick | `wf` (`fl`, `slot`) | on change |
| health/kinematics | watched soldiers: `health`, `pose` (RO), `pendingPose`, `transform.trans`, `velocity` (inherited from PhysicsEntity) [doc] [live:L10], `isAlive`, `isManDown` | `hp` | per tick when watched, else 1 Hz for bots only |
| tickets | `TicketManager:GetTicketCount(team)` → float (`server/library/ticketmanager.md`) [doc]; iterator `ServerTicketCounterEntity` → `TicketCounterEntity(e)`: `team` (RO), `ticketCount`, `controlledAreaValue` (`server/type/ticketcounterentity.md`) [doc] [live:L15] | `tk` | per tick in M-A/M-B, else 1 Hz |
| flags | iterator `ServerCapturePointEntity` → `CapturePointEntity(e)` fields (`server/type/capturepointentity.md`) [doc]; per-team inside counts from `playersInside` (RO) | `cp` | per tick for the active flag(s), else on change + 1 Hz |
| shots | hook `EntityFactory:Create(hookCtx, entityData, transform)` (`server/hook/entityfactory_create.md`) [doc]: only if `entityData:Is(...)` names a projectile **data** type (`BulletEntityData`, `MissileEntityData`, `GrenadeEntityData`, all children of `MeshProjectileEntityData`, `fb/*.md`; inventory kind `dtype`; whether `Is('MeshProjectileEntityData')` also matches the children is L32, so the three names are tested explicitly), call `hookCtx:Call()` and read the created entity's `uniqueId`; attribute to the watched soldier whose eye is within 2 m of `transform.trans` | `shot` | event [live:L12] |
| hits | hook `BulletEntity:Collision(hookCtx, entity RO, hit, giverInfo RO)` (`server/hook/bulletentity_collision.md`) [doc]: read only, neither `Call` nor `Return` | `hit` | event [live:L13] |
| damage | hook `Soldier:Damage(hookCtx, soldier RO, info, giverInfo RO | nil)` (`server/hook/soldier_damage.md`) [doc]: `dmg ph=pre` from `info`, then `hookCtx:Call()`, then `dmg ph=post` with `soldier.health` (R §2). `pre` also carries `fxd`, the fixture distance (target `fx` position to the shooter's current position: a bot's, or the watched human's) and `spos` (shooter position), so `dist` = \|pos − org\| can be cross-checked (W5) | `dmg` | event [live:L14] |
| events | `Player:Killed/Respawn/Reload/SuppressedEnemy/EnteredCapturePoint/ExitedCapturePoint/Left`, `CapturePoint:Captured/Lost`, `Soldier:HealthAction/ManDown`, `Server:RoundOver/RoundReset`, `Level:Loaded`, `Level:Destroy` (`server/event/*.md`, `shared/event/level_destroy.md`) [doc] | `ev` | event |

`EntityFactory:Create` runs for every entity creation, including the level load. The handler tests the data type first
and returns at once for everything else (no `Call`), so non-projectiles pay one type check [inferred].

### 3.11 Client (`ext/client`)

* `mh:ctl` (server → client): `{run, sc, rec:{gs, fov}}` starts or stops client recording. Client rows only while
  recording.
* `mh_cgs`: `GunSway:Update` for the local player's current weapon; `src=pred` when the event's `weaponFiring`
  instance matches `predictedWeaponFiring`, `corr` for `correctedWeaponFiring` (both RO, `client/type/soldierweapon.md`)
  [doc]; on `fireShot` and every 4th frame otherwise (R §3.3).
* `mh_fov`: on `aimingSimulation.zoomLevel` change (`client/type/aimingsimulation.md`) [doc]: camera transform, project
  points ±5° about the camera's right and up axes at 100 m with `ClientUtils:WorldToScreen` (→ Vec2 or nil) and
  `GetWindowSize` (`client/library/clientutils.md`) [doc]; `fov = 2·atan(tan 5° · (size/2) / offset_px)`; the forward
  sign is the one whose projection lands at the screen centre (R §3.3) [live:L19].
* `mh_cam`: answers `mh:camreq` with `{pos, fwd}` of the camera (forward sign as above) for `!mh wall/range/ring/aim`.

---

## 4. Row schema v1

Machine-readable copy: `schema/rows_v1.json` (the extractor validates against it; `apicheck.py` checks that every
literal `row('<e>', {...})` in the Lua uses only declared keys; the mock e2e tests validate every row the mod prints).
Units: m, m/s, s, degrees unless the API unit is unknown; then the raw value is logged and named so (`da`, `rec`, `ra`,
`rr` stay raw until L11 fixes the unit).

**Envelope** (every row): `v` 1 · `run` · `sc` (`P0 WA SB WB SA MA MB WC WD S`, reserved `VA VB VC AA AB AC MC NA`) ·
`r` `S`/`C` · `k` int · `t` s · `ms` int · `q` int · `e` row type.

| `e` | payload (req; *opt*) | notes |
|---|---|---|
| `meta` | `hv` harness version, `lvl`, `mode`, `rnd`, `rpm`, `hz` (GetTickrate), `freq` (`vu.FrequencyMode`), `vars` {whitelisted name: value}, `mods` [names]; *`clk`* (`postsim`/`engine`), *`aimcal`* {yaw, pitch, sign, unit, lat, pass, dt}, *`args`*, *`build`* (the VU build running, from RCON `version`), *`wpn`* [wid] (sorted wids known to the Lua state, ≤ 64; names in the `dict kind=wpn` rows re-emitted after every meta row), *`nwpn`* (their count), *`argsx`* (true when `args` were left out to stay under 1000 B), *`tcost`* {spawn, kill, remove}, *`fmt`* (`9g`) | one at load (session run) and one at every run start, followed by one `dict kind=wpn` row per known wid |
| `census` | one of `types` {entity type name: count}, `api` {symbol: present}, `res` {asset name: found} per row, `part`, `of`; *`jsonv3 osdate vm guid_nodash cmd`* (`cmd` = the client console command's full name) | P0; chunked ≤ 40 keys per row |
| `dict` | `kind` (`wpn mat cp bot unlock`), `id`, `name`; *`x`* {team, kit, wid} | before first use of an id |
| `fx` | `act` (`create equip spawn place pose input kill remove seat respawn admin`), `id`, `ok`; *`pos yaw pitch pose team wid prof arg`* | every fixture |
| `mark` | `lbl`; *`cfg i n x k0 k1 own`* | `cfg-start`, `cfg-done`, `cfg-skip`, `timeout:*`, `stopped`, `aborted`, `stalled`, `human-left`; M-A `hold` marks carry the window `k0..k1` and the ownership vector `own` |
| `prompt` | `kind`, `n`, `of`; *`wid zone d id dir wait`* | every instruction to the user (U scenarios) |
| `site` | `name`, `pos`, `ok`; *`n dir d clear flat inc`* | `wall range flat ring` |
| `gs` | `id`, `wid`, `src` (`srv poll pred corr`), `da`, `dmin`, `rec` [p,y,r,ty], `disp` [p,y], `ra`, `rr`, `seed`, `ifs`, `fs`, `fir`, `tsls`, `mov`, `jmp`, `jt`, `pose`, `zoom`; *`lag` [p,y], `gsf vsf smf chf`, `nup`, `dt`, `fl`* | R §3.4 names kept |
| `wf` | `id`, `wid`, `ws`, `wsl`, `wsn`, `ttw`, `rt`, `ra` [x,y,z], `ammo`, `ammo2`; *`fl` (FireLogicType), `slot`* | `ws` = `WeaponState` value (`shared/type/weaponstate.md`: 0 Deploy … 9 PrimaryAutomaticFire, 11 Reload, 12 PostReload) |
| `shot` | `id`, `wid`, `kind` (`bullet missile grenade other`), `uid`, `pos`, `dir`; *`td`* (data type name) | `dir` = transform forward with the sign from L12 |
| `hit` | `uid`, `pos`, `n`, `bone`, `part`; *`mat` [fai, pmi, ppi], `body` (type name), `shooter`* | |
| `dmg` | `victim`, `ph` (`pre post`), `dmg`, `hp`; *`bone pos org dir bul exp cli cspd dot giver wid dtype dist fxd spos wfd`* | `dist` = \|pos − org\|; `fxd` fixture distance |
| `hp` | `id`, `hp`, `pose`, `pos`, `alive`; *`ppose vel md ias`* | `ias` = isAllowedToSpawn (humans, S-A.4 only) |
| `tk` | `tm` [[team, tf, tc, cav], ...] | |
| `cp` | `cp`, `team`, `loc`, `ctrl`; *`att vel atk n1 n2 cd pen1 pen2 mov pre en`* | |
| `fov` | `zoom`, `vfov`, `hfov`, `w`, `h`; *`fovmul sgn`* | client |
| `ev` | `name`; *`id id2 wid pos a`* (`a` = small object of numbers/bools/labels) | |
| `cmd` | `who`, `verb`, `ok`; *`args`* | |
| `prog` | `done`, `total`; *`cfg eta tk1 tk2 fc1 fc2`* | tickets now / forecast at the item's end |
| `p0` | `step`, `ok`; *`val`* {numbers/bools/labels} | Phase 0 results |
| `lim` | `drop` {e: n}; *`nf`, `over`* | |
| `err` | `where`, `msg`, `n` | |
| `trunc` | `of`, `len` | |
| `veh`, `vdmg` | reserved for Sessions 2-3 (R §3.4 fields) | schema present, no emitter yet |

Labels (`s` type) match `^[A-Za-z0-9_.:/+=#()\[\], -]{0,96}$`. Rows are < 1000 B.

---

## 5. Commands (`!mh ...`, also client console `mh ...`)

| command | effect | repeat-safe behaviour |
|---|---|---|
| `help [verb]` | short help | — |
| `status` | active scenario, state, config, progress, ETA, queue, bots alive, rows/s, drops, errors, the current wait and its timeout, tickets per team and the forecast at the end of the queue, the last `cfg-done` per scenario | — |
| `census` | `census` row + chat summary (API present n/N, assets found n/N, entity types, vm) | re-emits |
| `p0 <step>` / `p0 all` / `p0 report` | Phase 0 steps (§6.1) | each step re-runnable |
| `wall` / `range` / `flat` / `ring [radii]` | record a site from the caller's camera (needs the client) | overwrites the site, prints the new values |
| `sites` | list recorded sites | — |
| `run <SC> [k=v ...]` | start a scenario now (refused if one is running). Common args: `from=<i>`, `cfg=<i>`, `parts=a,c` (W-A protocol parts), `mode=U|B`, `w=<weapons>`; test-only `scale=<0..1>` (shrinks repetition counts; recorded in `meta.args`); refused with the reason if a needed site is missing, the scenario is not certified (§14) or its ticket forecast fails (§3.8) | running: "already running X" |
| `queue <item> ...` / `queue` / `queue clear` / `queue resume` | append items `SC[:preset]` (`SB`, `MB`, `MA`, `WA:tier1`, `WA:rest` (tiers 2-3), `WA:all`, `WA:60`, `WB:B`, `SA:B`, `rr`, `nextmap`; a resume form `WA:all:from=<i>` for crash recovery); each item is validated at queue time (§3.8: sites, level, P0 results, ticket forecast); one invalid item refuses the whole line, with the reason; show with ETA and ticket forecast per item; clear; resume after a guard pause | duplicates allowed, shown with indices |
| `stop` | stop the active scenario (cleanup) and pause the queue | idle: "nothing running" |
| `pause` / `resume` | hold / continue the active scenario at its next wait | idempotent |
| `next` | skip the current config (`cfg-skip`) | idle: no-op |
| `mark <label>` | a `mark` row (user annotations: "ads", "wrong weapon") | — |
| `ok` | confirms a prompt in user-shooter scenarios | ignored when no prompt |
| `target <d>` / `target here` / `untarget` | spawn / remove a target bot (W-B U-mode, W-C) | replaces the previous target |
| `clear` | remove every harness bot | idempotent |
| `watch me` / `unwatch me` | per-tick `hp`/`gs` for the caller | idempotent |
| `rate <gsEvery>` | gs decimation (1 = every update) if Phase 0 shows print pressure | — |
| `nextmap` | `RCON:SendCommand('mapList.runNextRound')` (`server/library/rcon.md`) [doc]; `fx act=admin` | asks for `nextmap confirm` |
| `restartround` | `RCON:SendCommand('mapList.restartRound')`; `fx act=admin` | asks for confirm |

`nextmap` and `restartround` change the round, not game data or measured behaviour; they are logged as admin fixtures
(the accepted deviation of §1). As queue items (`nextmap`, `rr`) they need no `confirm`.

An unknown or invalid item refuses the **whole** `queue` line (nothing is queued), so the order the user typed is never
silently changed; the reply names the item and the reason (`MA refused: forecast T2 41 < 60, put rr before it`).

---

## 6. Scenarios as state machines

Notation: `STATE (duration) → next` with `[timeout → action]`. "Who": **B** bots only (client stays connected, R §4.4),
**U** the user acts. Durations are 30 Hz wall-clock estimates [inferred]; `estimate(args)` prints the same numbers in
chat before a run starts. Every U step emits a `prompt` row and a chat line; every phase change a `mark`.

### 6.1 P0 Phase 0 shakedown (U, 30-45 min, run id `...-P0-...`)

Step ids are stable (other sections cite them); the **run order** is P0.1, P0.16, P0.2, P0.13, P0.15, P0.3, P0.4,
P0.5, P0.6, P0.7 (+ P0.8, P0.11 from its data), P0.9, P0.10, P0.12, P0.14. The round and level checks come right
after the census so that the expected `-DisablePreRound` restart (L21) and the round-restart test of L22 happen before
any site is recorded.

| step | command | what it does | pass criterion | fallback |
|---|---|---|---|---|
| P0.1 load | (automatic) | session `meta` (with `build`); greeting in chat on first spawn, naming the client console command's full name; a plain line with the join GUID without dashes (`RCON:GetServerGuid`) | user sees `[mh] mharness <ver> ready` | check the server log for `[mharness]` lines; no log file in `ServerMeasure\logs` → relaunch with `-debuglog` (`content\general\args.md`); L01, L18, L28, L33, L35 |
| P0.16 deploy chat | on the deploy screen (before spawning): `!mh help` | chat and replies work from the deploy screen (the overnight state) | reply arrives | the user spawns and stays in a corner instead; console `mh`/`mharness.mh`; L06, L34 |
| P0.2 census | `!mh census` | API presence for every inventory symbol used (each probe in `pcall`), entity type counts (`TraverseAllEntities`), asset names resolved (`wa_cfg` weapons, kits, `Characters/Soldiers/MpSoldier`), `json.encode` sample, `os.date`, `vm`; chunked rows | all P0-required symbols present; ≥ 1 capture point and ticket counter; asset misses listed | missing asset: alternate names in `wa_cfg.lua`; missing API: the dependent scenario is disabled (status shows it); L02, L05, L07, L15, L27 |
| P0.13 round | `!mh p0 round` | spawns 3 bots (team 1, 2, 2; with the user 4 players, the round-start count) on their benches; waits ≤ 60 s for `isInPreRound` false on every flag; then measures the ticket cost of one bot spawn, one `Kill` and one `DeletePlayer` (tk deltas, 5 s apart) → `meta.tcost` | round started; `tcost` measured | still preround → expected outcome if bots do not count: stop the server, `setup-instance.ps1 -DisablePreRound`, restart, redo P0.1-P0.13 (RUNBOOK); L21, L16 |
| P0.15 level | `!mh p0 level confirm` | sends `mapList.restartRound` (`fx act=admin`); after `Level:Loaded` + 15 s checks that a mod counter, the id table and the bot players survived, and re-benches the bots | state kept | state lost → no `rr`/`nextmap` in tonight's queue (§3.8); L22 |
| P0.3 tick | `!mh p0 tick` | 10 s: PostSim count vs `GetTickrate()` and wall ms; `vu.FrequencyMode` | 300 ± 3 at 30 Hz | Engine:Update clock (§3.2); L03, L04 |
| P0.4 wall | `!mh wall` | client camera → raycast (`RaycastManager:DetailedRaycast(from, to, maxHits, materialFlags, flags)` → `RayCastHit{}`) → point, normal, distance, incidence; 7 × 7 grid ±1.5 m coplanar within 2 cm; floor clear at 10/12/25 m along the normal | `site wall ok` | pick another wall; L17, L19 |
| P0.5 aim | `!mh p0 aim` | derive the yaw/pitch convention from a bot alone (§3.9); human camera / `AimingSimulation` cross-check logged | error < 0.5° | apply the input in PostSim instead of PreSim and repeat; then aim via `EIAYaw/EIAPitch` closed loop on the body forward (W-A aims by turning); L08 |
| P0.6 bots | `!mh p0 bots` | 2 bots (team 1, 2) 5 and 10 m in front; poses stand/crouch/prone 2 s each; SetPosition 2 m; throttle 1 s (velocity non-zero; `deltaTime` set); a cross-faction `SelectWeapon` probe (logged only); slot check (`currentWeaponSlot`), M9 slot switch by `EIASelectWeapon2`; delete | all `fx ok`; pose rows match; `vel` > 1 m/s while moving; slot switch seen | no movement → repeat with the input written in PostSim; L07/L08/L09/L10/L24/L29/L31 per failing part |
| P0.7 fire | `!mh p0 fire` | shooter bot at the wall site (stand, hip): reads `fireLogic`, cycles to auto if needed (`EIACycleFireMode`); fires 5 single shots, then a 5-round burst (FIRE_N timeout 2 s) | 10 `shot` rows, ≥ 9 `hit` rows on the wall plane, `fl` = 2 (auto) at the burst, `gs` with `fs` once per shot, `nup` = 1 per tick, `src=srv` and `src=poll` agree | input pass variant, EIAFire level 1 for 2 ticks; no `src=srv` rows → W2-W4 use `src=poll` (L11); fire mode stuck → W-A runs protocols a and c only and the chat says so (L30); L08/L11/L12/L13 |
| P0.8 geometry | (from P0.7 data) | \|hit − ray∩plane\| from the shot pos/dir (drop-corrected); angle(shot dir, origin→hit) | median ≤ 1 cm and ≤ 0.05° (drop over the first tick allowed; §16) → "spawn dir is post-dispersion", "hit pos exact" | analysis uses origin→hit only (W1 does not need the spawn dir) |
| P0.9 damage | `!mh p0 damage` | bot (team 1, M16A3) shoots a team-2 bot at 10 m, chest, 5 shots | three outcomes: ≥ 3 `dmg` pairs with `post.hp < pre.hp` → **bot damage YES**; `hit` rows on the target (soldier body or bone ≥ 0) but no `dmg` → **NO (arbitration)**, L14 answered; no hit on the target → **NO (aim)**: L14 unanswered, repeat after `!mh p0 aim` | NO of either kind → W-B, S-A, W-C use the user as shooter (U mode) |
| P0.10 human shot | user fires 3 rounds at the wall, then `!mh p0 human` | server `shot`/`hit` rows for the human? | rows present → human shots observable server-side | W-D relies on client rows only; L25 |
| P0.11 cadence/units | (from P0.7) | `nup`, `da` vs data min (M16A3 hip 2.5, ADS 0.2) under both readings (half-angle, full cone angle), `rec` scale, ranges of `ra`, `rr`, `seed` | `da` equals the data min within 1 % → degrees (and which reading) | keep raw; W1 primary uses impacts only; W2/W3 use unit-free ratios (§10) (R §4.6) |
| P0.12 log | user runs `extract.py --dry-run` on the live logs | reads the open log, counts rows, `q` gaps, malformed, `trunc` | 0 malformed, 0 gaps, 0 trunc; a 950 B test row intact | lower `rate`; extract after the server stops; L20 |
| P0.14 report | `!mh p0 report` | one-line verdict per step in chat + `p0` rows, including the queue grammar for tonight (`rr` allowed or not, bot damage, gs source) | — | — |

### 6.2 W-A fixed wall (B; R §3.5 #1; items W1-W4, T1)

Requires `site wall` (P0.4) and the aim calibration (P0.5). Shooter: one bot (the weapon's faction team), stance/zoom
per config, at `wall.pos + wall.n · d` ground-snapped, facing the wall, aiming at the wall point; d = 10 m hip, 25 m ADS
(12 m ADS for protocol d, so the climb stays on the wall) [inferred: radii 0.44-1.23 m hip, 0.09-1.3 m ADS].

Per config:
```
SETUP      spawn shooter, equip, pose, zoom held 1.0 s                         [10 s timeout → cfg-skip]
SETTLE     until da stable at dmin for 10 ticks (≤ 5 s)                         [timeout → note, continue]
A_SINGLE   i=1..N_a: FIRE1, REST 1.5 s                                          (pure minimum disc, first-shot recoil)
B_BURST    i=1..N_b: FIRE_N(5), REST 1.5 s                                      (growth, recoil)
C_PAIRS    for g in G, j=1..N_c: FIRE1, WAIT g after the shot, FIRE1 (g=0.075: one 2-shot auto hold), REST 2.5 s
D_MAG      i=1..N_d: FIRE_N(magazine), RELOAD, REST 3 s                         (saturation, horizontal autocorrelation)
E_RECOV    i=1..N_e: FIRE_N(10 | 20), REST 4 s with per-tick gs                 (recovery curves from saturation)
DONE       mark cfg-done, remove shooter
sub-states: FIRE1 = hold EIAFire until 1 attributed shot or 3 ticks; FIRE_N = hold until N shots or the magazine is
empty, with its own timeout N·60/rpm + 1 s (→ mark timeout:fire, release, count as a skip unit); before every unit
that needs auto (B, C at g=0.075, D, E) `fl` must be 2, else FIREMODE (cycle, ≤ 4 presses; still not auto → those
parts are skipped for this config, `mark lbl=no-auto`, A and C still run); RELOAD before a unit that does not fit the
magazine (EIAReload 1 tick, wait until ammo = full ≤ 6 s); RESPAWN when the reserve is empty (counted in the ticket
forecast); PAUSE while the user is within 1 m of the line of fire ("step aside").
Shot attribution: gs.fs, else shot rows, else hit rows (whichever L11/L12/L13 left working).
```
Protocol sizes: **full** N_a 200, N_b 100, G = {0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5} × N_c 20, N_d 10,
N_e 15 + 15 (≈ 25 min per auto config). **short** (non-reference weapons) N_a 100, N_b 50, N_c 10, N_d 5, N_e 10
(≈ 13 min). **bolt** (M98B) A_SINGLE 200 + C_PAIRS with G = {1.5, 2, 3} × 20 (≈ 12 min). M98B stand-hip is dropped
(R §3.5 lists it; hip spread on a bolt sniper adds nothing to W1 and costs 12 min) [inferred]. A config whose `dmin`
is 0 (an ADS minimum of 0) has no disc to test: W1 reports "no spread" for it, W3/W4 still use it. If L11 shows
`gs.rec` unusable, C_PAIRS runs as triples (third shot at a fixed 0.1 s) so the impact of shot 3 measures shot 2's
kick (§10 W4). W-A respawns only to refill an empty reserve: ≈ 7 per full config, ≈ 4 per short one (in the forecast).

Config list (`scenarios/wa_cfg.lua`), queue order: tier 1 = M16A3 stand-ADS, stand-hip (full); tier 2 = M16A3
prone-ADS, crouch-ADS, crouch-hip, prone-hip (full); tier 3 = AK-74M stand-ADS, stand-hip, prone-ADS; M416 stand-ADS,
stand-hip, prone-ADS; M249 stand-ADS, stand-hip, prone-ADS (short); M98B stand-ADS, prone-ADS (bolt). Estimates:
tier 1 ≈ 50 min, tier 1+2 ≈ 2.5 h, all ≈ 5.1 h. Commands: `!mh run WA tier=1` / `tier=2` / `tier=all` / `cfg=<i>` /
`from=<i>` / `parts=a,c`; `!mh queue WA:tier1` (configs 1-2), `WA:rest` (configs 3-17), `WA:all`. Every config in
`wa_cfg.lua` names its weapon's slot and the fire mode it needs. The M98B ADS configs are flagged `sway`: a scoped
bolt-action rifle may have scope sway and a 0 minimum spread, so W1 reports them as "no spread" or
"sway-contaminated", never as a disc verdict (§10).

Unlock names (resolved and reported by P0.2): `Weapons/M16A4/U_M16A4` (M16A3), `Weapons/AK74M/U_AK74M`,
`Weapons/M416/U_M416` [inferred path], `Weapons/M249/U_M249`, `Weapons/Model98B/U_M98B`, `Weapons/M9/U_M9`,
`Weapons/Remington870/U_870`, `Weapons/RPG7/U_RPG7`, `Weapons/SMAW/U_SMAW`, `Weapons/Gadgets/M320/U_M320_HE`,
`Weapons/M67/U_M67` (lower-case forms in our dumps; VU's name lookup case rules [live:L07]).

### 6.3 W-A60 (B; T1)

The same machine on a `-high60` server (run id hz 60): M16A3 stand-hip and stand-ADS with protocol **short** plus the
full gap set G (≈ 25 min). `!mh run WA tier=60`.

### 6.4 S-B movement (B; S3, W2 minimum rise, stance transition, T1)

Requires `site flat` (start P, direction u, ≥ 30 m clear, ±5 cm flat; upward clearance measured for drops). The bot
returns to P facing u before each profile (`place`).
```
ACCEL      for pose in stand, crouch, prone; 3 reps: throttle 1 for 3 s, 0 for 3 s
SPRINT     3 reps: throttle 1 + sprint 1 for 4 s, 0 for 3 s
STRAFE     3 reps: strafe 1 for 3 s, 0 for 3 s;  BACK 3 reps: throttle -1 for 3 s, 0 for 3 s
JUMP       5 reps: jump 1 tick, wait 2.5 s
DROP       h in {5, 8, 10, 12, 15, 20, 30, 50} (≤ clearance − 2 m): place at P + (0,h,0), record until 2 s after
           landing, respawn                                                   [8 s timeout → mark, respawn]
POSE       idle: stand→crouch→prone→crouch→stand→prone→stand by pose inputs, 3 s holds; then the same while firing
           single shots every 0.3 s (gs watched)
MINRISE    hip and ADS, 3 reps: idle 2 s, throttle 1 for 3 s, 0 for 3 s (gs watched)
```
`hp` and `gs` per tick throughout. ≈ 12 min. `!mh run SB` (the same on a 60 Hz server gives `SB` with hz 60).

### 6.5 W-B falloff (U, or B if P0.9 says bot damage works; W5, W6)

Requires `site range` (origin O, horizontal direction u, clear length L by raycast). Distances D = {5, 8, 10, 15, 20,
25, 30, 40, 50, 60, 75, 100, 125, 150, 175} ∩ (0, L − 2]; skipped distances are marked. In U mode the user is watched
(per-tick `hp` with position), so every `dmg` row carries the shooter position `spos` and the fixture distance `fxd`.
```
WEAPON     for w in args (default M16A3, M98B; optional AK-74M, M9, 870MCS):
           B: spawn shooter S at O (stand, ADS, w, its slot);  U: prompt "equip w (team <US|RU>), stand on the mark,
           !mh ok" and wait for the human's current weapon name to match (or `ok`)       [300 s → skip weapon]
           spawn target T once (team opposite the shooter) on the bench
PASS1      zone chest, for d in D (870MCS: 5-30 only): place T at O + u·d facing O; wait 1 s;
           until 3 hits or 30 shots (B) / the user's 3 hits (U):
             B: aim at T's chest point (feet + 1.35 m [inferred], refined after the first hit by bone), FIRE1,
                wait 0.6 s;  U: prompt "chest at d m (n/3)"                               [120 s → skip distance]
             after each dmg pair for T, HEAL: if T died → respawn T (a ticket); else if hp ≤ 1.1 × the next expected
             damage (data falloff at d, zone multiplier; the pre-damage value does not depend on health) → B: wait for
             regeneration to 100 (≤ 30 s; `heal=regen`, the B default, costs no ticket) / U: respawn T (`heal=respawn`,
             the U default; `heal=regen` selectable); else continue at once
PASS2      zone head: the same loop (B: always; U: only with `zones=chest,head`, an optional second run)
PASS       d = 20: T1 at 20 m, T2 at 21 m in line; 5 chest shots at T1; record both
PEN        U only: `!mh target here` behind cover (wood, glass); user shoots through; 5 hits
```
`dmg` pre/post, `shot`, `hit` per shot. Timings: U chest-only ≈ 15 min per weapon (M16A3 + M98B ≈ 30 min; the M98B
needs a respawn after almost every hit), the head pass ≈ +10 min per weapon; B ≈ 10 min per weapon with regen waits.
Tickets: M16A3 ≈ 15 target respawns (U) / ≈ 0 (B, regen), M98B ≈ 45 (it kills at short range in either mode). If bot
damage is YES, at least the M16A3 runs in both modes (U attended, B overnight) so that W5 can show that the server- and
client-arbitrated falloff agree. The M98B end knot needs clear range to ≥ 160 m: with L − 2 < 160 the W5 M98B knot
verdict is `inconclusive` (§10). `!mh run WB mode=U w=M16A3,M98B [zones=chest,head] [heal=regen|respawn]` / `mode=B`.

### 6.6 S-A health and regeneration (B or U; S1, S2, S4)

```
REGEN      5 reps: T placed at 8 m (spawned once), one chest hit (≈ 25 dmg) by S (B) or the user (U); hold 40 s
           (hp per tick); regeneration restores T, so no respawn (respawn only if hp < 100 at the end)
SUPP       3 reps: as REGEN, plus at +4 s a spray 2 m beside T for 2 s: B: S with EIAFire held and an aim offset;
           U: prompt "spray a burst about 2 m to the side of the target now, do not hit it" (one magazine);
           evidence of suppression per rep: Player:SuppressedEnemy ev for T, or a change of T's gs `smf` (T's weapon
           is watched); a rep without evidence is marked `supp-none`
IMMUNE     4 delays {0.5, 1.0, 1.5, 2.5 s} × 3: respawn T, one shot at the delay (B); U: prompt "keep firing at the
           target's spawn point for 60 s while it respawns every 6 s"; the time since spawn is in each dmg row
RESPAWN    U only, optional: the user suicides from the menu; ias per tick until it flips
```
≈ 9 min B, ≈ 12 min U. Tickets: ≈ 12 target respawns (IMMUNE) in either mode. `!mh run SA mode=B|U`.

### 6.7 M-A ticket bleed (B; M1)

Levels `MP_Subway ConquestSmall0` and `MP_001 ConquestLarge0` (R §3.5 #6). Fully automatic: flags from the iterator;
HQs = capture points with capture disabled and a team (`isCaptureEnabled` false). M-A is the last item on each level
because it bleeds; the RUNBOOK queue puts `rr` in front of it.
```
INIT       find flags and HQs; refuse if any flag isInPreRound (L21); spawn a pool of 2 bots per team once, on the
           team's bench (its HQ area)
PLAN       Metro CQS: 0-0, 1-0, 2-0, 3-0, 2-1, 1-1;  Bazaar CQL: 0-0, 1-0, 2-0, 3-0, 4-0, 5-0, 4-1, 3-2, 2-2
           (the plan starts from the observed state; trimmed to the level's flag count; the end state does not
           bleed under either rule). Adaptive mirroring: before a state, if the worst-case bleed of that state would
           take the bleeding team below the guard, the state is mirrored (team roles swapped: the rule is a function
           of the ownership vector, so a mirrored state tests the same thing) or, if both teams are low, the plan ends
for each state:
  CHANGE   per flag: CAPTURE(team, the 2 pool bots placed at the flag, until team & controlled; then benched)
           [90 s timeout → retry with offsets ±3 m, then skip state]; NEUTRALISE (the enemy pool until team = 0, then
           benched at once). Flags keep their owner when empty, so the pool moves from flag to flag
  HOLD     150 s: tk per tick, cp 1 Hz; `mark lbl=hold k0 k1 own` (only these windows enter the rate fit)
END        remove all bots
```
Ticket guard (§3.8). ≈ 22 min Metro, ≈ 30 min Bazaar. Tickets: 4 spawns + the bleed (Metro rule A ≈ 112 for team 2,
rule B up to ≈ 225; the forecast uses the worst case). `!mh run MA`.

### 6.8 M-B capture (B; M2)

One flag F (`flag=<cp id>`, default: the capturable flag farthest from both HQs). A pool of 5 team-1 and 4 team-2
bots is spawned once (9 spawns); "remove" below means "bench" (a `place`, no ticket).
```
RESET      make F neutral (4 bots of the owner's enemy neutralise; bench them)
N_SERIES   n in {1,2,3,4,5} × 3: n team-1 bots placed at F in one tick; cp per tick until team 1 controlled [60 s];
           bench; RESET
FLIP       F to team 2 (4 bots); n in {1,2,4} × 3: n team-1 bots flip F [90 s]; RESET to team 2
CONTEST    1v1 and 2v1 inside F owned by team 2, 60 s each
LEAVE      3 reps: 1 team-1 bot on neutral F; at loc ≥ 50 % bench it; record 40 s
END        RESET to neutral; remove all bots
```
T(n) is measured from the tick the n-th attacker is inside (its `Player:EnteredCapturePoint` ev, or the `cp` inside
count reaching n) to `controlled`. ≈ 25 min. Tickets: 9 spawns. `!mh run MB [flag=<id>]`.

### 6.9 W-C explosives (U; W7)

`!mh target 30` for RPG-7 and SMAW direct hits × 3 each; M320 HE direct × 3 and offset (1-3 m beside, offset from the
detonation position); M67: `!mh ring 0.5,1,2,3,4,5,6,7` places 8 targets around the caller's aim point at those radii
(different azimuths); the user throws at the centre; × 3 (between throws only killed targets are respawned, the others
regenerate during the 20 s reset). The detonation point is the last per-tick position of the tracked grenade/missile
(`shot kind`), or an explosion entity creation (`ExplosionEntityData`) if L12 shows one. ≈ 15 min. Tickets ≈ 25-40
target respawns. The user's weapons are gadgets/grenades in their own slots: nothing to configure for bots tonight
(a future bot W-C uses the slot field and `EIASelectWeapon*`, §3.9). `!mh run WC`.

### 6.10 W-D client (U; W8, W9, W10)

`!mh run WD` sends `mh:ctl rec={gs,fov}` to the user's client. The user: 3 full-auto M16A3 magazines at the wall,
reloads at 0 / 50 / 90 % magazine, ADS with every optic they have on the M16A3 and the M98B scope. Server `shot` (L25)
and `wf` rows for the human plus client `gs` (pred/corr) and `fov` rows. ≈ 15 min.

### 6.11 Extension points (Sessions 2-3)

`scenarios/registry.lua` holds `VA VB VC AA AB AC MC NA` with `enabled=false`. Shared pieces already in the design:
input profiles `{t0, t1, action, level}` (S-B and later V-A), `mh_bots.seat`, the `veh`/`vdmg` schema rows,
part-transform sampling (`PhysicsEntityBase:GetPartTransform`, `shared/type/physicsentitybase.md`) [doc], `MapList`
profiles `s2`/`s3` in `setup-instance.ps1`, and mock hooks for vehicles (`vu_world.lua` keeps an entity list any
type can join; a `vu_vehicles.lua` module slots in like `vu_soldier.lua`).

### 6.12 Tonight's order, timings and ticket budget

| block | items | who | time |
|---|---|---|---|
| setup | checks, manifest before, setup-instance, launch, join | U | 15 min |
| Phase 0 | P0.1-P0.16 in the run order of §6.1 (the round restart of P0.15 refills both teams) | U | 30-45 min (+10 min if the `-DisablePreRound` restart is needed) |
| attended | W-B chest pass (M16A3, M98B first; head pass, AK-74M, M9, 870 if time), S-A, W-D, W-C | U | 75-90 min (B mode: W-B and S-A move to the queue) |
| prepare queue | `!mh flat`, `!mh wall` (and `!mh range` if `WB:B`/`SA:B` are queued) on Metro; `!mh queue rr WA:tier1 [WB:B SA:B] SB MB WA:rest rr MA nextmap MA` | U | 5 min |
| overnight (Metro, then Bazaar) | rr 2, W-A tier 1 50, [W-B B 20, S-A B 9], S-B 12, M-B 25, W-A rest ≈ 4.25 h, rr 2, M-A 22, nextmap 3, M-A Bazaar 30 | B | ≈ 6.7 h (≈ 7.2 h with WB:B SA:B) |
| morning | restart with `-high60` (after `!mh stop` and quitting cleanly), `!mh wall`, `!mh flat`, `!mh queue WA:60 SB` | U+B | 5 min + 40 min |
| close | stop server, manifest after, compare, extract | U | 10 min |

W-A tier 1 is first in the overnight queue: it carries W1-W4, the most valuable items, and must not depend on anything
that could stall before it.

**Ticket budget** (Metro ConquestSmall0 starts with 350 per team, P §7.3; guard 60, so 290 usable per round). Counts are
bot spawns (s) and deaths (d) per team; the forecast uses `tcost` from P0.13, defaulting to 1 per spawn and 1 per
death or removal (conservative; BF3 charges a ticket per spawn, so the likely cost is the s column).

| span | item | team 1 (s / d) | team 2 (s / d) |
|---|---|---|---|
| after P0.15 `rr` | P0.3-P0.12 | 4 / 4 | 4 / 4 |
| attended | W-B U M16A3 + M98B (chest) | — | 62 / 60 |
| | S-A U | — | 13 / 12 |
| | W-C | — | 40 / 35 |
| | optional W-B AK-74M (user on RU, target team 1), M9, 870 | 16 / 15 | 35 / 33 |
| | **sum** (without / with optional) | 4 / 4 → 20 / 19 | 119 / 111 → 154 / 144 |
| after queue `rr` | W-A tier 1 (shooter team 1) | 15 / 14 | — |
| | [W-B B (regen heal), S-A B] | — | [60 / 57] |
| | S-B (drops kill at 20-50 m) | 10 / 9 | — |
| | M-B (pool) | 5 / 5 | 4 / 4 |
| | W-A rest (M16A3 tier 2, M416, M249, M98B on team 1; AK-74M on team 2) | 62 / 58 | 15 / 14 |
| | **sum** | 92 / 86 | 19 / 18 (79 / 75 with WB:B SA:B) |
| after the second `rr` | M-A Metro (pool + bleed, adaptive mirroring) | 2 + bleed | 2 + bleed (A ≈ 112, B ≤ 225) |
| after `nextmap` | M-A Bazaar (initial tickets read live; plan trimmed and mirrored to fit) | 2 + bleed | 2 + bleed |

Every span fits 290 under the likely cost; the attended block with all optional weapons does not fit under the
conservative cost (154 + 144 = 298): if P0.13 measures a ticket per death too, `!mh run` refuses the last optional
weapon with the forecast, and `!mh restartround confirm` (sites stay valid) clears it. `!mh run` applies the same
forecast check as `!mh queue`.

---

## 7. API facts used (source and confirmation status)

All 308 inventory entries (279 in pass 2, 29 added in pass 3) were confirmed today against VU-Docs `f3cb9f19` by
`py/mhlib/vudocs.py` (name/signature on the page, enum values equal, read-only flags recorded, data types' parents). "doc" below means that. Behaviour
beyond the name and signature is a live check.

| API | realm | source page | status | live |
|---|---|---|---|---|
| Lua 5.4, library whitelist, `string.starts/ends`, `json.encode/decode` | all | `content\vext\_index.md` | doc | L02 |
| `require`, `__shared/` prefix, realm-relative paths, once-only | all | `content\vext\guides\code-splitting.md` | doc | L27 (VM sharing: undocumented) |
| `Events:Subscribe(name, fn)` (+ context overload) | shared | `shared/library/events.md` | doc | — |
| `Hooks:Install(name, priority, fn(hookCtx, ...))`; higher priority runs first; `HookContext:Call()` runs the original and stops lower-priority handlers; no `Pass`/`Return` | shared | `shared/library/hooks.md`; `shared/type/hookcontext.md`; `content\vext\guides\hooks.md` | doc | — |
| `UpdateManager:Update(deltaTime, updatePass)`; `UpdatePass_PreSim 0`, `PostSim 1` | shared | `shared/event/updatemanager_update.md`; `fb/updatepass.md` | doc | L03, L08 |
| `Engine:Update(deltaTime, simulationDeltaTime)` | shared | `shared/event/engine_update.md` | doc | L03 |
| `SharedUtils:GetTimeMS() int`, `GetTickrate() float`, `GetLevelName`, `GetCurrentGameMode`, `IsServerModule` | shared | `shared/library/sharedutils.md` | doc | L03, L04 |
| `GunSway:Update` / `GunSway:UpdateRecoil(gunSway, weapon | nil, weaponFiring | nil, deltaTime)` | shared | `shared/event/gunsway_update.md`, `gunsway_updaterecoil.md` | doc | L11 |
| `GunSway` fields (`dispersionAngle minDispersionAngle currentRecoilDeviation currentDispersionDeviation currentLagDeviation cameraRecoilTransform timeSinceLastShot randomAngle randomRadius jumpingTimer` float/struct; `seed initialFireShot` int; `fireShot isFiring isJumping isMoving` bool; `isSupportedShooting` RO) | shared | `shared/type/gunsway.md` | doc (types only) | L11 (units, cadence) |
| `GunSwayDeviation {pitch, yaw, roll, transY}` | shared | `shared/type/gunswaydeviation.md` | doc | L11 |
| `WeaponFiring` (`gunSway` RO, `instanceId` RO, `weaponState lastWeaponState nextWeaponState timeToWait recoilTimer recoilAngleX/Y/Z`) | shared | `shared/type/weaponfiring.md`; `shared/type/weaponstate.md` | doc | — |
| `EntityManager:GetIterator(entityType) → EntityIterator`, `TraverseAllEntities(fn)`; `EntityIterator:Next() → Entity | nil` | shared | `shared/library/entitymanager.md`; `shared/type/entityiterator.md` | doc | L15 |
| `entity:Is(typeName)`, casts `CapturePointEntity(e)` / `SoldierEntity(e)` / `TicketCounterEntity(e)`; inheritance SoldierEntity → ControllableEntity → PhysicsEntity → GameEntity → SpatialEntity → Entity → EntityBusPeer | shared/server | `shared/type/entitybuspeer.md`; type pages' "Inherits from" | doc | L15 |
| `Entity.uniqueId` RO, `EntityBusPeer.typeInfo` RO, `TypeInformation.name` RO | shared | `shared/type/entity.md`, `entitybuspeer.md`, `typeinformation.md` | doc | — |
| `ResourceManager:SearchForDataContainer(name) → DataContainer | nil` (read only) | shared | `shared/library/resourcemanager.md` | doc | L07 |
| `Vec3(x,y,z)`, `LinearTransform(left, up, forward, trans)`, fields `trans forward up left` | shared | `shared/type/vec3.md`, `lineartransform.md` | doc | — |
| `EntryInput()`, `deltaTime`, `authoritativeAimingYaw/Pitch`, `flags`, `zoomLevel` (all writable), `GetLevel/SetLevel(action, level)`; `EntryInputFlags.AuthoritativeAiming 1`; `EntryInputActionEnum` (`EIAThrottle 0 EIAStrafe 1 EIABrake 2 EIAYaw 4 EIAPitch 5 EIAFire 7 EIAZoom 11 EIAJump 12 EIAChangePose 13 EIAProne 14 EIAReload 15 EIASelectWeapon1-9 16-24 EIASprint 32 EIACycleFireMode 34`) | shared | `shared/type/entryinput.md`, `entryinputflags.md`; `fb/entryinputactionenum.md` | doc | L08 |
| `CharacterPoseType_Stand 0 / Crouch 1 / Prone 2`; `WeaponSlot_0`-`WeaponSlot_7` (0-7); `TeamNeutral 0 Team1 1 Team2 2`; `SquadNone 0 Squad1 1`; `FireLogicType` (`fltSingleFire 0 fltSingleFireWithBoltAction 1 fltAutomaticFire 2 fltBurstFire 3 fltHoldAndRelease 4 fltDetonatedFiring 5`) | shared | `fb/characterposetype.md`, `weaponslot.md`, `teamid.md`, `squadid.md`, `firelogictype.md` | doc | L30, L31 |
| `RayCastFlags` (`CheckDetailMesh 1 DontCheckWater 4 DontCheckRagdoll 16 DontCheckCharacter 32`); `RayCastHit {position normal material part bone rigidBody}` all RO; `material` is `DataContainer | nil` | shared | `shared/type/raycastflags.md`, `raycasthit.md` | doc | L13, L17 |
| `MaterialContainerPair(dc)` cast; `{flagsAndIndex, physicsMaterialIndex, physicsPropertyIndex}` | shared | `fb/materialcontainerpair.md` | doc | L13 |
| `Level:Loaded(levelName, gameMode, round, roundsPerMap)` (server; the client version has 2 args); `Level:Destroy()` | server / shared | `server/event/level_loaded.md`; `shared/event/level_destroy.md` | doc | L22 |
| `Player:Chat(player | nil, recipientMask, message)` | server | `server/event/player_chat.md` | doc | L06 |
| `ChatManager:SendMessage(message, toPlayer) → bool`, `Yell(message, duration, toPlayer)` | server | `server/library/chatmanager.md` | doc | L06 |
| `PlayerManager:CreatePlayer(name, team, squad) → Player | nil`, `DeletePlayer`, `GetPlayers`, `GetPlayerById`, `GetPlayerCount` | server | `server/library/playermanager.md` | doc | L07, L24 |
| `Player`: `id soldier alive hasSoldier` RO; `teamId squadId input selectedKit isAllowedToSpawn` writable (the mod writes only `input`/`selectedKit` of its bots); `SelectWeapon(slot, unlock, unlocks{})`, `SelectUnlockAssets(customization, visualUnlocks{})`, `CreateSoldier(blueprint, transform) → SoldierEntity | nil`, `SpawnSoldierAt(soldier, transform, pose)`, `EnterVehicle(vehicle, entryId)`, `ExitVehicle(force, exitOnEntry)` | server | `server/type/player.md` | doc | L07, L08, L29 |
| `SoldierEntity`: `health maxHealth pendingPose` writable (never written), `pose isAlive isManDown worldTransform weaponsComponent player` RO; `SetPosition SetPose SetTransform Kill`; `velocity` via PhysicsEntity, `transform` via SpatialEntity | server | `server/type/soldierentity.md`, `physicsentity.md`, `spatialentity.md` | doc | L09, L10 |
| `SoldierWeaponsComponent.currentWeapon/currentWeaponSlot/weapons/isZooming` RO; server `SoldierWeapon.weaponFiring/name` RO, `primaryAmmo/secondaryAmmo/fireLogic` writable (never written: forbidden_assign) | server | `server/type/soldierweaponscomponent.md`, `soldierweapon.md` | doc | L11, L30, L31 |
| hook `Soldier:Damage(soldier RO, info, giverInfo RO | nil)`; `DamageInfo` (`damage position origin direction boneIndex isBulletDamage isExplosionDamage isClientDamage collisionSpeed distributeDamageOverTime`); `DamageGiverInfo` (`giver weaponUnlock damageType`; `weaponFiring` is a `DataContainer`, the WeaponFiringData, not the runtime `WeaponFiring`) | server | `server/hook/soldier_damage.md`; `server/type/damageinfo.md`, `damagegiverinfo.md` | doc | L14 |
| hook `BulletEntity:Collision(entity RO, hit, giverInfo RO) → bool | nil` | server | `server/hook/bulletentity_collision.md` | doc | L13 |
| hook `EntityFactory:Create(entityData, transform) → Entity | Component | nil`; filtered by the data type names `BulletEntityData MissileEntityData GrenadeEntityData` (children of `MeshProjectileEntityData`) and `ExplosionEntityData` (inventory kind `dtype`); the created entities are `ServerBulletEntity` etc. (`content\vext\entities.md`) | server | `server/hook/entityfactory_create.md`; `fb/bulletentitydata.md`, `missileentitydata.md`, `grenadeentitydata.md`, `meshprojectileentitydata.md`, `explosionentitydata.md` | doc | L12, L32 |
| `TicketManager:GetTicketCount(team) → float`, `GetCurrentRound`, `GetRoundCount` (`SetTicketCount` forbidden) | server | `server/library/ticketmanager.md` | doc | L16 |
| `TicketCounterEntity {team RO, ticketCount, initialTicketCount RO, controlledAreaValue}` | server | `server/type/ticketcounterentity.md` | doc | L15 |
| `CapturePointEntity` (`name playersInside isInPreRound isAttacked attackingTeam isFlagMoving previousOwner flagPole` RO; `team flagVelocity flagLocation flagHome isControlled isCaptureEnabled cooldownTimer team1/2CooldownPenalty` writable, never written) | server | `server/type/capturepointentity.md` | doc | L15, L21 |
| events `CapturePoint:Captured/Lost(capturePoint)`, `Player:EnteredCapturePoint/ExitedCapturePoint(player, capturePoint)`, `Player:Killed`, `Player:Respawn(player)`, `Player:Reload(player, weaponName, position)`, `Player:SuppressedEnemy(player, enemy)`, `Player:Left(player)`, `Soldier:HealthAction(soldier, action)`, `Soldier:ManDown(soldier, inflictor | nil)`, `Server:RoundOver(roundTime, winningTeam)`, `Server:RoundReset()` | server | `server/event/*.md` | doc | — |
| `RaycastManager:DetailedRaycast(from, to, maxHits, materialFlags, flags) → RayCastHit{}`, `CollisionRaycast` | server | `server/library/raycastmanager.md` | doc | L17 |
| `RCON:SendCommand(command[, args{}]) → string{}` (only whitelisted literal commands, no args: apicheck rule 11), `GetServerGuid() → Guid | nil` (`RegisterCommand` forbidden) | server | `server/library/rcon.md` | doc | L04, L05, L18, L28 |
| `NetEvents:Subscribe/SendTo(eventName, player, ...)/Broadcast` (server), `Subscribe/Send(eventName, ...)` (client) | server/client | `server/library/netevents.md`; `client/library/netevents.md` | doc | L19 |
| `Console:Register(name, description, callback) → ConsoleCommand`; `ConsoleCommand.name/groupName` RO | client | `client/library/console.md`, `client/type/consolecommand.md` | doc | L19 |
| `ClientUtils:GetCameraTransform() → LinearTransform | nil`, `WorldToScreen(worldPos) → Vec2 | nil`, `GetWindowSize() → Vec2 | nil`; client `PlayerManager:GetLocalPlayer` | client | `client/library/clientutils.md`, `client/library/playermanager.md` | doc | L19 |
| client `SoldierWeapon.correctedWeaponFiring/predictedWeaponFiring/aimingSimulation` RO; `AimingSimulation.fovMultiplier/zoomLevel/yaw/pitch` (read only by the mod) | client | `client/type/soldierweapon.md`, `aimingsimulation.md` | doc | L19 |
| launch `-server -dedicated -serverInstancePath <path> -high60 -unlisted -noUpdate`; `vu://join/<guid>[/<password>]` (GUID without dashes, the server key's id); console `connect <guid> [password]`; unlisted servers are joined only by those two | — | `content\general\args.md`; `content\general\commands.md` | doc | L18, L28 |
| `vu.FrequencyMode` returns `regular` / `high60` / `high120`; `vu.SuppressionMultiplier` default 100 (0 disables suppression incl. spread); `vu.TimeScale` default 1.0; `vu.DisablePreRound` must be set before the level loads (Startup.txt) | — | `content\hosting\commands.md` | doc | L04, L21 |
| instance layout (`server.key`, `Admin\MapList.txt` "MapID GamemodeID Rounds", `Startup.txt`, `ModList.txt`, `Admin\Mods\<name>`); several instances need separate keys and ports only when run concurrently | — | `content\hosting\config.md`, `content\hosting\mods.md` | doc | — |
| BF3 R25 `vars.*` semantics | — | R §3.1 (RCON protocol R25) | research | L05 |

The full list (one entry per symbol, used by `apicheck.py`, `luaload.py` and the mock) is `schema/api_inventory.json`.

---

## 8. Phase 0 live checks and fallbacks

| id | assumption | check (P0 step) | fallback |
|---|---|---|---|
| L01 | mod loads; server log lines `[VeniceEXT] [mharness] ...` | P0.1 | extractor accepts any `[mharness] MH1 {` prefix or a bare `MH1 {` line |
| L02 | `os.date`, `string.format('%.6g')`, coroutines in handlers; `json.encode` sample | P0.2 | run id from `ms`; encoder fallback `%.9g` |
| L03 | PostSim once per sim tick on the server | P0.3 | Engine:Update simulation-delta clock |
| L04 | `GetTickrate` 30/60; `RCON:SendCommand` returns `{'OK', value}` | P0.3 | hz from the PostSim rate; meta stores the raw table |
| L05 | `vars.*`, `modList.ListRunning` readable via RCON | P0.2 | values from `Startup.txt` copied in by the user's setup (`meta vars_src=startup`) |
| L06 | `Player:Chat` gives the message; `SendMessage(msg, player)` reaches the player | P0.1/2 | client console `mh ...`; `Yell` |
| L07 | `CreatePlayer` + equip + `CreateSoldier` + `SpawnSoldierAt` give a bot with the weapon; asset names resolve | P0.2, P0.6 | alternate asset names; spawn with the kit's default weapon and log its name |
| L08 | bot input via own `EntryInput` with `AuthoritativeAiming` drives fire/zoom/move/aim; latency constant; whether `EntryInput.deltaTime` matters (the mod always sets it to the tick dt; `meta.aimcal.dt`) | P0.5, P0.6, P0.7 | apply in PostSim instead of PreSim; aim via `EIAYaw/EIAPitch` closed loop on the body forward; fire 2-tick hold; if nothing moves with `deltaTime` set, the B scenarios are disabled for the night and the RUNBOOK says so |
| L09 | `SetPose(…, true, true)` immediate; `SetPosition` teleports and zeroes velocity | P0.6 | pose inputs; wait 1 s after a teleport |
| L10 | soldier `velocity` populated | P0.6 | finite differences of positions (the analysis does both) |
| L11 | `GunSway:Update` server-side for bot weapons, once per tick, `fireShot` once per shot; `da` in degrees, half-angle or full cone | P0.7, P0.11 | `nup` lets the analysis use the last update per tick; no event → the PostSim poll `gs src=poll` (§3.10) feeds W2-W4; W1 primary uses impacts only |
| L12 | `EntityFactory:Create` sees bullet creation; transform = muzzle + post-dispersion direction | P0.7, P0.8 | shots from `gs.fs`; directions from origin→hit |
| L13 | `BulletEntity:Collision` for bot bullets on static geometry; `hit.position` exact; `uniqueId` links | P0.7, P0.8 | ray-plane intersection from shot pos/dir |
| L14 | bot bullets damage soldiers under the shipped arbitration (fun-bots implies not, R §4.4) | P0.9 (YES / NO arbitration / NO aim) | U mode for W-B, S-A, W-C; "NO (aim)" leaves L14 open |
| L15 | iterators `ServerCapturePointEntity`, `ServerTicketCounterEntity`; casts and fields; HQs are capture points | P0.2, P0.13 | `TraverseAllEntities` + `Is` |
| L16 | tickets float; ticket cost of a bot spawn, a `Kill`, a `DeletePlayer` (`meta.tcost`) | P0.13 | conservative default 1/1/1 in the forecast |
| L17 | `DetailedRaycast` hits walls and ground with normals | P0.4 | `CollisionRaycast` |
| L18 | `RCON:GetServerGuid` gives the join GUID | P0.1 | the VU key portal lists the key's GUID; or relaunch without `-unlisted` and join through the server browser (`mh-measure`, passworded) |
| L19 | NetEvents both ways; the console command's full name (`groupName.name`, probably `mharness.mh`); camera forward sign; client log rows; ms offset | P0.1, P0.4, P0.12 | `!mh wall` from the human's server-side aim (needs the L08 convention from the bot calibration) |
| L20 | prints ≤ 1 KB intact; 60 rows/s without loss; live log readable. Peaks are higher (≈ 180-240 rows/s in 60 Hz W-A and in M-A with per-tick `tk`/`cp` rows), beyond what P0.12 verifies; any loss shows as `q` gaps per run (`extract --dry-run`), never silently | P0.12 | `rate 2`; extract after shutdown |
| L21 | round leaves preround with 1 human + 3 bots (round start needs 4 players, P §3.7; whether bots count is unverified); flags capturable | P0.13 | `vu.DisablePreRound true` in `Startup.txt` (RUNBOOK; expected outcome) |
| L22 | Lua state and created players survive a round restart (and so, presumably, a level change) | P0.15 | no `rr`/`nextmap` in the queue; the queue holds one round's worth |
| L23 | the server keeps simulating overnight with the client connected and idle (`vars.idleTimeout 0`) | overnight `prog` rows | — (the user stays connected) |
| L24 | `DeletePlayer` removes a bot cleanly | P0.6 | `Kill` + keep the player |
| L25 | human shots create server bullets | P0.10 | W-D from client rows |
| L26 | the user's loadout offers each W-B/W-C weapon without attachments (US kit: M16A3, RU kit: AK-74M) | W-B prompt | switch team in the menu; skip the weapon |
| L27 | shared and realm entries share globals or not; load order | P0.2 (`census.vm`) | none needed: the mod works both ways |
| L28 | the GUID string format (`tostring(Guid)` with or without dashes) | P0.1 | the plain line prints both forms |
| L29 | `SelectWeapon` / `selectedKit` behave for bots as for humans; a bot keeps the selected weapon after `SpawnSoldierAt` | P0.6 | wait for the soldier and read `currentWeapon.name`; respawn on mismatch |
| L30 | `SoldierWeapon.fireLogic` is the current fire mode; `EIACycleFireMode` on a bot's input cycles it | P0.7 | protocols a and c only (single shots and released pairs need no auto) |
| L31 | `EIASelectWeapon<n>` switches a bot's slot (n = slot + 1 assumed); `currentWeaponSlot` follows | P0.6 | `SelectWeapon` into slot 0 for the M9 as well (logged); skip M9 in W-B B mode |
| L32 | `entityData:Is('MeshProjectileEntityData')` matches its children (logged only; the three child names are tested explicitly) | P0.7 | — |
| L33 | `-debuglog` is not needed for log files in `ServerMeasure\logs` (R found `vu_*.log` without it) | P0.1 | relaunch with `-debuglog` (RUNBOOK) |
| L34 | chat commands and replies work while the user is on the deploy screen | P0.16 | the user spawns and parks in a safe corner overnight |
| L35 | RCON `version` returns the running VU build (`meta.build`) | P0.1 | the extractor takes the build from the log header |

---

## 9. Extractor (`extract.py`)

```
python -I extract.py --session 20261016..20261017 \
   --server-logs "C:\Users\olku\Documents\Battlefield 3\ServerMeasure\logs" \
   --client-logs "%LOCALAPPDATA%\VeniceUnleashed" --out <repo>\research\measurements [--since 2026-10-16T18:00]
   [--manifest-before x.csv --manifest-after y.csv] [--force] [--dry-run]
```
* `--session` is a date `YYYYMMDD`, an inclusive range `YYYYMMDD..YYYYMMDD` or a comma list of these. A run id
  carries the date its run **started** (`mh_clock`), so the evening session with its overnight queue and the morning
  60 Hz repeat spans two dates: the RUNBOOK passes `20261016..20261017`. Rows of runs outside the session are skipped
  but never silently: the summary prints `WARNING n rows of runs outside --session ... (run dates: ...)`.
* Refuses inputs under `C:\Program Files` and the normal instance (`...\Battlefield 3\Server`, separator-aware), and
  any output inside a log folder. Reads only `vu_*.log` files directly in those folders (no recursion), newest first,
  filtered by the timestamp in the file name.
* Per line: bytes up to 64 KiB (longer: count `too_long`, skip to the next newline); UTF-8 with replacement (count
  `bad_utf8`); regex `\[VeniceEXT\] \[mharness\] MH1 (\{.*\})\s*$` (also a bare `^MH1 {` for the L01 fallback). Other
  lines are ignored except the first 400 lines of each file, scanned only for `Build (\d+)` and
  `VeniceEXT engine v([\d.]+)`.
* JSON: `json.loads` with `parse_constant` raising (no NaN/Infinity), `object_pairs_hook` rejecting duplicate keys,
  depth ≤ 6, ≤ 64 keys per object. Never `eval`.
* Validation against `schema/rows_v1.json`: envelope types; known `e`; required keys and types; numbers finite; labels
  match the label pattern; **PII guard**: any string matching a GUID (`[0-9a-fA-F]{32}` or 8-4-4-4-12), an IPv4 address,
  or any key not in the schema → row dropped, counted `pii_suspect`/`unknown_key`.
* Split by `run`; client rows (`r=C`) join the server run named in their envelope. Per run: `raw.jsonl` sorted by
  (`r`, `q`) (gzip to `raw.jsonl.gz` if > 20 MB), `meta.json` {run, sc, hz, VU build, VeniceEXT version, meta rows,
  counts per `e`, malformed counts by reason, `q` gaps per realm, source log basenames and SHA-256, extractor version,
  session manifests' paths and compare result}. Session manifests live in `research/measurements/session-<date>/`.
* Deterministic and idempotent: the same inputs give byte-identical outputs; an existing run folder is skipped unless
  `--force`. Exit 0; 2 if malformed > 1 % of `MH1` lines or any `pii_suspect`.
* `--dry-run` writes nothing and prints per run: rows, malformed, gaps, `trunc`, and a **resume hint**: the last
  `cfg-done` per scenario with its index (`WA last cfg-done 7/17 -> !mh run WA from=8, queue item WA:all:from=8`). This is the crash-resume path
  of the RUNBOOK (the mod also echoes each `cfg-done` to chat).

---

## 10. Analysis rules and their code mapping (`analyze.py`)

`python -I analyze.py <run-dir> [<run-dir60>] [--items W1,W3,...]` prints per item: **verdict**, the numbers, the
implied change, and a draft SOURCES row; writes `summary.json` and `sources-draft.md` into the run folder. Data values
for predictions come from `data/weapon_constants.json` (each with its source). Every rule is tested on synthetic logs
for both variants (§14) and end to end through the mock (§13).

| item | data used | statistic and threshold | verdicts → change in our code | oracle |
|---|---|---|---|---|
| **W1** disc | W-A protocol a (single shots; shots whose `gs.rec` at the shot exceeds 2 % of one kick are **dropped**, not corrected); impacts on the wall plane (≤ 5 cm off-plane) | §10.1: KS distance of the normalised radius against uniform-area (F = ρ²) and uniform-radius (F = ρ), with the centre and a robust (median-based) scale estimated per model, an outlier screen, and critical values from a bootstrap that replicates the whole estimator; secondary normalisation by `gs.rr`/`da` must agree; per-config verdicts combine by §10.2 | `uniform_area` / `uniform_radius` / `empirical` (both rejected in ≥ 2 decisive configs: 16-knot CDF table, a new `DiscDistribution` value, a code change) / `inconclusive` / `no spread` / `sway-contaminated` → `Mechanics::disc` in `src/bf3_sim/include/bf3/sim/weapons/mechanics.h` (m2a-weapons §5.6, §6.9) | new P §7.1 row "spread disc" (H:W1) |
| **W2** growth/decay | gs `da` after the last shot (b, e), shot jumps (b); S-B MINRISE | decay slope by least squares over ticks (time = k/hz) with da > dmin (R² > 0.99); per-shot jump; min rise: da(t) after the moving flag: a step within 1 tick vs a ramp (slope ≈ decay rate ± 10 %). Units (§10.3): with the P0.11 verdict "degrees" the slope is compared in °/s; otherwise the unit-free ratio slope/dmin is compared with 15/2.5 (hip) and 15/0.2 (ADS) | slope = data DecreasePerSecond (15 °/s on rifles, topics/01 §2.1) ± 5 % → "data confirmed" (C14 closes, no change); = community 8 ± 5 % → record the factor (new field `Mechanics::dispersion_decrease_scale`, a code change); min rise → `Mechanics::min_spread_rise` `Immediate` / `AtDecreaseRate`; stance transition (S-B POSE firing) → `Mechanics::stance_transition_penalty` | P §7.1 M16A3 "hip 2.5-7 +0.1" row; P §8 C14 |
| **W3** recovery | gs `rec.p` after bursts (b, d, e); during firing (d) | per curve (time = k/hz): fit linear-to-zero p(t) = max(0, p0 − a t) and exponential p0·e^(−bt); a curve counts only with ≥ 6 non-zero samples (else excluded; a fast exponential at 30 Hz can leave 2-4); the winner needs RMS < 0.5 × the loser's and < 5 % of p0 in ≥ 80 % of counted curves, and ≥ 10 counted curves, else `inconclusive`; while-firing decay ratio vs after; lag-1 autocorrelation of yaw increments (d), pairs only inside each magazine curve (never across a curve boundary), **one-sided** r > 2/√n → correlated, fewer than 20 pairs → inconclusive (under independent kicks r is biased below 0 by the per-gap demeaning and the mean-reverting in-tick yaw recovery: the pass-5 two-sided \|r\| test read the null as correlated in 4/15 synthetic seeds; the hypothesis and the mock's `hcorr` are a positive correlation). Parameter comparison with `RecoilAmplitudeDecreaseFactor` 18 in degrees only with the P0.11 unit verdict; otherwise only the law and the unit-free ratio a/p0 (or b) are reported | `Mechanics::recovery` `Linear` / `Exponential` with the fitted parameter vs `RecoilAmplitudeDecreaseFactor` (ratio printed; unit inference: Linear reads the factor as degrees per second, m2a-weapons §5.6); ShootingRecoilDecreaseScale 0.1 [inferred in spread_recoil] confirmed or replaced; correlated yaw → a new open item for D7 (code change) | P §7.1 M16A3 "dec 18" row |
| **W4** first-shot re-arm | protocol c (pairs, or triples per L11) | gaps binned by the **measured** gap (k of shot 2 − k of shot 1, /hz), not the requested g (0.075 and 0.1 s both land on ≈ 3 ticks at 30 Hz); per bin: ratio Δp(shot 2)/Δp(shot 1) from `gs.rec`, with shot 2's Δp corrected for the in-tick recovery: for a held pair by the recovery seen in the tick before shot 2 (the trigger is down in both ticks, so the firing-scale recovery is the same), otherwise by the fitted W3 law (the W3 law is fitted with the trigger released and over-corrected held pairs: with a fast linear recovery every held ratio was unclear) (units cancel); armed if ≈ 1 (± 15 %), not armed if ≈ 1/FSRM (± 15 %); t_rec (rec back < 5 % after one shot) and t_min (da back to dmin) from gs | held (0.075) not armed in at least 4 classified held units and every released gap armed → `OnTriggerRelease` (unclassified held pairs are no evidence); armed iff gap ≥ t_rec + j for released gaps, and for a held pair iff its own recoil state before shot 2 is back under 5 % of shot 1's kick (t_rec is measured with the trigger released; held, recovery runs at the firing scale) → `WhenRecovered`; armed iff gap ≥ t_min + j (a held pair: its own `da` at `dmin`) → `WhenSpreadMin` (new enum value), with j = 0 (the shot sees its own tick's state) or j = 1 tick (it sees the previous tick's), no slack below t (t_rec, t_min and the gap all counted in gs-row ticks from the row that first shows the shot); both fitting with t_rec and t_min more than half a tick apart → inconclusive (the gaps do not separate them); `OnTriggerRelease` and a threshold law that both fit predict the same for every observed bin (no released gap below the threshold, no held pair recovered) → inconclusive, both named as indistinguishable, no code change (a fast linear recovery does this: t_rec under one tick, every held pair unrecovered at the firing scale); an `AfterTime` step between the held and the shortest released gap is always consistent with OnTriggerRelease and does not count against it; a step at a gap matching neither → `AfterTime(T)` (new enum value), never when T sits within half a tick of t_rec or t_min → `Mechanics::first_shot_rearm` | P §7.1 M16A3 "FSRM 2.5" row |
| **W5** falloff | W-B chest `dmg` pre values vs distance; per bone index | rows whose |`dist` − `fxd`| > 1.0 m are flagged (a clean hit reads ~0.6 m short: bullet origin ahead of the shooter position, hit point on the body surface)\|`dist` − `fxd`\| > 0.5 m are flagged and excluded (the client-arbitrated path's `origin` is unverified); fits per bone index, or after normalising each bone by its ratio to the chest at 5 m (an arm or stomach hit in a "chest" prompt otherwise inflates the RMS); piecewise linear with the data knots (start/end distance, start/end damage): RMS residual < 0.1 hp → `linear` confirmed; else a free-knot fit (grid 0.5 m) and the knots reported; M98B end knot free in [100, 175] → 150 (data) vs 120 (community) by the smaller RMS **only if** points exist beyond 150 m (range L − 2 ≥ 160), else `inconclusive`; B vs U (server vs client arbitration) compared when both exist; basis StraightLine vs PathLength is not separable (< 1 mm, m2a-weapons §5.9) → stays | `Mechanics::falloff` (only `Linear` exists; non-linear → new value + table, code change), `Mechanics::falloff_basis` unchanged | P §7.1 M98B and "shots to kill" rows |
| W6 (report) | PASS, PEN | T2 damage from the same `uid`/time window; ratio to T1 | `Mechanics::bullets_pass_soldiers` true/false (+ the ×0.8 cell); penetration through cover; `apply_protection_threshold` if damage below the threshold is zeroed | P §7.1 |
| W7 (report) | W-C | direct damage RPG/SMAW (225 vs 100), blast dmg vs distance from detonation: linear fit inner/outer radius vs a quadratic alternative, RMS | `Mechanics::blast_falloff` (only `Linear`), RPG `Damage` row | P §7.1 RPG, M320/M67 rows |
| **S1** regen | S-A REGEN/SUPP `hp` per tick (also W-B B regen waits) | onset = first tick with hp increase − tick of the last damage (/hz); rate = slope over the rise. SUPP reps count only with evidence of suppression (a `Player:SuppressedEnemy` ev for T or a change of T's gs `smf`); without any → "suppression not induced", no SUPP verdict | onset 10 ± 0.5 s and 5 ± 0.25 hp/s → data confirmed (`SoldierDef::regeneration_delay` / `regeneration_rate`, `src/bf3_core/include/bf3/core/gameplay/soldier_def.h`, used by the M2b health module); onset 16 ± 0.5 s → community value: a second timer (suppression) must be modelled (code change in the health module); SUPP onset relative to the last suppression event → `SuppressionAbortsHealthRegeneration` semantics | P §7.2 health row; P §8 C14 |
| S2 (report) | S-A IMMUNE | largest delay with pre dmg > 0 and hp unchanged | `Mechanics::spawn_protection_blocks`; `SoldierDef::immortal_time_after_spawn` (2) | P §7.2 |
| **S3** motor | S-B `hp.vel` (and position differences), time = k/hz | after a throttle step, three candidate laws: **exponential lag** v* − (v* − v0)·e^(−t/τ), **linear** (constant a, clipped at v*), **instant** (v* within 1 tick); winner by RMS < 0.5 × the next best (instant wins when the step completes within 1 tick); decel likewise; free fall: quadratic fit of y(t) → g (± 0.05); terminal speeds; fall damage vs impact speed table; jump apex. Our `FractionPerReferenceTick` *is* an exponential lag: an exponential winner maps to the gain at reference_hz 30 by g = 1 − e^(−1/(30 τ)) (soldier_motor.h `GainModel`, m1.md §6.4) and is compared with the data AccelerationGain 0.35 | `bf3-opensource/config/motor_tuning.cfg` keys `acceleration_model`, `deceleration_model`, `sprint_model` (exponential → `fraction_per_reference_tick` with the fitted g; `linear` with a; `instant`), `gravity`, `air_control_scale`, `reference_hz` (`MotorTuning`, m1.md §6.4, §6.6, §6.11); fall-damage curve vs `CollisionData` (m1.md §6.8) | P §7.2 speeds, jump/fall rows |
| **M1** bleed | M-A `tk.tf` inside the `hold` windows only (`mark lbl=hold k0 k1 own`; transitions excluded) | rate per state by least squares (tickets/min, 95 % CI); rule A predicts 15/min iff enemy AreaValue sum > 100 and > own, else 0; rule B: the rate grows with the lead (fit rate = 15 × f(lead), f linear and step candidates); mirrored states pooled with their originals | all states within ± 1 ticket/min of A → `rule A`; else `rule B` with the fitted f → the D12 bleed function (planned M4, P §3.7 "Bleed rule"; no code yet; inputs `CapturePointDef::area_value`) | P §7.3 conquest row |
| **M2** capture | M-B `cp.loc` per tick | T(n) = time from the tick the **n-th** attacker is inside to controlled; model T = 20 / min(1 + 0.34(n−1), 4) (20.0, 14.9, 11.9, 9.9, 8.5 s); flip = lose + get (40 s at n = 1); contested: loc slope 0; leave: return/decay rate | all T(n) within ± 5 % → `model holds`; else fit the per-player multiplier and the flip rule → the D12 capture function (planned M4; inputs `CapturePointDef` `time_to_get_control time_to_lose_control added_multiplier_per_player max_capture_multiplier return_multiplier seesaw min_nr_to_take_control`, `mode_def.h`) | P §7.3 conquest row |
| **T1** tick coupling | the same quantities from a 30 Hz and a 60 Hz run | ratio 60/30 per quantity (W2 slope, W3 parameter, W4 re-arm time, S3 gains, gravity); equal if \|ratio − 1\| < max(3·SE, 5 %). **Not informative**: quantities VU patched for high frequency (changeset 20056: "quicker acceleration", "faster damage expiration", R §4.5), i.e. the S3 acceleration/deceleration gains and damage-over-time expiry: equality there reflects VU's correction, not native BF3 (the patch itself suggests BF3's motor gain is per tick) | equal (informative quantities only) → per-second rates reproduce BF3 for them (C5 holds, D4 stands); different → list the tick-coupled quantities: our 60 Hz sim must convert them (`MotorTuning::reference_hz`; weapons recovery "per whole tick", m2a-weapons §5.6); the VU-patched quantities are reported as "VU-corrected, 30 Hz values are the oracle" | P §4 D4 |
| W8-W10 (report) | W-D | client vs server shot intervals (W8), reload commit vs weaponState (W9), vfov/hfov per zoom (W10) | `ClientFireRateMultiplier` meaning; `kReloadCommitted` rule (`empty_reload_unchambered`, H:W9); D13 camera FOV | P §7.1 |

### 10.1 W1 statistic (revised in pass 3)

Per config, with n impacts:

1. **Angles.** θ-plane coordinates of each impact: the angular offset of origin→hit from the aim direction, as a 2-D
   vector (yaw, pitch components, radians). Constant drop and aim bias move all points together.
2. **Centre.** ĉ = mean of the 2-D offsets (the estimated disc centre); θ_i = \|offset_i − ĉ\|.
3. **Outlier screen** (model-free): drop points with θ > 3 · median(θ) (under either model the largest true θ is
   √2 · median (uniform-area) or 2 · median (uniform-radius), so no true point is dropped); recompute ĉ and θ. The count
   is reported; more than 2 % dropped → the config is `contaminated` (inconclusive).
4. **Scale per model, robust.** R̂_A = √2 · median(θ), R̂_B = 2 · median(θ) (the median of θ is R/√2 under
   uniform-area and R/2 under uniform-radius). A single outlier moves a median by at most one rank, unlike max θ.
5. **Statistic.** D_A = sup \|F_n(θ/R̂_A) − min(ρ², 1)\|, D_B = sup \|F_n(θ/R̂_B) − min(ρ, 1)\| (one-sample KS against the
   model CDF clipped at 1; computed exactly from the sorted sample; the "two-sample KS against 100,000 model draws" of
   R §3.6 gives the same value within about 0.005 and is printed as a cross-check).
6. **Critical values** at α = 0.01 per model and n from a 4,000-replicate **parametric bootstrap that replicates the
   whole estimator**: draws from the model with an unknown centre, then steps 2-5 exactly as on the data (centre,
   screen, median scale). Values from this pass's simulation (`sims/w1crit.py`, numpy, seed 1): n = 100:
   0.217 (A) / 0.216 (B); n = 150: 0.181 / 0.180; n = 200: 0.160 / 0.161. The asymptotic 1.628·√((n+m)/(nm)) and the
   known-centre value (≈ 0.115 at n = 200) are invalid here and only printed for reference. (A bootstrap that
   re-estimated only the scale and not the centre would reject a true uniform-area disc about a fifth of the time: the
   review's finding.)
7. **Power** (same simulation): P(correct verdict) = 0.99 at n = 200 and n = 150, 0.90 at n = 100, for both true
   models; with 1 planted outlier at 1.2 R or 3 at 1.3 R: 0.99 (n = 200), 0.86 / 0.81 (n = 100). "Both rejected" occurs
   in about 1 % of true-model configs (α), which is why `empirical` needs two configs (§10.2).
8. **Secondary.** The same test with θ_i replaced by `gs.rr` at shot i normalised by its maximum (unit-free), and, once
   P0.11 fixes the unit and the half/full-angle reading, by θ_i / `da`(shot i). If a secondary is available and gives
   the other model, the config is `inconclusive`; if unavailable, the primary stands alone and the verdict says so.
9. **No-disc cases.** `dmin` = 0 → `no spread`; a config flagged `sway` (M98B ADS) or whose impacts show a drift
   correlated with time (|Spearman ρ(shot index, offset)| > 0.3) → `sway-contaminated`.

### 10.2 One `Mechanics::disc` from many configs

Decisive configs: protocol full (n ≥ 150 after the screen), `dmin` > 0, not contaminated or sway-flagged (in practice
the six M16A3 configs; tier-3 `short` configs with n = 100 are reported but never decide on their own). Then: every
decisive non-inconclusive config gives the same model M and at least two do → `Mechanics::disc = M`; any decisive config
gives the other model → `conflict` (no change; the per-weapon/stance pattern is reported for review); `empirical` only
if ≥ 2 decisive configs reject both models; otherwise `inconclusive`. A pooled test (normalised ρ of all decisive
configs, each with its own centre and scale, bootstrapped the same way) is printed as a cross-check.

### 10.3 Units plumbing

`analyze.py` reads the P0 run's `p0` rows first: `P0.11 units` ∈ {`deg`, `rad`, `unknown`} and `angle` ∈ {`half`,
`full`, `unknown`}. Rules that compare with data values in degrees (W2 slope vs DecreasePerSecond 15, W3 parameter vs
RecoilAmplitudeDecreaseFactor 18, W1 secondary via `da`) use them only when the unit is known; otherwise they report
unit-free ratios (W2: slope/dmin vs 15/2.5 and 15/0.2; W3: the law and a/p0) and say "unit unknown". Every rule takes
time as k/hz.

**SOURCES.md row** (draft, one per finding):
`| H:<item> | <quantity> | <value> | <unit> | <run id> | VU <build> | <Hz> | n=<n> | <statistic> | <consumer: file
field> | <oracle row> |`. `analyze.py` writes drafts; an agent reviews them before they go into
`research/measurements/SOURCES.md`, and `Mechanics::measured` becomes true only when all of W1-W5 have rows.

---

## 11. PowerShell scripts

### 11.1 `install-manifest.ps1 -Out <csv> [-Root <install>]` and `install-compare.ps1 -Before -After`

* Per item (root included): `Path` (relative), `Dir`, `Length`, `LastWriteUtc`, `CreationUtc`, `Sha256` for `.toc .cat
  .mft .json .xml .exe .dll` and `initfs_Win32` (R §4.2). Never `LastAccessTime`. Default root `C:\Program Files\EA
  Games\Battlefield 3`.
* `-Out` check: `[IO.Path]::GetFullPath`, then the deepest existing ancestor resolved through `LinkTarget` (junctions,
  symlinks); refused if equal to or under the install root (separator-aware), or under the normal VU instance. Read
  errors are fatal (no silent gaps).
* Compare: exit 0 with `install unchanged: N items`, else exit 1 with the differing rows (added, removed, changed
  fields). Also exit 1 on a header mismatch or an empty manifest.
* Tests: a scratch tree (files with each hashed extension, an `initfs_Win32`, nested dirs); negative controls (touch,
  content change, add, delete) must give exit 1; `-Out` inside the scratch "install", via `..`, and via a junction must
  be refused. Once read-only against the real install with `-Out` in scratch, printing only the summary line.

### 11.2 `setup-instance.ps1` (the user runs it)

```
pwsh -NoProfile -File setup-instance.ps1 [-Target <dir>] [-Session p0|s1|s2|s3] [-Password <pw>] [-NewPassword]
     [-CopyKeyFrom <path>] [-DisablePreRound] [-WhatIf]
```
* Default target `<MyDocuments>\Battlefield 3\ServerMeasure`. Refuses: the normal instance or anything under it, both
  as `<MyDocuments>\Battlefield 3\Server` **and** as the hard-coded absolute `C:\Users\olku\Documents\Battlefield
  3\Server` (they match today; OneDrive folder redirection would split them), separator-aware; the BF3 install;
  `%LOCALAPPDATA%\VeniceUnleashed`; the repo itself. Paths are compared after `GetFullPath` and after resolving the
  deepest existing ancestor through `LinkTarget`.
* Refuses to run if the target or any item inside it is a reparse point (junction, symlink): checked before writing,
  before mirroring and before deleting stale files in `Admin\Mods\mharness` (deletion never follows a link).
* Rerunning with `-DisablePreRound` (the P0.13 recovery) keeps the existing `server.key` (not touched without
  `-CopyKeyFrom`) and the existing password (read from the target's own `Startup.txt`); the server must be stopped
  first (the script warns if a `vu.exe` process has the target on its command line, read-only via CIM).
* Writes `Admin\ModList.txt` (exactly `mharness`), `Admin\MapList.txt` per session (`s1`: `MP_Subway ConquestSmall0 1`
  then `MP_001 ConquestLarge0 1`; `p0` = `s1`; `s2`/`s3` placeholders), `Admin\Startup.txt` (the neutral set of R §3.1:
  `vars.serverName "mh-measure"`, `vars.gamePassword`, `vars.regenerateHealth true`, `vars.soldierHealth 100`,
  `vars.bulletDamage 100`, `vars.playerRespawnTime 100`, `vars.gameModeCounter 100`, `vars.vehicleSpawnAllowed true`,
  `vars.vehicleSpawnDelay 100`, `vars.friendlyFire false`, `vars.autoBalance false`, `vars.idleTimeout 0`,
  `vu.SuppressionMultiplier 100`, `vu.TimeScale 1.0`; `vu.DisablePreRound true` only with `-DisablePreRound`), and
  mirrors `mod\mharness` into `Admin\Mods\mharness` (stale files removed only inside that folder).
* Password: generated locally (`RandomNumberGenerator`, 12 chars `[a-z0-9]`, URL-safe for `vu://join`), shown once;
  later runs keep the password in the target's own `Startup.txt` unless `-Password`/`-NewPassword`.
* `-CopyKeyFrom`: copies the named file's bytes to `<target>\server.key` (never printed, never hashed to output; size
  check only: 1-4096 bytes). This is the script's only read outside the repo and the target. Without it the script
  reminds the user that a key is needed.
* Idempotent (second run: "no changes" except the mod mirror), `-WhatIf` via `SupportsShouldProcess` prints every
  intended write. Prints the launch lines for 30 Hz and 60 Hz.
* Tests: `-WhatIf` (no file written), a scratch target (twice: idempotent), refusals (normal-instance path, its
  subfolder, a path that merely starts with "Server", the install, a junction into the normal instance), a fake key
  copied byte-exact.

---

## 12. Lua verification without VU

### 12.1 `luaload.py`: real Lua 5.4 (built, §16)

* Runtime `lupa.lua54` (`_VERSION` asserted `Lua 5.4`), one fresh `LuaRuntime(register_eval=False,
  register_builtins=False)` per realm and entry order. A host chunk keeps the real libraries; the mod sees only `env`:
  the VU library whitelist (§3.1), `string.starts/ends`, a `json` stub, VU's `require` (§3.1 rules, refuses non-`.lua`
  paths and missing files), strict globals (reading an undefined global errors with file:line, except `MH`; assigning
  any global except `MH` errors), enums from the inventory with unknown values erroring, and permissive recorder stubs
  for the VU libraries (`Events:Subscribe`, `Hooks:Install`, `NetEvents:Subscribe` names recorded).
* Steps per realm (server, client) and per order (shared entry first, realm entry first): compile every file
  (`load(src, '@ext/<path>', 't', env)`); run both entries; then require every remaining file on its own (scenario
  modules load lazily). Recorded event/hook names must be in the inventory for that realm or shared; NetEvent names
  must start `mh:`.
* Negative tests (run during this pass on a scratch copy): `io.open` → "undefined global 'io'"; a global assignment →
  error; an unknown enum value → error; an event not valid on the client → error; a missing `require` target → error;
  syntax errors → the compiler's file:line message.

### 12.2 `apicheck.py`: static checks

Over every `.lua` in the mod, with the realm from the path (`ext/server` → server + shared, `ext/client` → client +
shared, `ext/shared` → shared). Tokens come from `py/mhlib/lualex.py` (a Lua 5.4 lexer: names, numerals incl. hex
floats, short strings with every escape, long strings/comments of any level), so strings and comments never trigger
rules.
1. `Events:Subscribe('<name>'`, `Hooks:Install('<name>'` names exist for the realm; NetEvent names are ours (`mh:*`).
2. `Lib:Method(` for inventory libraries: the method exists in that realm.
3. `Enum.Value`: the value exists.
4. `x.field` / `x:method(` on non-mod names: the member exists on some inventory type (coarse; the mock checks exactly).
5. Forbidden symbols and strings (`forbidden_symbols`, `forbidden_strings` of the inventory).
6. Assignments `x.<field> =` with a `forbidden_assign` field → error anywhere; any engine-field assignment outside
   `mh_bots.lua` → error; inside it only `bot_assign` fields.
7. Fixture calls (`fixture_calls`) only in `mh_bots.lua`, and the enclosing function also calls `fx(`.
8. `print(` only in `mh_row.lua`.
9. Literal `row('<e>', { k = ... })` and `prompt(...)`: keys ⊆ schema keys of `<e>`.
10. `require` paths resolve to files of the mod.
11. `RCON:SendCommand(` only with a string literal first argument from `rcon_read_whitelist.read` and no second
    argument; `rcon_read_whitelist.admin` literals only in `mh_cmd.lua` / `mh_runner.lua`, in a function that also
    emits `fx(`. Any other form (a variable, a concatenation, an args table) is an error.
12. `ctx:wait`, `ctx:wait_ticks`, `ctx:wait_until`, `coroutine.yield` inside a function literal that is passed as a call
    argument (a callback) → error (no yield across a C boundary, §3.1).
13. String literals passed to `:Is(` are inventory `dtype` names or entity type names of `content\vext\entities.md`
    recorded in the inventory.
14. `--vudocs <dir>` (optional, read-only): `py/mhlib/vudocs.py` re-confirms every inventory entry (page, name, enum
    value, RO flag) and prints `confirmed n/N`; any mismatch fails. If the directory is unreadable, the check prints
    "VU-Docs not readable: inventory facts unconfirmed this run" and does not fail (the Phase 0 census still checks
    presence live).

### 12.3 Inventory confirmation (`py/mhlib/vudocs.py`, built)

Per entry kind: `prop` → the `prop` shortcode with the name on the type page and its read-only marker; `method`,
`event`, `hook`, `ctor` → a `> **Name**(` signature line; `enum` → the value row and its integer; `global` → the name
on the page. Pages larger than 512 KiB are refused. Output: counts and failure reasons only. Run today: 308/308 (pass 3; kind `dtype` checks the page and its "Inherits from" parent).

---

## 13. Mock VU runtime (`py/mockvu`)

### 13.1 Purpose and limits

A test double for **our assumptions about VU**, so every scenario runs end to end without VU: chat command in →
fixtures, hooks and samplers → `MH1` rows in VU-format log files → `extract.py` → `analyze.py` → the verdict for the
law the mock was set to. It implements only the VU surface the mod uses (driven by `schema/api_inventory.json`), with
simple, known laws. It is not VU: every behaviour it assumes is listed in §13.8 with the live check that confirms it.

### 13.2 Structure

* **Python host** (`host.py`): creates one `lupa.lua54` runtime per realm (server, client), mirroring the network
  split; loads the mock Lua core into each runtime outside the mod's sandbox (the same sandbox builder as `luaload.py`,
  with real objects instead of recorder stubs); loads the mod (both entry orders, and the "separate states" variant
  where the shared entry runs in its own `env`); then advances the clock in chunks of 10 ticks inside Lua
  (`mock.run_ticks(n)`), so the per-tick work never crosses into Python. Between chunks it drains each realm's print
  buffer to a log file, delivers NetEvents queued by either realm (one tick latency), and lets the scripted user act.
* **Logs**: `<scratch>/instance/logs/vu_YYYY-MM-DD_HH-MM-SS_NNNN.log` (server) and `<scratch>/client/vu_*.log`
  (client), header lines `Initializing Venice Unleashed Server (Build 20682)` and `VeniceEXT engine v1.15.0` [mock],
  then `[<time>] [info] [VeniceEXT] [mharness] <text>` per print, `<time>` from the mock wall clock. Optional faults:
  truncate lines over N bytes, drop a fraction of lines, insert garbage lines.
* **Lua core** (`py/mockvu/lua`):
  * `vu_core.lua`: classes generated from the inventory (properties with RO flags, methods, inheritance chain from
    `inherits`); unknown members raise `mock: <Type>.<name> not in api_inventory` (a dynamic API check of every path
    the tests execute); RO writes raise; every write to an engine object goes into the **journal** {tick, type, field,
    owner: bot/human/none}; casts (`SoldierEntity(e)`) check the runtime type; `Events` (subscribe, dispatch in
    subscription order), `Hooks` (priority order, a `HookContext` with `Call`, and `Pass`/`Return` that work but are
    journaled as violations), `NetEvents`, enums from the inventory, `json.encode` (VU-like, for the census sample).
  * `vu_math.lua`: `Vec3` (+, −, scalar ×, `magnitude`), `LinearTransform` (left, up, forward, trans), `Quat` as needed.
  * `vu_server.lua`: `PlayerManager` (humans and bots, `CreatePlayer` → `Player` with `id`, `DeletePlayer`),
    `ChatManager` (messages to a per-player inbox the scripted user reads), `RCON:SendCommand` (vars from a table;
    `mapList.runNextRound` triggers a level change), `GetServerGuid`, `TicketManager`, `RaycastManager` (against
    `vu_world`), `ResourceManager:SearchForDataContainer` (the weapon/kit/blueprint names of `wa_cfg.lua`, case
    rules per switch), `EntityManager` iterators and `TraverseAllEntities`, `SharedUtils`.
  * `vu_client.lua`: client `PlayerManager:GetLocalPlayer`, the local soldier's client `SoldierWeapon` with
    `predictedWeaponFiring` / `correctedWeaponFiring`, `ClientUtils` (camera = the user's eye, a pinhole projection with
    a vertical FOV per zoom level, window 2560 × 1440), `Console:Register`.
  * `vu_world.lua`: level layouts from `levels.py` (ground plane y = 0 with a flat area, one wall plane with a normal, a
    range lane of 180 m, flags as circles with HQs, spawn points); ray/plane/capsule intersection; soldier capsules
    with zones (head, chest, body, legs) for hits.
  * `vu_soldier.lua`: motor (§13.3 laws), poses with transition times, jump, gravity, fall damage curve, health,
    regeneration, spawn immunity, suppression events, `Player:Killed/Respawn`, man-down off.
  * `vu_weapons.lua`: fire timing from rpm, magazine and reload with `weaponState` transitions, `GunSway` state (spread
    min/max/increase/decay per stance and zoom, recoil offset with kick, horizontal draw, first-shot multiplier, recovery
    law, re-arm rule), the random polar sample exposed as `randomAngle`/`randomRadius` in the unit the switch says,
    bullets as straight lines with drop (v0, g), `EntityFactory:Create` for bullets/missiles/grenades, collisions with
    the wall/ground (`BulletEntity:Collision` with `RayCastHit`) and soldier capsules (`Soldier:Damage` with
    `DamageInfo` and falloff), explosives with fuse and blast falloff.
  * `vu_conquest.lua`: flags (`flagLocation` 0-100, attacker counts from capsules inside, capture law), HQs, tickets as
    floats with bleed, round over at 0, preround flag.
* **Clock**: 30 or 60 Hz; per tick: `UpdateManager:Update(dt, PreSim)`, the simulation step, `GunSway:Update` (once
  per tick, or n times per tick with dt/n under `gs_cadence=frame:n`), `UpdateManager:Update(dt, PostSim)`,
  `Engine:Update(dt, dt)`. `SharedUtils:GetTimeMS` and `os.date` (overridden in the mod env only) follow the mock
  wall clock, so runs are deterministic for a seed.

### 13.3 Law switches (`laws.py`; each recovered by an analysis rule)

| switch | values (default first) | recovered by |
|---|---|---|
| `disc` | `area` (r = s·√u) / `radius` (r = s·u) | W1 |
| `ra_units` | `deg` / `rad` / `norm` (randomAngle, randomRadius as degrees, radians, or 0-1) | W1 secondary, P0.11 |
| `decay` | 15 / 8 °/s (DecreasePerSecond as data or community) | W2 |
| `min_rise` | `immediate` / `ramp` | W2 |
| `recovery` | `linear` (18 °/s) / `exp` (rate 18/s) | W3 |
| `hcorr` | 0 / 0.6 (lag-1 correlation of horizontal kicks) | W3 |
| `rearm` | `release` / `recovered` / `spreadmin` / `timer:0.3` | W4 |
| `falloff` | `linear` / `smooth` (smoothstep between the knots) | W5 |
| `m98b_end` | 150 / 120 m | W5 |
| `pass_soldiers`, `prot_threshold` | off / on | W6 |
| `blast` | `linear` / `quadratic`; `rpg_direct` 225 / 100 | W7 |
| `regen_delay`, `regen_rate`, `supp_abort` | 10 / 16 s; 5 hp/s; on / off | S1 |
| `immune_s` | 2 s | S2 |
| `motor` | `fraction` (an exponential lag: 0.35 per 30 Hz tick) / `linear` (a m/s²) / `instant` | S3 (exponential / linear / instant; the fitted g at 30 Hz recovered within 0.02) |
| `gravity` | 9.81 / 15 m/s² | S3 |
| `bleed` | `A` / `B` (rate × lead) | M1 |
| `capture` | `model` (20 / min(1 + 0.34(n−1), 4)) / `linear_n` (20 / n); `flip` `lose_get` / `direct` | M2 |
| `hz`, `coupled` | 30 / 60; `none` / `accel` / `recovery` / `decay` (that quantity applied per tick, not per second) | T1 |

Robustness switches (the R §9 unknowns and Phase 0 failure modes): `bot_damage` yes/no (no: bot bullets collide with
soldiers but never call `Soldier:Damage`); `gs_cadence` `tick` / `frame:n`; `gs_server` yes/no (no `GunSway:Update` for
bot weapons on the server); `ef_bullets` yes/no (`EntityFactory:Create` not called for bullets); `hit_noise` 0 / 2 cm;
`idle_advance` yes/no (no ticks while no human is connected); `rcon_vars` yes/no; `iter_types` yes/no (named
iterators return nothing, `TraverseAllEntities` still works); `vm` `same` / `separate`; `init_order`; `preround`
yes/no; `asset_case` `exact` / `insensitive`; `print_limit` bytes; `log_drop` fraction; `spawn_fails` (a config's
`SpawnSoldierAt` silently does nothing → the stuck path); `chat` on/off (off: only the console path works);
`level_change` (Lua state kept / reloaded); pass 3 adds: `fire_mode` `auto` / `single` / `burst` (the mode a weapon spawns in; `EIACycleFireMode` cycles it, or `stuck` ignores the input); `input_dt` `ignored` / `required` (required: bot inputs with `deltaTime` 0 do nothing, so a mod that forgot it fails every B scenario); `w1_outliers` k@f (k impacts per config planted at f·R); `tcost` spawn/kill/remove ticket costs (default 1/0/0; 1/1/1 in the conservative runs); `rr_state` kept / lost (a round restart reloads the Lua state); `member_missing` raise / nil (reading an absent member raises, as assumed in §3.1).

### 13.4 Observe-only enforcement at runtime

The journal must show, for every e2e run: no `Pass`/`Return`; no write to any object owned by a human or by no player
(flags, tickets, world); writes only of `bot_assign` fields on bot players and their `EntryInput`s; every fixture call
(`fixture_calls`) followed within the same tick by an `fx` row for that bot; every `RCON:SendCommand` a whitelisted read without arguments, or an admin command followed by `fx act=admin`. A test fails otherwise.

### 13.5 Scenario coverage (e2e cases in `mockrun.py`; `--list` prints them with their suites)

| run | laws / faults | expected |
|---|---|---|
| P0 all | defaults | every `p0` step `ok`; census api N/N; `vm` reported; extractor 0 malformed |
| P0 faults | `bot_damage=no`, `ef_bullets=no`, `gs_cadence=frame:2`, `preround=yes`, `chat=off` (console) | P0.9 "NO (arbitration)", P0.7 not ok with `shots 0, fs 10` (the `gs.fs` fallback count), P0.8 reported, P0.13 "preround", commands and replies via the console path |
| P0 aim fault | aim offset 30° on bots (`aim_bias`) | P0.9 "NO (aim)", not "NO (arbitration)" |
| P0 level | `rr_state=kept` / `lost` | P0.15 ok (in P0 all) / no P0.15 row at all (the run dies with its Lua state, the greeting comes again); with `lost` the queue refuses `rr`; the old state's bots stay as orphans (reported as a note) |
| fire mode | `fire_mode=single` (cycled to auto) / `fire_mode=stuck` | W-A all parts / parts a and c only with `no-auto` marks; FIRE_N timeouts counted |
| input dt | `input_dt=required` | every B scenario still works (the mod sets `deltaTime`) |
| no server GunSway | `gs_server=no` | P0.7 reports `src=poll` only; W2-W4 verdicts unchanged from `src=poll` |
| W-A tier 1, `parts=a,b,c,d,e` | `disc=area`, then `disc=radius` | W1 `uniform_area` / `uniform_radius` (n = 200 per config), combined by §10.2 |
| W-A outliers | `disc=area`, `w1_outliers=1@1.2` and `3@1.3`, a world origin 1500 m away | W1 still `uniform_area` (no quantisation loss with `%.9g`) |
| W-A tier 1 | `recovery=linear` / `exp`; `rearm=release` / `recovered`; `decay=15` / `8`; `hcorr=0` / `0.6` | W3, W2, W3-yaw verdicts match; W4 matches with `recovery=exp` and is `inconclusive` (release and recovered indistinguishable) with `recovery=linear` |
| W-A W4 re-arm | `rearm=spreadmin` (decay 8, decay 15 at seed 2) and `timer:0.3`, default M16A3 data, `parts=a,c` | W4 `spreadmin` / `timer:0.3` (the pass-4 rule read `timer:0.2` for spreadmin: t_min is a whole number of ticks) |
| W-A W4 alias | `recovery=linear` with `rec_rate=30` and `rearm=recovered` / `release` (seed 4); `hz=60`, `decay=15`, `rearm=recovered` | W4 `inconclusive` with release and recovered named indistinguishable, ≥ 4 held units classified, no code change (pass 5 read `release` with no held pair classified) |
| W-A W3-yaw null | `recovery=linear`, `rec_rate=18`, `hcorr=0`, `parts=d` (seed 4) | W3-yaw `none` (pass 5 read `correlated` at r = −0.185) |
| W-A W1 hit noise | `disc=area`, `hit_noise=0.02` | W1 `area` or `inconclusive`, never `radius` or a conflict |
| W-A small n | `disc=area`, `scale=0.1` | W1 `inconclusive` |
| W-A frame cadence | `gs_cadence=frame:3`, `ra_units=rad` | W1-W4 verdicts unchanged (analysis uses `nup` and impacts) |
| W-A60 + W-A | `hz=60` with `coupled=none` / `recovery` | T1 equal / tick-coupled (recovery); W4 binned by measured gaps |
| S-B | `motor=fraction` / `linear` / `instant`; `gravity=9.81` / `15`; `hz=60, coupled=accel` | S3 exponential (→ g) / linear / instant and g; T1 lists the gains as "not informative (VU-corrected)", never as equal |
| W-B mode B | `falloff=linear` / `smooth`; `m98b_end=150` / `120` | W5 verdicts |
| W-B mode U | `bot_damage=no`; the scripted user equips and shoots on prompts, 10 % of its "chest" hits land on an arm; a 150 m range | W5 from user shots (`cli` true) still `linear` (per-bone normalisation); M98B end knot `inconclusive` (range < 160 m); prompts answered; target respawns equal the heal-policy forecast; no bot-bullet damage rows |
| S-A mode B / U | `regen_delay=10` / `16`; `supp_abort=on`; `supp_effect=none` (bot bullets do not suppress) | S1 verdicts; S2 report; with `supp_effect=none` "suppression not induced" and no SUPP verdict; REGEN reps without respawns |
| M-A | `bleed=A` / `B` (single runs); and `queue MA rr nextmap MA` after `p0 level confirm` | M1 verdicts from hold windows only; four ends `done`; the second MA runs on MP_001 |
| tickets | 120/120 tickets, `tcost=1/1/1`: a 7-item MB line, then one MB; 110/110 with `tcost` spawn 15 for a W-A run (forecast at 1/1/1 passes, the real cost does not) | the long line refused with "forecast ... put rr before it"; the accepted item prints its forecast (T1 109 T2 112: the user's own spawn cost one); the under-forecast run trips the guard (`guard`, item requeued, queue paused) |
| queue validation | `!mh queue WB:B`, `SB WA:tier1`, `MA WA:tier1`, `MA SA:B`, `run WA` with no sites | each refused at queue time with the missing site, nothing queued, nothing started |
| RUNBOOK re-prepare | a fresh 60 Hz host and a fresh 30 Hz host; the commands of the RUNBOOK `reprepare` block, **parsed from RUNBOOK.md**, then `queue WA:60 SB` / `queue WB:B SA:B SB MB` | before: refused with every missing prerequisite in one reply; after: every R step ok, P0.3 at the host rate, the line accepted, the first item starts, no orphan bots; RUNBOOK step 13 queues `WA:60 SB` right after R |
| M-B | `capture=model` + `flip=lose_get` / `capture=linear_n` + `flip=direct` | M2 and M2-flip verdicts (the direct flip never reads neutral: the reset treats "taken, not controlled" as neutral) |
| W-C | `blast=linear` / `quadratic` | W7 report |
| W-D | client realm: pred/corr gs rows, fov rows for 3 zoom levels | fov within 0.1° of the mock's vertical FOV |
| stuck | `spawn_fails=WA:1` (config 1 spawns are no-ops); `WA:*` with `queue WA:rest:parts=b,c,d SB` | `cfg-skip`, run `done`; every config skipped → one `aborted` end mark ("3 consecutive skips"), then SB `done`; no orphan bots |
| repeats | `run WA` twice, `clear` while running, `ok` with no prompt, `queue` of two items while running, `queue`, `queue clear` twice, `stop` twice, `wall` twice, `clear` twice, unknown verb, `run XX`, `queue NOPE:x`, `status` | the replies of §5; the queued items never start; no handler errors; no orphan bots (PlayerManager count) |
| no human | the user leaves 8 s into a W-A run and comes back: `idle_advance=no` (30 s away) and `yes` (150 s away, longer than the 120 s stall timeout) | `human-left` and `human-back` marks, no `stalled`, the run ends `done` |
| log faults | `print_limit=600`, `log_drop=0.01`, `log_garbage=0.002` | extractor exit 0 or 2, malformed rows and q gaps counted per run; W2 still analysed from the damaged runs |
| meta many weapons | one Lua state: P0 damage, W-B mode B, W-C, P0 aim and bots, W-D (8 weapons), then a run meta with oversize args | no `trunc` row; every meta row < 1000 B; every extracted run has a `dict` name for each wid its meta rows list; the W-D meta lists 8 wids; the oversize-args meta row has `argsx` and no `args` |

Default suite: scaled protocols (`scale`, `parts`) so the whole e2e set runs in a few minutes; `--full` runs the full
tier-1 W-A protocol and the whole overnight queue at 30 Hz (≈ 0.6 M ticks) as a stress run.

### 13.6 Scripted user (`user.py`)

A Python agent with a soldier in the mock world: joins, spawns (team per weapon), stands at a site and looks at a
target (sets the client camera), types chat commands (`Player:Chat`) or console commands (`mh:cmd`), reads its chat
inbox and the `prompt` rows, and acts on them: equips the prompted weapon, aims at the target zone and fires
(`EntryInput` of the human through the same mock weapon path, `isClientDamage` true), answers `!mh ok`, throws a
grenade at the ring centre, suicides for S-A RESPAWN, fires magazines and reloads for W-D, steps out of the line of
fire. Scripts per scenario mirror the RUNBOOK steps, so the RUNBOOK order is exercised.

### 13.7 Determinism

All randomness comes from seeded generators (the mock world, the scripted user's aim noise); two runs with the same
seed and laws give byte-identical logs, and the extractor output is byte-identical too (tested).

### 13.8 What the mock assumes about VU (each is a live check)

The tick pipeline order and PostSim once per tick (L03); `GunSway:Update` fires for bot weapons on the server with
`fireShot` once per shot (L11); `randomAngle/randomRadius` hold the last polar sample (L11, P0.11); bullets are
straight lines with drop and `EntityFactory:Create` sees them with the post-dispersion direction (L12); collisions
report the exact point (L13); `Soldier:Damage` fires before damage is applied and `Call()` applies it (L14); bot
`EntryInput` with `AuthoritativeAiming` aims and fires like a human (L08); `SetPosition` zeroes velocity and `SetPose`
is immediate (L09); iterators by engine type name work and HQs are capture points (L15); tickets are floats (L16);
raycasts return hits with normals (L17); NetEvents arrive the next tick (L19); a level change keeps the Lua state
(L22); `DeletePlayer` removes a bot (L24); human shots create server bullets (L25); the log line format (L01). The mock
also chooses VU-like but invented details where VU-Docs is silent (window size, client FOV per zoom, asset case
rules); tests must not depend on their exact values.

---

## 14. Test plan (`check_all.py`)

| stage | content |
|---|---|
| layout + JSON | every file of §2 present; `rows_v1.json`, `api_inventory.json`, `weapon_constants.json`, `mod.json` parse; `mod.json` `HasVeniceEXT` true and `veniceext ^1.15.0` |
| `luaload.py` | 0 errors, both realms, both entry orders (§12.1); `tests/test_luaload.py` runs the negative corpus (`tests/fixtures/lua_bad/`) and expects each error |
| `apicheck.py` | the whole mod with 0 errors; `tests/test_apicheck.py`: each rule with a positive and a negative snippet; `--vudocs` when the clone is readable (279/279 or more) |
| `tests/test_extract.py` | tiny fixture logs (server + client format, header lines); malformed: truncated JSON, 100 KiB line, NaN, invalid UTF-8, duplicate keys, deep nesting, missing envelope keys, wrong types, GUID/IP strings, unknown keys, unknown `e`; idempotence (byte-identical rerun); refusals (install path, normal instance); run split incl. client rows; gzip threshold |
| `tests/test_stats.py` | KS statistic and p-value against known values; bootstrap critical value reproducible with a seed; fits on exact synthetic curves |
| `tests/test_synth_rules.py` | `synth.py` generates logs for **both** variants of each rule (W1 area/radius; W2 decay 15/8, min rise; W3 linear/exp, hcorr; W4 release/recovered/timer; W5 linear/smooth, 150/120; S1 10/16 s; S3 fraction/linear, g; M1 A/B; M2 model/alt; T1 equal/coupled) with seeds; `extract` → `analyze` returns the generating variant; `inconclusive` when n is too small |
| `mockrun.py --suite all` (stage mock-e2e) | the §13.5 runs: scenario in → rows → extract → analyze → expected verdict; every printed row validates against the schema; the journal passes §13.4; `--coverage` adds the Lua line coverage of the mod over the suite (host-side `debug.sethook`, invisible to the mod) per file with the uncovered lines (approximate executable lines; reported, not a gate); on a full pass `--write-cert` writes `mh_cert.lua` |
| `tests/ps/run-ps-tests.ps1` | §11.1 and §11.2 tests on scratch trees under the scratchpad (plain asserts; no Pester needed) |
| real install | the read-only real-install manifest (summary line only) runs by default; `--no-install` skips it |

`check_all.py` exits 0 only if every stage passes and prints one PASS/FAIL line per stage.

**Staged gate `check_all.py --phase0`** (for a partly finished harness tonight): layout + JSON, invgen, `luaload`, `apicheck`
(whole mod), `tests/test_extract.py`, the PowerShell tests, and the mock e2e runs of P0 (all P0 rows incl. faults,
level, fire mode, input dt, no server GunSway) and of the attended U scenarios (W-B U, S-A U, W-D, W-C) with the
repeats/stuck/log-fault runs. It prints `phase0 gate passed: Phase 0 and the attended items may run; the overnight
queue may not` and exits 0. Each passing run writes the generated `mod/mharness/ext/shared/mh_cert.lua` (the certified
scenario ids, the gate name and the commit; ignored by git), which `setup-instance.ps1` copies with the mod; an
uncertified scenario answers `[mh] WA not certified tonight` to `run` and `queue`. Without the file only the `p0`
commands, `census`, `status` and the site commands work.

---

## 15. Risks and open questions

* Logs: W-A at 30 Hz ≈ 35 rows/s × ~400 B (with `%.9g`) ≈ 50 MB/h raw, ≈ 180-260 MB for `WA:all`, ≈ 20-30 MB gzipped. Whether runs
  this size go into git (or only `meta.json` + `summary.json` with the raw file kept locally and hashed) is for the
  user to decide.
* `data/weapon_constants.json` has the stand values of P §7.1 and topics/01 §1-§2; crouch/prone minima and every
  non-rifle DecreasePerSecond are null and must be filled from our own `bf3dump weapon-sim` (read-only on the install)
  before W2 compares them; until then W2 reports the measurement alone.
* The W-A wall must be flat over ~3 × 3 m with ~26 m of clear, flat floor in front, away from flags (bleed). The drop
  tests need open sky (Metro's outdoor start area).
* If the round ends overnight (the forecast or the guard misses something, or Zeus drops the server), the queue stops;
  `extract.py --dry-run` prints the resume index and `from=` resumes the next evening.
* Ticket costs are unmeasured until P0.13; the forecast is conservative until then. Bazaar's initial tickets are read
  live after `nextmap` (assumed 300 in the queue-time forecast) [inferred].
* The bench (§3.9) assumes idle bots on their own HQ area are harmless: no AI, no out-of-bounds kill on their own HQ
  [inferred]; P0.13 benches the first bots and the census reports deaths.
* W1 power (§10.1) is simulated with ideal discs; real impacts may carry sway or residual recoil the screen does not
  remove. §10.2 never lets a single config decide, and `conflict` stops any code change.
* The mock can only test our reading of VU; a wrong shared assumption (e.g. the aim convention) passes the mock and
  fails live. Phase 0 is designed to catch exactly those (§8), and each P0 step has a fallback that keeps Session 1
  useful.
* VU version drift: the VU-Docs clone is at changeset 20878 and the changelog lists builds 20818-20878 after the
  installed 20682; the client will probably auto-update tonight. None of those changes touch the API used here (WebUI,
  UI scaling, `vu.SquadSize`), the census checks presence live, and `meta.build` records the running build.
* Everything marked [live:Lnn] is unconfirmed until Phase 0 tonight.

## 16. As-built notes

* **2026-10-16, design pass 2.** Kept from pass 1: the mod skeleton, schema, inventory, scenario state machines,
  analysis rules, PowerShell design and RUNBOOK. Changed: the home-grown Lua parser of pass 1 is replaced by the real
  Lua 5.4 runtime (`luaload.py`, lupa 2.8, `lupa.lua54`); the mock VU runtime (§13) is new; `prompt` rows, `census.vm`
  and `guid_nodash`, the `parts`/`scale` arguments, stuck/no-human handling, live checks L26-L29 and the forbidden
  assignment lists are new; weapon constants were filled from topics/01 (M416, AK-74M, rifle DecreasePerSecond 15).
* `luaload.py` built and run: 31 skeleton files, both realms, both orders, PASS; negative copy in scratch: every
  planted error reported with file:line.
* `py/mhlib/vudocs.py` built and run against VU-Docs `f3cb9f19`: 279/279 inventory entries confirmed; `ro` flags
  written into `schema/api_inventory.json` (57 read-only properties); inheritance map added from the type pages.
* **2026-10-16, design pass 3 (after three reviews).** Blocking findings addressed: bot `EntryInput.deltaTime` set on
  every input pass and allowed in `bot_assign` (§3.2, §3.9, L08); the ticket budget redone with target respawns, a bot
  bench, a heal policy, per-item forecasts that refuse at queue time, `rr` queue items and W-A tier 1 first (§3.8,
  §6.5-§6.8, §6.12); queue-time validation of sites, so `!mh range` is recorded before `WB:B`/`SA:B` are queued (§3.8,
  RUNBOOK step 10); the W1 rule rebuilt (centre and median scale per model, outlier screen, a bootstrap of the whole
  estimator, simulated critical values and power in `sims/w1crit.py`, secondary agreement, multi-config combination;
  §10.1-§10.2); floats `%.9g` and time from k/hz (§3.5); fire mode read and cycled (`fireLogic`, `EIACycleFireMode`,
  FIRE_N timeout, L30) and the `gs src=poll` fallback for a server without `GunSway:Update` (§3.10, L11); S3 decides
  exponential/linear/instant and maps τ to the 30 Hz gain, T1 lists the VU-patched quantities as not informative (§10).
  Nonblocking findings taken: console `groupName`, `RayCastHit.material` cast and `flagsAndIndex`, `DamageGiverInfo.
  weaponFiring` as data, projectile data type names (`dtype`), weapon slots, bot-only aim calibration, `pcall` probes
  and no yield in callbacks, the 127-char chat cap, weapon aliases, `meta.build`, `-debuglog`, the RCON whitelist rule,
  the admin-action deviation, census chunking, P0.9's three outcomes, P0.13 with 3 bots and early `rr` test of L22
  (P0.15), deploy-screen chat (P0.16), resume hints, setup-instance reparse-point and absolute-path refusals, W5
  cross-checks and per-bone fits, W4 measured-gap bins, W3 minimum samples, unit plumbing (§10.3), S1 suppression
  evidence, M-A hold windows and M-B n-th-attacker timing, the staged `--phase0` gate (§14). The inventory gained 29
  entries (308/308 confirmed against VU-Docs; `vudocs.py` learned kind `dtype`); `rows_v1.json` gained the new fields.
* **2026-10-16, build pass 4 (mock VU and the end-to-end suite).** Built: `py/mockvu` (two `lupa.lua54` runtimes,
  server and client, proxies checked against `api_inventory.json` with a write journal, §13.4 checks into the journal,
  ticks run 10 at a time inside Lua, NetEvents delivered one tick later both ways, client frames fed from per-tick
  server snapshots), the scripted user (`user.py`: Python generators, one yield per chunk; responders answer `prompt`
  rows; it also steps out of the line of fire when the chat says so), `levels.py` (mock Metro and Bazaar layouts) and
  `mockrun.py` with 43 cases (`--list`; suite `phase0` = P0, fault, attended-U, stuck, repeats, log-fault and
  queue-validation cases). No VU process was started; every number below comes from the mock.
* Mod defects the e2e suite found and fixed: shooter bots never reloaded in W-B and S-A, so long series ran dry and
  every later distance was skipped (`L.keep_loaded`: reload before the shot, or respawn at the last spawn spot when the
  reserve is gone; zoom restored); the M-B reset waited forever for a neutral flag under a direct flip (now "taken by the
  other team, not controlled" counts as neutral once a direct flip has been seen, marked `direct-flip`); `Ctx:abort`
  wrote a second `aborted` end mark (the runner's end mark is now the only one); S-A U immune taps and W-C equip order
  in the mock user. Analysis: the W5 `|dist − fxd|` screen was 0.5 m, but a clean hit reads ~0.6 m short (bullet origin
  ahead of the shooter position, hit point on the body surface): now 1.0 m (`FXD_TOL`). P0.8's angle limit is 0.05°
  (bullet drop over the first tick's chord passes). The census counts a member that reads `nil` as present and reports
  how many did (`API n/N (k nil)`); RUNBOOK step 8.2 says so.
* Mock-side fixes (not mod behaviour): the out-of-band row scan restarts at every drain (false "fixture without fx row"
  journal entries before); coverage hooks every new coroutine and starts before the mod loads; a human leaving happens
  inside a server tick even when the empty server then idles (`idle_advance=no`).
* Law pairs the data cannot tell apart, by design of the rule: W4 `recovered` vs `release` look alike when the
  recovery is fast (linear at 8-30 deg/s: t_rec is under the shortest released gap, and every held pair is
  unrecovered at the firing scale); the rule then says `inconclusive` and names both (pass 6; the cases
  `wa_w4_linear_recovered`, `wa_w4_linear_release` and `wa_w4_linear60_recovered` require exactly that, and
  `wa_t1_linear` expects it), the cases that need an identified W4 verdict run with `recovery=exp`; W1 needs two decisive
  configs (§10.2), so W1 cases run `tier=1` (two configs), and one config is `inconclusive` by rule; the W1 critical
  values come from a seeded Monte Carlo (`stats.w1_crit`), so a verdict within its Monte Carlo error of the critical
  value can flip with the seed (the cases sit far from it).
* As built, not as designed: W3's while-firing decay ratio is not computed (the result says so in its notes);
  W2-minrise reads `immediate` only when `da` reaches the moving minimum in the same tick; time is k/hz from the PreSim
  tick counter (P0.3 checks the clock); bot ownership is by player id (`bots.owns`); `extract.py --glob` (default
  `*.log`) selects log files.
* Covered by the e2e suite: every scenario (P0, W-A incl. 60 Hz for T1, S-B, W-B B/U, S-A B/U, M-A incl. a queue with
  `rr` and `nextmap` across two levels, M-B, W-C, W-D client rows) under both law variants of each rule it feeds, bot
  damage off (W-B U, P0 faults), GunSway every frame and every 3rd frame, server GunSway absent, fire mode single/stuck,
  input deltaTime required, stuck and aborted runs, repeated and stray commands, the human leaving, log damage, ticket
  forecast and guard, queue validation, and a lost Lua state. Not covered: the full overnight queue at full scale
  (`--full`, ≈ 0.6 M ticks) was not run in this pass; the default suite runs scaled protocols (`scale`, `parts`).
  Lua line coverage over the suite is reported by `mockrun.py --coverage` (approximate executable lines).
* Everything the mock assumes about VU (§13.8) remains a Phase 0 live check; a shared wrong assumption passes the mock
  and fails live.
* **2026-10-16, build pass 5 (verifier findings).** Extractor: `--session` takes a range or list
  (`20261016..20261017`), because a run id carries its start date and the overnight and morning runs are dated the
  next day; skipped rows of other dates are printed as a WARNING with their dates (§9; RUNBOOK steps 9 and 16). W4:
  no slack below t_rec/t_min (it accepted an unarmed gap one tick short of a whole-tick t_min and fell to a spurious
  `AfterTime`); a threshold fits with j = 0 or 1 tick (shot sees its own or the previous tick); t_rec and t_min are
  counted from the gs row that first shows the shot; both fitting and more than half a tick apart is inconclusive;
  the timer verdict is never given for a step on t_rec/t_min (§10). Runner: a queue or run refusal lists every missing
  prerequisite with its command (level, sites, aim calibration, bot damage), and RUNBOOK gained "R. Re-prepare after a
  new Lua state", used by step 11 (state lost), step 13 (60 Hz) and the crash row; the mock case `runbook_reprepare`
  replays that block from RUNBOOK.md. The PostSim work runs in separate error budgets (`samplers`, `hooks.post`,
  `sites`, `runner`, `inputs.post`) so 20 sampler errors no longer switch off the runner. New e2e cases: W4 spreadmin
  (two) and timer, W1 hit noise, RUNBOOK re-prepare.
* **2026-10-16, build pass 6 (verifier findings).** W4: OnTriggerRelease needs evidence (≥ 4 held units
  classified not armed; before, unclassified held bins left it vacuously consistent and it won); a held pair's
  in-tick recovery is the one seen in the tick before shot 2 (the W3 law, fitted with the trigger released,
  over-corrected it); WhenRecovered/WhenSpreadMin predict a held pair from its own state before shot 2, because
  t_rec and t_min are released-trigger times; a threshold law that agrees with OnTriggerRelease on every observed
  bin makes the verdict `inconclusive` with both named (`numbers.indistinguishable`, no code change). With a fast
  linear recovery protocol c cannot separate them at all; `wa_t1_linear` now expects that. W3-yaw: one-sided
  r > 2/√n on within-curve pairs (`stats.lag1_within`), < 20 pairs inconclusive; the null is biased below 0.
  New e2e cases: W4 linear recovered / release / 60 Hz recovered, W3-yaw null with linear recovery; synthetic
  tests: a W3-yaw null sweep over seeds (none/linear/exp at 30 and 60 Hz) and the W4 variants over seeds.
  Small fixes: a refusal while P0.9 said NO (arbitration) points to `mode=U` instead of repeating `!mh p0 damage`;
  the W5 M98B shape is judged against the end knot the end-knot test chose (a 120 m table read `other` against the
  150 m data knot) and an `other` verdict names the knots and both RMS values; RUNBOOK step 1 says that the last
  check run decides the certificate.
* **2026-10-16, build pass 7 (acceptance finding).** `meta.wpn` held `{wid: {name}}` for every weapon the Lua state
  had seen, so the meta row grew about 40 B per weapon and became a `trunc` row once 5 were known (Phase 0 alone sees
  3; W-B, W-C and W-D add 5 more): every later run of the night would have lost its provenance row. It is now the
  sorted wid list (`nwpn` the count), the names go to `dict kind=wpn` rows re-emitted after each meta row, and
  `emit_meta` keeps the row under the cap (§3.5). The spec of pass 2 (`{wid: {slot, fl}}`) was never built: slot and
  fire logic per weapon are in the `wf` rows (`wid`, `wsl`, `fl`). New e2e case `meta_many_weapons` (phase0 and all suites);
  it fails on the pass-6 code with three `trunc` rows.
