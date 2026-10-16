# S5 measurement harness (`mharness`): design

**Date** 2026-10-16. **Branch** `s5/harness` (from main `7b3e8fa`). **Status** design + skeleton. Nothing here has run
in VU yet. The user runs Phase 0 and Session 1 tonight from [RUNBOOK.md](RUNBOOK.md).

**Specification.** `research/topics/gap-r1-measurement-harness.md` (cited as **R** with its section, e.g. R §3.5).
Plan context: `research/PORTING-RESEARCH.md` §3.13, D7, D12, D18, R3, R5, R17 and the oracles in §7.1-§7.3 (cited as
**P**). Consumers of the results: `src/bf3_sim/include/bf3/sim/weapons/mechanics.h` and `docs/design/m2a-weapons.md`
§5.6, §5.9, §6.9 (branch `m2a/weapons`); `src/bf3_sim/include/bf3/sim/soldier_motor.h` `MotorTuning`,
`bf3-opensource/config/motor_tuning.cfg` and `docs/design/m1.md` §6.4, §6.6, §6.8, §6.11 (main).

**Tags.** **[doc]**: the name and signature are in VU-Docs (`C:\Users\olku\VU-Docs`, commit `f3cb9f19`, read
2026-10-16; paths below are relative to `content\vext\ref\` unless they start with `content\`). **[research]**: cited
from R §2 / §3.3 without a re-read today. **[live:Lnn]**: the behaviour is an assumption that Phase 0 check Lnn must
confirm (§8). **[inferred]**: my design choice or reading. VU-Docs is used as an API reference only. No text, type
yaml or example from it is copied into this directory; Lua sources cite the page path in a comment.

---

## 0. Summary

* One VU server mod, `mharness`, in a separate clean instance (`Documents\Battlefield 3\ServerMeasure`), loaded alone.
  It prints one row per tick or event as `MH1 <json>` into the instance log (server rows) and the VU client log
  (client rows). Rows hold numbers, enum values, data-asset names and small integer ids only.
* The mod observes: hooks always pass through; the only writes are to **our own test bots** (create, spawn, place,
  pose, aim, input, kill, remove, seat), each logged as an `fx` row. A static checker enforces this (§12.2).
* Scenarios are coroutines resumed once per sim tick. Each is a state machine with `mark` rows at every phase change,
  timeouts on every wait, and chat progress. `!mh` chat commands start, stop, queue and report them; every command is
  safe to repeat.
* Python tools (`python -I`, numpy only) extract the rows into `research/measurements/<run>/`, validate them against
  `schema/rows_v1.json`, and apply the R §3.6 decision rules. Each verdict names the exact switch or constant it
  changes in our code and drafts a `research/measurements/SOURCES.md` row.
* PowerShell scripts: `install-manifest.ps1` / `install-compare.ps1` (R §4.2, read-only on the install) and
  `setup-instance.ps1` (the user runs it; refuses the normal instance and the install).
* Tonight: Phase 0 (30-45 min attended), then the attended Session 1 items, then an unattended queue overnight
  (S-B, M-B, M-A Metro, W-A, then M-A Bazaar), then a `-high60` repeat in the morning (§6.12).

---

## 1. Hard rules and how each is enforced

| rule | enforcement |
|---|---|
| BF3 install read-only; agents never launch VU or inject | Only the user starts VU (RUNBOOK). `install-manifest.ps1` reads metadata and hashes and refuses an `-Out` that resolves inside the install (full-path + junction resolution, §11.1). `setup-instance.ps1` and `extract.py` refuse any path under the install. Tests use scratch trees; one read-only real-install manifest run prints only its summary. |
| Clean room | No Rime, fun-bots, unfun-bots, IceBloc, AssetBankPlugin, FrostyToolsuite or probe-ant material is opened. Bot fixtures are written from the VU API reference only. `apicheck.py --vudocs` reads VU-Docs at check time to confirm names; nothing from it is stored in the repo except page paths. |
| Observe-only (R §3.2) | `apicheck.py` rejects `MakeWritable`, `RegisterInstanceLoadHandler*`, `RegisterPartitionLoadHandler*`, `MountSuperBundle`, `BeginLoadData`, `AddRegistry`, `hookCtx:Pass`, `hookCtx:Return`, `SetTicketCount`, `vu.SetTeamTicketCount`, `EnableSpawners/DisableSpawners`, `ApplyDamage`, `ApplyImpulse`, `ApplyForce`, `SetProperty`, and any assignment to an engine property outside `mh_bots.lua` (§12.2). Fixture calls are confined to `mh_bots.lua`, and each fixture function must emit an `fx` row. At runtime `mh_bots` refuses any player it did not create. |
| No other mods; vanilla kits | `setup-instance.ps1` writes `ModList.txt` with exactly `mharness`. The `meta` row carries `modList.ListRunning` [live:L05]. Bots use vanilla unlock assets named in `scenarios/wa_cfg.lua`; the names go into `dict` rows. |
| No personal data in rows | The mod never subscribes to `Player:Joining` and never reads `name`, `guid`, `accountGuid`, `onlineId`, `ip` of a human. Humans get ids 1-99 in order of first sight, bots 101+. Bot names are ours (`mh101`). Material and weapon ids are small integers with `dict` rows. The `meta` var list is a whitelist without `vars.gamePassword`. `extract.py` drops and counts any string that looks like a GUID, an IPv4 address or a non-whitelisted key (§9). The server GUID needed to join is printed as a plain log line (not `MH1`), which the extractor never copies. |
| Never touch the normal instance or read `server.key` | `setup-instance.ps1` refuses `...\Battlefield 3\Server` and anything inside it as a target (compared with a trailing separator, so `ServerMeasure` is not confused with `Server`); `-CopyKeyFrom` copies bytes the user points it at and never prints them. `extract.py` refuses the normal instance's `logs`. |
| No downloads or installs | Python stdlib + numpy; PowerShell 7; git. Our own Lua 5.4 syntax checker (§12.1). |
| `python -I`, defensive parsing | Entry scripts add only their own `py\` directory to `sys.path` (isolated mode drops the script directory; verified with Python 3.14.4). Line length cap 64 KiB, JSON depth cap 6, `parse_constant` rejects NaN/Infinity, duplicate keys rejected, every failure counted by reason (§9). |

---

## 2. Files

```
research/probes/tools/vu-harness/
  DESIGN.md  RUNBOOK.md
  check_all.py                     one command: layout, JSON, Lua syntax, API check, unittests, PowerShell tests
  luasyntax.py                     Lua 5.4 lexer + parser (+ scope lint), python -I
  apicheck.py                      static API / observe-only checker over the mod, python -I
  extract.py                       logs -> research/measurements/<run>/raw.jsonl + meta.json
  analyze.py                       decision rules W1-W5, S1, S3, M1, M2, T1 (+ W6, W7, S2 reports)
  synth.py                         synthetic VU logs with known laws (both variants per rule) + malformed lines
  schema/rows_v1.json              row schema (envelope + payload keys/types), shared by Lua checker and extractor
  schema/api_inventory.json        every VU symbol the mod may use: realm, kind, doc page, status, live check
  data/weapon_constants.json       data values the rules compare against (from P §7.1-§7.3 / our dumps), with source
  ps/install-manifest.ps1  ps/install-compare.ps1  ps/setup-instance.ps1
  py/mhlib/                        logparse, schema, runs, stats (KS, bootstrap), fits, rules_*.py, sources
  tests/                           unittest modules; tests/ps/run-ps-tests.ps1; tests/fixtures/ (tiny logs, Lua corpus)
  mod/mharness/                    master copy of the mod (copied into the instance by setup-instance.ps1)
    mod.json
    ext/shared/__init__.lua        requires the shared modules; nothing else
    ext/shared/mh_row.lua          JSON encoder, rounding, envelope, size cap, rate limiter, print
    ext/shared/mh_clock.lua        tick counter k, sim time t, wall ms, run id
    ext/shared/mh_vec.lua          plain-number vector math (no VU operator overloads)
    ext/shared/mh_gs.lua           GunSway -> gs payload (server and client use the same field list)
    ext/server/__init__.lua        wiring: events, hooks, NetEvents, command registration
    ext/server/mh_ids.lua          small integer ids (players, weapons, materials, flags), dict rows
    ext/server/mh_chat.lua         replies and progress through ChatManager
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
    ext/client/mh_cam.lua          camera transform replies for `!mh wall` / `range` / `ring`
research/measurements/             (created by extract.py)
  session-YYYYMMDD/manifest-before.csv manifest-after.csv compare.txt
  <run>/raw.jsonl[.gz] meta.json summary.json sources-draft.md
  SOURCES.md                       one row per accepted finding (written by an agent after review)
```

The mod is research tooling and is never shipped. The skeleton files contain module headers and stubs only.

---

## 3. Runtime architecture of the mod

### 3.1 Realms and loading

* Lua 5.4 with `table string math os coroutine package utf8`; no `io`, `load`, `dofile`, `loadfile`,
  `os.execute/exit/getenv/remove/rename` (`content\vext\_index.md`) [doc]. `require('x')` resolves inside the realm's
  `ext\<realm>` folder, `require('__shared/x')` inside `ext\shared` (`content\vext\guides\code-splitting.md`) [doc].
* The VU log prefix is `[<time>] [info] [VeniceEXT] [<module>] <text>`: verified on the 2026-06-28 client log (module
  name lower-cased folder name). The server log format is assumed identical [live:L01].
* Server mods' client scripts are sent to joining clients ("Received VeniceEXT module" in the client log) [verified],
  so client rows land in `%LOCALAPPDATA%\VeniceUnleashed\vu_*.log`.

### 3.2 Tick pipeline (server)

`UpdateManager:Update(deltaTime, updatePass)` (`shared/event/updatemanager_update.md`; `fb/updatepass.md`) [doc].

| pass | work |
|---|---|
| `UpdatePass_PreSim` (0) | `mh_bots.apply_inputs()`: write each bot's prepared `EntryInput` (fire, zoom, throttle, aim). [live:L08 decides PreSim vs "write at the end of PostSim"; whichever gives the shorter, constant input-to-shot latency is used and recorded in `meta.aimcal.lat`] |
| `UpdatePass_PostSim` (1) | `k += 1`, `t += deltaTime`; samplers (hp, tk, cp, wf) for watched ids; `mh_runner.step()` resumes the active scenario coroutine; limiter bookkeeping |

* Hooks and other events (`GunSway:Update`, `EntityFactory:Create`, `BulletEntity:Collision`, `Soldier:Damage`,
  `Player:*`) emit rows with the current `k` (the tick in progress). Rows inside one tick are ordered by `q`.
* If PostSim does not fire exactly once per sim tick [live:L03], `k`/`t` come from `Engine:Update(dt, simDt)`
  accumulating `simDt` (`shared/event/engine_update.md`) [doc]; `meta.clk` records which source is used.

### 3.3 Time base and run ids

* `k`: sim ticks since the run started; `t`: sum of PostSim `deltaTime` since the run started (s); `ms`:
  `SharedUtils:GetTimeMS()` (`shared/library/sharedutils.md`) [doc]; `q`: per-realm row sequence since mod load (gap
  detection).
* Run id `YYYYMMDD-<SC>-<HZ>-<HHMMSS>` from `os.date` at run start [live:L02] and `GetTickrate()` rounded. R §3.4 says
  `<n>`; the mod cannot persist a counter (no `io`), so the start time makes ids unique across server restarts
  [inferred]. Rows outside a scenario belong to the session pseudo-run `YYYYMMDD-S-<HZ>-<HHMMSS of load>`.
* Client rows carry the server's run id (sent in `mh:ctl`), their own `k` (client frames since `mh:ctl`), `t`, `ms`,
  and `r:"C"`. A ping pair (`mh:ping` / `mh:pong`) records the server-client `ms` offset in a `p0` row [live:L19].

### 3.4 Ids (no personal data)

| id space | rule |
|---|---|
| players | humans 1-99 in order of first sight (event or command), bots 101-999; key = `Player.id` (an engine slot number, never logged). `dict kind=bot` rows give a bot's team, kit and weapon. |
| weapons `wid` | 1.. per distinct `SoldierWeapon.name` / unlock asset name; `dict kind=wpn` gives the data name (e.g. `Weapons/M16A4/U_M16A4`). |
| materials `mat` | `[physicsMaterialIndex, physicsPropertyIndex]` read from the hit's `MaterialContainerPair` (`fb/materialcontainerpair.md`) [doc]: the same index spaces as our grid (P §3.2). No GUID is logged (deviation from R §3.4 `mat` GUID and `dmg.wfire` GUID). |
| flags `cp` | 1.. in iterator order per level; `dict kind=cp` gives `CapturePointEntity.name` (a data label). |
| entities `uid` | `Entity.uniqueId` (`shared/type/entity.md`) [doc]: links `shot` to `hit`. |

### 3.5 Rows, encoder, limits

* `mh_row.row(e, payload, prio)` adds the envelope (§4) and prints `MH1 <json>`. Own JSON encoder (deterministic,
  compact): integers with `%d`, floats with `%.6g` (6 significant digits, R §3.3), Vec3 as `[x,y,z]`, quaternions as
  `[x,y,z,w]`, booleans, labels. Non-finite numbers become `null` and count in `lim.nf`. `json.encode` is not used for
  rows because its float and Vec3 formats are not specified (`content\vext\_index.md` lists types only); Phase 0
  prints one `json.encode(Vec3(1,2,3))` sample for the record [live:L02].
* Size cap 1000 B per row. An oversize row is replaced by `trunc {of, len}` (R §4.6: line limits unknown) [live:L20].
* Rate limiter: token bucket 240 rows/s, burst 480. Priority 1 rows (`meta census dict fx mark site shot hit dmg ev cmd
  err p0 prog lim trunc`) are never dropped; priority 2 (`gs wf hp tk cp veh fov`) are dropped first, oldest class
  `hp` of non-primary ids first. Each second with drops emits `lim {drop:{gs:n,...}}`. Expected peaks: W-A 1 shooter
  gs + wf at 30 Hz ≈ 35 rows/s; M-A tk 30/s + cp ≤ 10/s; W-C ring hp 8 × 30 = 240/s for 2 s (burst).
* Sampling: per-tick only for watched ids (`mh_samplers.watch(id, what, until_k)`), else 1 Hz (`hp`) or on change
  (`cp`, `wf`).

### 3.6 Error containment

Every handler runs through `mh_row.safe(name, fn, ...)` (`pcall`). An error emits `err {where, msg, n}` (message
sanitised to the label character set, ≤ 120 chars). After 20 errors one handler disables itself and the chat says so.
A hook handler that errs before its `Call()` simply returns: VU then runs the original (the engine calls the original
when neither `Call` nor `Return` was used: `content\vext\guides\hooks.md`) [doc], so an error never changes game
behaviour. A scenario coroutine error aborts that scenario (cleanup runs, §3.8) and the queue continues with the next
item.

### 3.7 Commands and chat

* `Player:Chat(player, recipientMask, message)` (`server/event/player_chat.md`) [doc]. Messages starting `!mh` are
  parsed; tokens are `[A-Za-z0-9_.,:=+-]{1,32}`, at most 12. Unknown verbs get a one-line help.
* Replies: `ChatManager:SendMessage(msg, player)` (`server/library/chatmanager.md`) [doc], prefix `[mh]`, ≤ 120 chars
  per message, ≤ 6 messages per reply. Phase changes optionally `Yell(msg, 3, player)`.
* Fallback input path: the client registers a console command `mh` (`client/library/console.md`) [doc] that forwards
  its text by `NetEvents:Send('mh:cmd', text)` (`client/library/netevents.md`) [doc]; the server treats it like chat
  from that player [live:L19].
* Every command emits a `cmd {who, verb, args, ok}` row. The full command reference is §5.

### 3.8 Scheduler (`mh_runner`)

* One active scenario at a time plus a FIFO queue. A scenario is `{id, title, who, run(ctx, args), cleanup(ctx),
  estimate(args)}`. `run` executes as a coroutine resumed once per PostSim tick.
* Context API: `ctx:wait(s)`, `ctx:wait_ticks(n)`, `ctx:wait_until(pred, timeout_s, label)` (returns false on timeout
  and emits `mark lbl=timeout:<label>`), `ctx:mark(lbl, fields)`, `ctx:progress(done, total, cfg)` (a `prog` row and a
  chat line at most every 30 s), `ctx:say(text)`, `ctx:need_site(name)`, `ctx:guard()` (raises `stop` if `!mh stop`,
  round over, level destroy or ticket guard).
* `!mh stop` sets a flag; on the next tick the runner closes the coroutine, runs `cleanup` (release all inputs, remove
  the scenario's bots, unwatch) and emits `mark lbl=stopped`. Repeating `stop` when idle answers "nothing running".
* Ticket guard: before each config and every 10 s, if either team's `TicketManager:GetTicketCount` is below 60 the
  runner pauses the queue and says so (round end would destroy the level and the run) [inferred].
* Level change: `Level:Destroy` (shared) marks every bot soldier invalid and pauses; after `Level:Loaded` + 15 s the
  queue continues if its next item is level-independent or requires the new level (M-A on Bazaar) [live:L22].
* Resume: scenarios take `from=<config index>`; the `mark cfg-done` rows tell which configs finished.

### 3.9 Fixtures (`mh_bots`, the only writer)

| fixture | API (all server) | row |
|---|---|---|
| create | `PlayerManager:CreatePlayer('mh'..id, team, squad)` (`server/library/playermanager.md`) [doc] | `fx act=create id team` |
| equip | `ResourceManager:SearchForDataContainer(name)` (`shared/library/resourcemanager.md`) [doc] for the weapon unlock, kit and soldier blueprint; `player:SelectWeapon(WeaponSlot.WeaponSlot_0, unlock, {})`, `player:SelectUnlockAssets(kitCustomization, {})`, `player.selectedKit = kit` (`server/type/player.md`) [doc] [live:L07] | `fx act=equip id wid` |
| spawn / respawn | `player:CreateSoldier(blueprint, transform)` then `player:SpawnSoldierAt(soldier, transform, pose)` [doc] [live:L07] | `fx act=spawn id pos yaw pose` |
| place | `soldier:SetPosition(Vec3)` / `SetTransform` (`server/type/soldierentity.md`) [doc] [live:L09] | `fx act=place` |
| pose | `soldier:SetPose(pose, immediately, overridePending)` [doc], or pose inputs `EIAChangePose`/`EIAProne` | `fx act=pose` |
| aim / input | own `EntryInput()` object (`shared/type/entryinput.md`) [doc]: `flags = EntryInputFlags.AuthoritativeAiming` (1) (`shared/type/entryinputflags.md`) [doc], `authoritativeAimingYaw/Pitch`, `SetLevel(EntryInputActionEnum.*, level)`; assigned to `player.input` (writable) [doc] each input pass [live:L08] | `fx act=input` at profile start/stop only (not per tick) |
| kill / remove | `soldier:Kill()`; `PlayerManager:DeletePlayer(player)` [doc] [live:L24] | `fx act=kill/remove` |
| seat (S2/S3) | `player:EnterVehicle(vehicle, entryId)`, `ExitVehicle` [doc]; entry from `ControllableEntity:GetDataForEntry` (`server/type/controllableentity.md`) [doc] | `fx act=seat` |

* Aim convention (yaw zero direction, sign, radians vs degrees, pitch sign) is unknown [live:L08]. `!mh p0 aim`
  derives it: the client sends its camera forward (`ClientUtils:GetCameraTransform`) and the server reads the same
  human's `player.input.authoritativeAimingYaw/Pitch`; the candidate mapping (yaw = atan2(±x, ±z) + n·π/2, pitch =
  ±asin(y)) with the smallest error is kept and then verified with five bot shots at the wall. The result goes into
  `meta.aimcal` and every later bot aim uses it.
* Ammo: magazine refills by input (`EIAReload` 1 tick) when the next protocol unit would not fit; a respawn when the
  reserve (`SoldierWeapon.secondaryAmmo` [inferred meaning]) is empty. Writing `primaryAmmo` is forbidden (R §3.2.3).
* Ownership: `mh_bots` keeps the set of `Player.id`s it created; every fixture call checks membership and refuses
  others (an `err` row).

### 3.10 Samplers and hooks (observers)

| observer | source | row | rate |
|---|---|---|---|
| weapon sway | `GunSway:Update(gunSway, weapon, weaponFiring, dt)` (`shared/event/gunsway_update.md`) [doc]; match `weaponFiring.instanceId` against each watched soldier's `weaponsComponent.currentWeapon.weaponFiring.instanceId` (refreshed per tick) | `gs` | every update while watched; `nup` counts updates per tick [live:L11] |
| firing state | watched weapons' `WeaponFiring` fields, compared per tick | `wf` | on change |
| health/kinematics | watched soldiers: `health`, `pose`, `pendingPose`, `transform.trans`, `velocity` (PhysicsEntity) [doc] [live:L10], `isAlive`, `isManDown` | `hp` | per tick when watched, else 1 Hz for bots only |
| tickets | `TicketManager:GetTicketCount(team)` float (`server/library/ticketmanager.md`) [doc]; `ServerTicketCounterEntity` iterator → `TicketCounterEntity(e)`: `team`, `ticketCount`, `controlledAreaValue` (`server/type/ticketcounterentity.md`) [doc] [live:L15] | `tk` | per tick in M-A/M-B, else 1 Hz |
| flags | `ServerCapturePointEntity` iterator → `CapturePointEntity(e)` fields (`server/type/capturepointentity.md`) [doc]; per-team inside counts from `playersInside` | `cp` | per tick for the active flag(s), else on change + 1 Hz |
| shots | hook `EntityFactory:Create(hookCtx, entityData, transform)` (`server/hook/entityfactory_create.md`) [doc]: only if `entityData:Is(...)` is a bullet/missile/grenade data type, call `hookCtx:Call()`, read the created entity's `uniqueId`; attribute to the watched soldier whose eye is within 2 m of `transform.trans` | `shot` | event [live:L12] |
| hits | hook `BulletEntity:Collision(hookCtx, entity, hit, giverInfo)` (`server/hook/bulletentity_collision.md`) [doc]: read only, never `Call`/`Return` | `hit` | event [live:L13] |
| damage | hook `Soldier:Damage(hookCtx, soldier, info, giverInfo)` (`server/hook/soldier_damage.md`) [doc]: `dmg ph=pre` from `info`, then `hookCtx:Call()`, then `dmg ph=post` with `soldier.health` (R §2) | `dmg` | event [live:L14] |
| events | `Player:Killed/Respawn/Reload/SuppressedEnemy/EnteredCapturePoint/ExitedCapturePoint`, `CapturePoint:Captured/Lost`, `Soldier:HealthAction/ManDown`, `Server:RoundOver/RoundReset`, `Level:Loaded`, `Level:Destroy` (`server/event/*.md`, `shared/event/level_destroy.md`) [doc] | `ev` | event |

`EntityFactory:Create` runs for every entity creation, including the level load. The handler tests the data type
first and returns at once for everything else (no `Call`), so non-projectiles pay one type check [inferred].

### 3.11 Client (`ext/client`)

* `mh:ctl` (server → client): `{run, sc, rec:{gs, fov}}` starts or stops client recording. Client rows only while
  recording.
* `mh_cgs`: `GunSway:Update` for the local player's current weapon; `src=pred` when the event's `weaponFiring`
  instance matches `predictedWeaponFiring`, `corr` for `correctedWeaponFiring` (`client/type/soldierweapon.md`) [doc];
  on `fireShot` and every 4th frame otherwise (R §3.3).
* `mh_fov`: on `aimingSimulation.zoomLevel` change (`client/type/aimingsimulation.md`) [doc]: camera transform, project
  points ±5° about the camera's right and up axes at 100 m with `ClientUtils:WorldToScreen`, `GetWindowSize`
  (`client/library/clientutils.md`) [doc]; `fov = 2·atan(tan 5° · (size/2) / offset_px)`; the forward sign is the one
  whose projection lands at the screen centre (R §3.3) [live:L19].
* `mh_cam`: answers `mh:camreq` with `{pos, fwd}` of the camera (forward sign as above) for `!mh wall/range/ring/aim`.

---

## 4. Row schema v1

Machine-readable copy: `schema/rows_v1.json` (the extractor validates against it; `apicheck.py` checks that every
literal `row('<e>', {...})` in the Lua uses only keys declared there). Units: m, m/s, s, degrees unless the API unit is
unknown; then the raw value is logged and named so (`da`, `rec`, `ra`, `rr` are raw until L11 fixes the unit).

**Envelope** (every row): `v` 1 · `run` · `sc` (`P0 WA SB WB SA MA MB WC WD S`, reserved `VA VB VC AA AB AC MC NA`) ·
`r` `S`/`C` · `k` int · `t` s · `ms` int · `q` int · `e` row type.

| `e` | payload (req; *opt*) | notes |
|---|---|---|
| `meta` | `hv` harness version, `lvl`, `mode`, `rnd`, `rpm`, `hz` (GetTickrate), `freq` (`vu.FrequencyMode`), `vars` {whitelisted name: value}, `mods` [names]; *`clk`* (`postsim`/`engine`), *`aimcal`* {yaw, pitch, sign, lat}, *`args`* | one at load (session run) and one at every run start |
| `census` | `types` {entity type name: count}, `api` {symbol: present}, `res` {asset name: found} | P0 |
| `dict` | `kind` (`wpn mat cp bot unlock`), `id`, `name`; *`x`* {team, kit, wid} | before first use of an id |
| `fx` | `act` (`create equip spawn place pose input kill remove seat respawn admin`), `id`, `ok`; *`pos yaw pitch pose team wid prof arg`* | every fixture |
| `mark` | `lbl`; *`cfg i n x`* | phase changes, `cfg-start`, `cfg-done`, `timeout:*`, `stopped` |
| `site` | `name`, `pos`, `ok`; *`n dir d clear flat inc`* | `wall range flat ring` |
| `gs` | `id`, `wid`, `src` (`srv pred corr`), `da`, `dmin`, `rec` [p,y,r,ty], `disp` [p,y], `ra`, `rr`, `seed`, `ifs`, `fs`, `fir`, `tsls`, `mov`, `jmp`, `jt`, `pose`, `zoom`; *`lag` [p,y], `gsf vsf smf chf`, `nup`, `dt`* | R §3.4 names kept |
| `wf` | `id`, `wid`, `ws`, `wsl`, `wsn`, `ttw`, `rt`, `ra` [x,y,z], `ammo`, `ammo2` | `ws` = `WeaponState` value (`shared/type/weaponstate.md`) |
| `shot` | `id`, `wid`, `kind` (`bullet missile grenade other`), `uid`, `pos`, `dir`; *`td`* (data type name) | `dir` = transform forward with the sign from L12 |
| `hit` | `uid`, `pos`, `n`, `bone`, `part`; *`mat` [pmi, ppi], `body` (type name), `shooter`* | |
| `dmg` | `victim`, `ph` (`pre post`), `dmg`, `hp`; *`bone pos org dir bul exp cli cspd dot giver wid dtype dist`* | `dist` = \|pos − org\| |
| `hp` | `id`, `hp`, `pose`, `pos`, `alive`; *`ppose vel md ias`* | `ias` = isAllowedToSpawn (humans, S-A.4 only) |
| `tk` | `tm` [[team, tf, tc, cav], ...] | |
| `cp` | `cp`, `team`, `loc`, `ctrl`; *`att vel atk n1 n2 cd pen1 pen2 mov pre en`* | |
| `fov` | `zoom`, `vfov`, `hfov`, `w`, `h`; *`fovmul sgn`* | client |
| `ev` | `name`; *`id id2 wid pos a`* (`a` = small object of numbers/bools/labels) | |
| `cmd` | `who`, `verb`, `ok`; *`args`* | |
| `prog` | `done`, `total`; *`cfg eta`* | |
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
| `status` | active scenario, config, progress, ETA, queue, bots alive, rows/s, drops, errors | — |
| `census` | `census` row + chat summary (API present n/N, assets found n/N, entity types) | re-emits |
| `p0 <step>` / `p0 all` / `p0 report` | Phase 0 steps (§6.1) | each step re-runnable |
| `wall` / `range` / `flat` / `ring [radii]` | record a site from the caller's camera (needs the client) | overwrites the site, prints the new values |
| `sites` | list recorded sites | — |
| `run <SC> [k=v ...]` | start a scenario now (refused if one is running) | running: "already running X" |
| `queue <item> [<item> ...]` / `queue` / `queue clear` / `queue resume` | append items `SC[:preset]` (`SB`, `MB`, `MA`, `WA:tier1`, `WA:all`, `WA:60`, `WB:B`, `SA:B`, `nextmap`); show with an ETA; clear; resume after a guard pause | duplicates allowed, shown with indices |
| `stop` | stop the active scenario (cleanup) and pause the queue | idle: "nothing running" |
| `pause` / `resume` | hold / continue the active scenario at its next wait | idempotent |
| `next` | skip the current config (marks `cfg-skip`) | idle: no-op |
| `mark <label>` | a `mark` row (user annotations: "ads", "wrong weapon") | — |
| `ok` | confirms a prompt in user-shooter scenarios | ignored when no prompt |
| `target <d>` / `target here` / `untarget` | spawn / remove a target bot (W-B U-mode, W-C) | replaces the previous target |
| `clear` | remove every harness bot | idempotent |
| `watch me` / `unwatch me` | per-tick `hp`/`gs` for the caller | idempotent |
| `rate <gsEvery>` | gs decimation (1 = every update) if Phase 0 shows print pressure | — |
| `nextmap` | `RCON:SendCommand('mapList.runNextRound')` (`server/library/rcon.md`) [doc]; `fx act=admin` | asks for `nextmap confirm` |
| `restartround` | `RCON:SendCommand('mapList.restartRound')`; `fx act=admin` | asks for confirm |

`nextmap` and `restartround` change the round, not game data or measured behaviour; they are logged as admin fixtures.

---

## 6. Scenarios as state machines

Notation: `STATE (duration) → next` with `[timeout → action]`. "Who": **B** bots only (client stays connected,
R §4.4), **U** the user acts. Durations are 30 Hz wall-clock estimates [inferred]; `estimate(args)` prints the same
numbers in chat before a run starts.

### 6.1 P0 Phase 0 shakedown (U, 30-45 min, run id `...-P0-...`)

| step | command | what it does | pass criterion | fallback |
|---|---|---|---|---|
| P0.1 load | (automatic) | session `meta`, greeting in chat on first spawn | user sees `[mh] mharness <ver> ready` | check server log for `[mharness]` lines; L01 |
| P0.2 census | `!mh census` | API presence for every inventory symbol, entity type counts (`TraverseAllEntities`), asset names resolved (`wa_cfg` weapons, kits, `Characters/Soldiers/MpSoldier`), `json.encode` sample, `os.date` | all P0-required symbols present; ≥ 1 capture point and ticket counter; asset misses listed | missing asset: try the alternate names in `wa_cfg.lua`; missing API: the dependent scenario is disabled (status shows it) |
| P0.3 tick | `!mh p0 tick` | 10 s: PostSim count vs `GetTickrate()` and wall ms; `vu.FrequencyMode` | count = 300 ± 3 at 30 Hz | Engine:Update clock (§3.2) |
| P0.4 wall | `!mh wall` | client camera → raycast (`RaycastManager:DetailedRaycast`) → point, normal, distance, incidence; 7 × 7 grid ±1.5 m coplanar within 2 cm; floor clear at 10/12/25 m along the normal | `site wall ok` | pick another wall; L17 |
| P0.5 aim | `!mh p0 aim` | derive the yaw/pitch convention (§3.9) | error < 0.5° | try EIAYaw/EIAPitch levels instead (degrade: W-A aims by turning) |
| P0.6 bots | `!mh p0 bots` | 2 bots (team 1, 2) 5 and 10 m in front; poses stand/crouch/prone 2 s each; SetPosition 2 m; throttle 1 s (velocity non-zero); delete | all `fx ok`; pose rows match; `vel` > 1 m/s while moving | L07/L09/L10/L24 per failing part |
| P0.7 fire | `!mh p0 fire` | shooter bot at wall site (stand, hip) fires 5 single shots then a 5-round burst | 10 `shot` rows, ≥ 9 `hit` rows on the wall plane, `gs` with `fs` once per shot, `nup` = 1 per tick | if no shot: input pass variant, EIAFire level 1 for 2 ticks; L08/L11/L12/L13 |
| P0.8 geometry | (from P0.7 data) | \|hit − ray∩plane\| using the shot pos/dir (drop-corrected); angle(shot dir, origin→hit) | median ≤ 1 cm and ≤ 0.02° → "spawn dir is post-dispersion", "hit pos exact" | analysis uses origin→hit only (W1 does not need the spawn dir) |
| P0.9 damage | `!mh p0 damage` | bot (team 1, M16A3) shoots a team-2 bot at 10 m, chest, 5 shots | ≥ 3 `dmg` pairs with `post.hp < pre.hp` → **bot damage YES** | NO → W-B, S-A, W-C use the user as shooter (U mode); L14 |
| P0.10 human shot | user fires 3 rounds at the wall, then `!mh p0 human` | server `shot`/`hit` rows for the human? | rows present → human shots observable server-side | W-D relies on client rows only; L25 |
| P0.11 cadence/units | (from P0.7) | `nup`, `da` vs data min (M16A3 hip 2.5, ADS 0.2), `rec` scale, ranges of `ra`, `rr`, `seed` | `da` equals the data min within 1 % → degrees | keep raw, W1 uses impacts (R §4.6) |
| P0.12 log | user runs `extract.py` on the live logs | reads the open log, counts rows, `q` gaps, malformed | 0 malformed, 0 gaps; a 950 B test row intact | lower `rate`; extract after the server stops; L20 |
| P0.13 round | `!mh p0 round` | `isInPreRound` of every flag, round state, ticket counts; bot respawn ticket cost (respawn one bot, tk delta) | flags not in preround (or noted) | M-A/M-B need `vu.DisablePreRound true` (RUNBOOK fallback); L21, L16 |
| P0.14 report | `!mh p0 report` | one-line verdict per step in chat + `p0` rows | — | — |

### 6.2 W-A fixed wall (B; R §3.5 #1; items W1-W4, T1)

Requires `site wall` (P0.4) and the aim calibration (P0.5). Shooter: one bot (team 1), stance/zoom per config, at
`wall.pos + wall.n · d` ground-snapped, facing the wall, aiming at the wall point; d = 10 m hip, 25 m ADS (12 m ADS
for protocol d, so the climb stays on the wall) [inferred: radii 0.44-1.23 m hip, 0.09-1.3 m ADS].

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
empty; RELOAD before a unit that does not fit the magazine (EIAReload 1 tick, wait until ammo = full ≤ 6 s);
RESPAWN when the reserve is empty; PAUSE while the user is within 1 m of the line of fire ("step aside").
```
Protocol sizes: **full** N_a 200, N_b 100, G = {0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5} × N_c 20, N_d 10,
N_e 15 + 15 (≈ 25 min per auto config). **short** (non-reference weapons) N_a 100, N_b 50, N_c 10, N_d 5, N_e 10
(≈ 13 min). **bolt** (M98B) A_SINGLE 200 + C_PAIRS with G = {1.5, 2, 3} × 20 (≈ 12 min). M98B stand-hip is dropped
(R §3.5 lists stand-hip; hip spread on a bolt sniper adds nothing to W1 and costs 12 min) [inferred]. If L11 shows
`gs.rec` unusable, C_PAIRS runs as triples (third shot at a fixed 0.1 s) so the impact of shot 3 measures shot 2's
kick (§10 W4).

Config list (`scenarios/wa_cfg.lua`), in queue order: tier 1 = M16A3 stand-ADS, stand-hip (full); tier 2 = M16A3
prone-ADS, crouch-ADS, crouch-hip, prone-hip (full); tier 3 = AK-74M stand-ADS, stand-hip, prone-ADS; M416 stand-ADS,
stand-hip, prone-ADS; M249 stand-ADS, stand-hip, prone-ADS (short); M98B stand-ADS, prone-ADS (bolt).
Estimates: tier 1 ≈ 50 min, tier 1+2 ≈ 2.5 h, all ≈ 5.1 h. Commands: `!mh run WA tier=1` / `tier=2` / `tier=all` /
`cfg=<i>` / `from=<i>`; `!mh queue WA:all`.

Unlock names (resolved and reported by P0.2): `Weapons/M16A4/U_M16A4` (M16A3), `Weapons/AK74M/U_AK74M`,
`Weapons/M416/U_M416` [inferred path], `Weapons/M249/U_M249`, `Weapons/Model98B/U_M98B`, `Weapons/M9/U_M9`,
`Weapons/Remington870/U_870`, `Weapons/RPG7/U_RPG7`, `Weapons/SMAW/U_SMAW`, `Weapons/Gadgets/M320/U_M320_HE`,
`Weapons/M67/U_M67` (lower-case forms in our dumps; VU name lookup case rules [live:L07]).

### 6.3 W-A60 (B; T1)

Same machine on a `-high60` server (run id hz 60): M16A3 stand-hip and stand-ADS with protocol **short** plus the full
gap set G (≈ 25 min). Command `!mh run WA tier=60`.

### 6.4 S-B movement (B; S3, W2 minimum rise, stance transition, T1)

Requires `site flat` (start P, direction u, ≥ 30 m clear, ±5 cm flat; upward clearance measured for drops). Bot
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
`hp` and `gs` per tick throughout. ≈ 12 min. `!mh run SB` (`SB60` is the same on a 60 Hz server).

### 6.5 W-B falloff (U, or B if P0.9 says bot damage works; W5, W6)

Requires `site range` (origin O, horizontal direction u, clear length L by raycast). Distances
D = {5, 8, 10, 15, 20, 25, 30, 40, 50, 60, 75, 100, 125, 150, 175} ∩ (0, L − 2]; skipped distances are marked.
```
WEAPON     for w in args (default M16A3, M98B, AK-74M, M9, 870MCS):
           B: spawn shooter S at O (stand, ADS, w);  U: prompt "equip w, stand on the mark, !mh ok" and wait for the
           human's current weapon name to match (or `ok`)
DIST       for d in D (870MCS: 5-30 only): spawn target T (team 2) at O + u·d facing O; wait 2.5 s (spawn immunity)
ZONE       for z in chest, head: until 3 hits or 30 shots (B) / the user's 3 hits:
             B: aim at T's z point (feet + 1.35 / 1.62 m [inferred], refined after the first hit by bone), FIRE1,
                wait 0.6 s;  U: prompt "z at d m (n/3)"
             on a dmg pair for T: respawn T at full health, wait 2.5 s
PASS       d = 20: T1 at 20 m, T2 at 21 m in line; 5 chest shots at T1; record both
PEN        U only: `!mh target here` behind cover (wood, glass); user shoots through; 5 hits
```
`dmg` pre/post, `shot`, `hit` per shot. U mode ≈ 10 min per weapon; B ≈ 6 min per weapon. `!mh run WB mode=U
w=M16A3,M98B` / `mode=B`.

### 6.6 S-A health and regeneration (B or U; S1, S2, S4)

```
REGEN      5 reps: spawn T at 8 m, wait 2.5 s, one chest hit (25 dmg) by S (B) or the user (U); hold 40 s (hp per tick);
           respawn T
SUPP       3 reps: as REGEN, plus at +4 s S sprays 2 m beside T for 2 s (EIAFire held, aim offset); hold 40 s;
           Player:SuppressedEnemy ev rows
IMMUNE     4 delays {0.5, 1.0, 1.5, 2.5 s} × 3: respawn T, one shot at the delay (B); U mode: the user holds fire at the
           spawn point while T respawns every 6 s, the time since spawn is in each dmg row
RESPAWN    U only, optional: the user suicides from the menu; ias per tick until it flips
```
≈ 9 min B, ≈ 12 min U. `!mh run SA mode=B|U`.

### 6.7 M-A ticket bleed (B; M1)

Levels `MP_Subway ConquestSmall0` and `MP_001 ConquestLarge0` (R §3.5 #6). Fully automatic: flags from the iterator,
HQs = capture points with capture disabled and a team.
```
INIT       find flags and HQs; refuse if any flag isInPreRound (L21); spawn one anchor bot per team at its HQ
PLAN       Metro CQS: 0-0, 1-0, 2-0, 3-0, 2-1, 1-1;  Bazaar CQL: 0-0, 1-0, 2-0, 3-0, 4-0, 5-0, 4-1, 3-2, 2-2
           (the plan starts from the observed state; end state is non-bleeding under both rules)
for each state:
  CHANGE   per flag: CAPTURE(team, 2 bots placed at the flag, until team & controlled) [90 s timeout → retry with
           offsets ±3 m, then skip state]; NEUTRALISE (until team = 0, then remove at once); remove capturers
  HOLD     150 s: tk per tick, cp 1 Hz; mark state-start/state-end with the ownership vector
END        remove all bots
```
Ticket guard (§3.8). ≈ 22 min Metro, ≈ 30 min Bazaar. `!mh run MA`.

### 6.8 M-B capture (B; M2)

One flag F (`flag=<cp id>`, default: the capturable flag farthest from both HQs).
```
RESET      make F neutral (4 bots of the owner's enemy neutralise; remove)
N_SERIES   n in {1,2,3,4,5} × 3: n team-1 bots at F; cp per tick until team 1 controlled [60 s]; remove; RESET
FLIP       F to team 2 (4 bots); n in {1,2,4} × 3: n team-1 bots flip F [90 s]; RESET to team 2
CONTEST    1v1 and 2v1 inside F owned by team 2, 60 s each
LEAVE      3 reps: 1 team-1 bot on neutral F; at loc ≥ 50 % remove it; record 40 s
END        RESET to neutral; remove all bots
```
≈ 25 min. `!mh run MB [flag=<id>]`.

### 6.9 W-C explosives (U; W7)

`!mh target 30` for RPG-7 and SMAW direct hits × 3 each; M320 HE direct × 3 and offset (1-3 m beside, offset from the
detonation position); M67: `!mh ring 0.5,1,2,3,4,5,6,7` places 8 targets around the caller's aim point at those radii
(different azimuths); the user throws at the centre; × 3 (ring respawned). The detonation point is the last
per-tick position of the tracked grenade/missile (shot `kind`), or an explosion entity creation if L12 shows one.
≈ 15 min. `!mh run WC`.

### 6.10 W-D client (U; W8, W9, W10)

`!mh run WD` sends `mh:ctl rec={gs,fov}` to the user's client. The user: 3 full-auto M16A3 magazines at the wall,
reloads at 0 / 50 / 90 % magazine, ADS with every optic they have on the M16A3 and the M98B scope. Server `shot`
(L25) and `wf` rows for the human plus client `gs` (pred/corr) and `fov` rows. ≈ 15 min.

### 6.11 Extension points (Sessions 2-3)

`scenarios/registry.lua` holds `VA VB VC AA AB AC MC NA` entries with `enabled=false`. Shared pieces already in the
design: input profiles `{t0, t1, action, level}` (S-B and later V-A), `mh_bots.seat`, the `veh`/`vdmg` schema rows,
part-transform sampling (`PhysicsEntityBase:GetPartTransform`, `shared/type/physicsentitybase.md`) [doc], and
`MapList` profiles `s2`/`s3` in `setup-instance.ps1`.

### 6.12 Tonight's order and timings

| block | items | who | time |
|---|---|---|---|
| setup | checks, manifest before, setup-instance, launch, join | U | 15 min |
| Phase 0 | P0.1-P0.14 | U | 30-45 min |
| attended | W-B (M16A3, M98B first; AK-74M, M9, 870 if time), S-A, W-D, W-C | U | 60-75 min (B mode: W-B and S-A move to the queue) |
| prepare queue | `!mh flat`, `!mh wall` on Metro; `!mh queue SB MB MA WA:all nextmap MA` | U | 5 min |
| overnight (Metro, then Bazaar) | S-B 12, M-B 25, M-A 22, W-A all ≈ 5.1 h, nextmap, M-A Bazaar 30 | B | ≈ 6.6 h |
| morning | restart with `-high60` (after `!mh stop` and quitting cleanly), `!mh wall`, `!mh flat`, `!mh queue WA:60 SB` | U+B | 5 min + 40 min |
| close | stop server, manifest after, compare, extract | U | 10 min |

Ticket budget on Metro (team 1 = shooter team): deaths only (≈ 10 W-A respawns per full config, ≈ 120 in all) +
M-A/M-B bleed lands on team 2; the M-A plan ends 1-1 so nothing bleeds overnight [inferred; L16 measures the cost of
a respawn].

---

## 7. API facts used (source and confirmation status)

"doc" = present in VU-Docs today with this signature (page relative to `content\vext\ref\`); "research" = R §2 row.
Semantics beyond the name/signature are live checks.

| API | realm | source | status | live |
|---|---|---|---|---|
| Lua 5.4, library whitelist, `require`/`__shared` paths | all | `content\vext\_index.md`; `content\vext\guides\code-splitting.md` | doc | L02 |
| `Events:Subscribe(name, fn)` | shared | `shared/library/events.md` | doc | — |
| `Hooks:Install(name, prio, fn(hookCtx, ...))`; `HookContext:Call` (no `Pass`/`Return`) | shared | `shared/library/hooks.md`; `shared/type/hookcontext.md`; `content\vext\guides\hooks.md` | doc | — |
| `UpdateManager:Update(dt, pass)`; `UpdatePass_PreSim 0`, `PostSim 1` | shared | `shared/event/updatemanager_update.md`; `fb/updatepass.md` | doc | L03, L08 |
| `Engine:Update(dt, simDt)` | shared | `shared/event/engine_update.md` | doc | L03 |
| `SharedUtils:GetTimeMS/GetTickrate/GetLevelName/GetCurrentGameMode/IsServerModule` | shared | `shared/library/sharedutils.md` | doc | L03, L04 |
| `GunSway:Update(gunSway, weapon, weaponFiring, dt)`, `GunSway:UpdateRecoil` | shared | `shared/event/gunsway_update.md`, `gunsway_updaterecoil.md` | doc | L11 |
| `GunSway` fields (`dispersionAngle ... seed initialFireShot fireShot isFiring isMoving isJumping`, `isSupportedShooting` RO) | shared | `shared/type/gunsway.md`; R §2 | doc (types only) | L11 (units, cadence) |
| `GunSwayDeviation {pitch, yaw, roll, transY}` | shared | `shared/type/gunswaydeviation.md` | doc | L11 |
| `WeaponFiring` (`gunSway weaponState lastWeaponState nextWeaponState timeToWait recoilTimer recoilAngleX/Y/Z instanceId`) | shared | `shared/type/weaponfiring.md`; `shared/type/weaponstate.md` | doc | — |
| `EntityManager:GetIterator(type)`, `TraverseAllEntities(fn)`; `EntityIterator:Next` | shared | `shared/library/entitymanager.md`; `shared/type/entityiterator.md` | doc | L15 |
| `entity:Is(typeName)` with engine type names (`ServerCapturePointEntity` ...), cast `CapturePointEntity(e)` | shared | `shared/type/entitybuspeer.md`; `content\vext\guides\entities.md`; `content\vext\entities.md` | doc | L15 |
| `Entity.uniqueId`, `typeInfo.name`, `instanceId` | shared | `shared/type/entity.md`, `entitybuspeer.md`, `typeinformation.md` | doc | — |
| `ResourceManager:SearchForDataContainer(name)` (read only) | shared | `shared/library/resourcemanager.md` | doc | L07 |
| `Vec3(x,y,z)`, `LinearTransform(left, up, forward, trans)`, fields `trans forward up left` | shared | `shared/type/vec3.md`, `lineartransform.md` | doc | — |
| `EntryInput()`, `authoritativeAimingYaw/Pitch`, `flags`, `SetLevel/GetLevel`; `EntryInputFlags.AuthoritativeAiming 1`; `EntryInputActionEnum` (`EIAThrottle 0 EIAStrafe 1 EIABrake 2 EIAYaw 4 EIAPitch 5 EIAFire 7 EIAZoom 11 EIAJump 12 EIAChangePose 13 EIAProne 14 EIAReload 15 EIASprint 32`) | shared | `shared/type/entryinput.md`, `entryinputflags.md`; `fb/entryinputactionenum.md` | doc | L08 |
| `CharacterPoseType_Stand 0 / Crouch 1 / Prone 2`; `WeaponSlot_0`; `TeamNeutral 0 Team1 1 Team2 2`; `SquadNone 0 Squad1 1` | shared | `fb/characterposetype.md`, `weaponslot.md`, `teamid.md`, `squadid.md` | doc | — |
| `RayCastFlags` (`CheckDetailMesh 1 DontCheckWater 4 DontCheckRagdoll 16 DontCheckCharacter 32`); `RayCastHit {position normal material part bone rigidBody}` | shared | `shared/type/raycastflags.md`, `raycasthit.md` | doc | L13, L17 |
| `MaterialContainerPair {physicsMaterialIndex, physicsPropertyIndex}` | shared | `fb/materialcontainerpair.md` | doc | L13 |
| `Level:Loaded(levelName, gameMode, round, roundsPerMap)` (server); `Level:Destroy()` | server / shared | `server/event/level_loaded.md`; `shared/event/level_destroy.md` | doc | L22 |
| `Player:Chat(player, recipientMask, message)` | server | `server/event/player_chat.md` | doc | L06 |
| `ChatManager:SendMessage(msg, player)`, `Yell(msg, s, player)` | server | `server/library/chatmanager.md` | doc | L06 |
| `PlayerManager:CreatePlayer(name, team, squad)`, `DeletePlayer`, `GetPlayers`, `GetPlayerById` | server | `server/library/playermanager.md` | doc | L07, L24 |
| `Player`: `id soldier alive hasSoldier teamId isAllowedToSpawn input selectedKit`; `SelectWeapon`, `SelectUnlockAssets`, `CreateSoldier`, `SpawnSoldierAt`, `EnterVehicle`, `ExitVehicle` | server | `server/type/player.md` | doc | L07, L08 |
| `SoldierEntity`: `health maxHealth pose pendingPose isAlive isManDown worldTransform weaponsComponent player`; `SetPosition SetPose SetTransform Kill`; inherits `PhysicsEntity.velocity` | server | `server/type/soldierentity.md`, `controllableentity.md`, `physicsentity.md` | doc | L09, L10 |
| `SoldierWeaponsComponent.currentWeapon/isZooming`; `SoldierWeapon.weaponFiring/name/primaryAmmo/secondaryAmmo` | server | `server/type/soldierweaponscomponent.md`, `soldierweapon.md` | doc | L11 |
| hook `Soldier:Damage(soldier RO, info, giverInfo RO)`; `DamageInfo` (`damage position origin direction boneIndex isBulletDamage isExplosionDamage isClientDamage collisionSpeed distributeDamageOverTime`); `DamageGiverInfo` (`giver weaponUnlock damageType`) | server | `server/hook/soldier_damage.md`; `server/type/damageinfo.md`, `damagegiverinfo.md` | doc | L14 |
| hook `BulletEntity:Collision(entity RO, hit, giverInfo RO)` | server | `server/hook/bulletentity_collision.md` | doc | L13 |
| hook `EntityFactory:Create(entityData, transform)`; types `ServerBulletEntity ServerMissileEntity ServerGrenadeEntity` | server | `server/hook/entityfactory_create.md`; `content\vext\entities.md` | doc | L12 |
| `TicketManager:GetTicketCount(team)` float, `GetCurrentRound`, `GetRoundCount` | server | `server/library/ticketmanager.md` | doc | L16 |
| `TicketCounterEntity {team ticketCount initialTicketCount controlledAreaValue}` | server | `server/type/ticketcounterentity.md` | doc | L15 |
| `CapturePointEntity` fields (`name playersInside isInPreRound isAttacked attackingTeam isFlagMoving team flagVelocity flagLocation flagHome isControlled isCaptureEnabled cooldownTimer team1/2CooldownPenalty`) | server | `server/type/capturepointentity.md` | doc | L15, L21 |
| events `CapturePoint:Captured/Lost`, `Player:EnteredCapturePoint/ExitedCapturePoint/Killed/Respawn/Reload/SuppressedEnemy`, `Soldier:HealthAction/ManDown`, `Server:RoundOver/RoundReset` | server | `server/event/*.md` | doc | — |
| `RaycastManager:DetailedRaycast(from, to, maxHits, materialFlags, flags)` | server | `server/library/raycastmanager.md` | doc | L17 |
| `RCON:SendCommand(cmd, args) → string{}`, `GetServerGuid()` | server | `server/library/rcon.md` | doc | L04, L05, L18 |
| `NetEvents:Subscribe/SendTo/Broadcast` (server), `Subscribe/Send` (client) | server/client | `server/library/netevents.md`; `client/library/netevents.md` | doc | L19 |
| `Console:Register(name, description, fn)` | client | `client/library/console.md` | doc | L19 |
| `ClientUtils:GetCameraTransform/WorldToScreen/GetWindowSize`; `PlayerManager:GetLocalPlayer` | client | `client/library/clientutils.md`, `playermanager.md` | doc | L19 |
| client `SoldierWeapon.correctedWeaponFiring/predictedWeaponFiring/aimingSimulation`; `AimingSimulation.fovMultiplier/zoomLevel` | client | `client/type/soldierweapon.md`, `aimingsimulation.md` | doc | L19 |
| launch `-server -dedicated -serverInstancePath -high60 -unlisted -noUpdate`; `vu://join/<guid>[/<password>]`; console `connect <guid> [password]` | — | `content\general\args.md`; `content\general\commands.md` | doc | L18 |
| `vu.FrequencyMode` returns `regular` / `high60`; `vu.SuppressionMultiplier` default 100; `vu.TimeScale` 1.0; `vu.DisablePreRound` before level load | — | `content\hosting\commands.md` | doc | L04 |
| instance layout, `MapList.txt` line format, `Startup.txt`, `ModList.txt` | — | `content\hosting\config.md`, `content\hosting\mods.md`; R §3.1 | doc | — |
| BF3 R25 `vars.*` semantics | — | R §3.1 (RCON protocol R25) | research | L05 |

The full list (one entry per symbol, used by `apicheck.py`) is `schema/api_inventory.json`.

---

## 8. Phase 0 live checks and fallbacks

| id | assumption | check (P0 step) | fallback |
|---|---|---|---|
| L01 | mod loads; server log lines `[VeniceEXT] [mharness] ...` | P0.1 | extractor regex accepts any `[VeniceEXT] [mharness]` or bare `MH1 {` line |
| L02 | `os.date`, `string.format('%.6g')`, coroutines in handlers; `json.encode` sample | P0.2 | run id from `ms`; encoder fallback `%.9g` |
| L03 | PostSim once per sim tick on the server | P0.3 | Engine:Update simDt clock |
| L04 | `GetTickrate` 30/60; `RCON:SendCommand` returns `{'OK', value}` | P0.3 | hz from the PostSim rate; meta stores the raw table |
| L05 | `vars.*`, `modList.ListRunning` readable via RCON | P0.2 | values from `Startup.txt` copied in by the user's setup (meta `vars_src=startup`) |
| L06 | `Player:Chat` gives the message; `SendMessage(msg, player)` reaches the player | P0.1/2 | client console `mh ...`; `Yell` |
| L07 | `CreatePlayer` + equip + `CreateSoldier` + `SpawnSoldierAt` give a bot with the weapon; asset names resolve | P0.2, P0.6 | alternate asset names; spawn with the kit's default weapon and log its name |
| L08 | bot input via own `EntryInput` with `AuthoritativeAiming` drives fire/zoom/move/aim; latency constant | P0.5, P0.7 | apply in PostSim instead of PreSim; aim via `EIAYaw/EIAPitch` closed loop; fire 2-tick hold |
| L09 | `SetPose(…, true, true)` immediate; `SetPosition` teleports and zeroes velocity | P0.6 | pose inputs; wait 1 s after teleport |
| L10 | soldier `velocity` populated | P0.6 | finite differences of positions (analysis does both) |
| L11 | `GunSway:Update` server-side for bot weapons, once per tick, `fireShot` once per shot; `da` in degrees | P0.7, P0.11 | `nup` lets the analysis use the last update per tick; W1 from impacts only |
| L12 | `EntityFactory:Create` sees bullet creation; transform = muzzle + post-dispersion direction | P0.7, P0.8 | shots from `gs.fs`; directions from origin→hit |
| L13 | `BulletEntity:Collision` for bot bullets on static geometry; `hit.position` exact; `uniqueId` links | P0.7, P0.8 | ray-plane intersection from shot pos/dir |
| L14 | bot bullets damage soldiers under the shipped arbitration (fun-bots implies not, R §4.4) | P0.9 | U mode for W-B, S-A, W-C |
| L15 | iterators `ServerCapturePointEntity`, `ServerTicketCounterEntity`; casts and fields | P0.2, P0.13 | `TraverseAllEntities` + `Is` |
| L16 | tickets float; respawn ticket cost | P0.13 | budget from `tc` |
| L17 | `DetailedRaycast` hits walls and ground with normals | P0.4 | `CollisionRaycast` |
| L18 | `RCON:GetServerGuid` gives the join GUID (plain log line) | P0.1 | relaunch without `-unlisted`, join through the server browser (`mh-measure`, passworded) |
| L19 | NetEvents both ways; console command; camera forward sign; client log rows; ms offset | P0.4, P0.12 | `!mh wall` from the human's server-side aim (needs L08 convention from bot shots instead) |
| L20 | prints ≤ 1 KB intact; 60 rows/s without loss; live log readable | P0.12 | `rate 2`; extract after shutdown |
| L21 | round leaves preround with 1 human + bots; flags capturable | P0.13 | `vu.DisablePreRound true` in `Startup.txt` for M-A/M-B (RUNBOOK) |
| L22 | Lua state and created players survive a level change | first `nextmap` | queue re-creates bots; user re-queues |
| L23 | the server keeps simulating overnight with the client connected and idle | overnight `prog` rows | — (the user stays connected) |
| L24 | `DeletePlayer` removes a bot cleanly | P0.6 | `Kill` + keep the player |
| L25 | human shots create server bullets | P0.10 | W-D from client rows |

---

## 9. Extractor (`extract.py`)

```
python -I extract.py --session 20261016 \
   --server-logs "C:\Users\olku\Documents\Battlefield 3\ServerMeasure\logs" \
   --client-logs "%LOCALAPPDATA%\VeniceUnleashed" --out <repo>\research\measurements [--since 2026-10-16T18:00]
   [--manifest-before x.csv --manifest-after y.csv] [--force] [--dry-run]
```
* Refuses inputs under `C:\Program Files` and the normal instance (`...\Battlefield 3\Server`, separator-aware), and
  any output inside a log folder. Reads only `vu_*.log` files directly in those folders (no recursion), newest first,
  filtered by the timestamp in the file name.
* Per line: bytes up to 64 KiB (longer: count `too_long`, skip to the next newline); UTF-8 with replacement (count
  `bad_utf8`); regex `\[VeniceEXT\] \[mharness\] MH1 (\{.*\})\s*$` (also bare `^MH1 {` for L01 fallback).
  Other lines are ignored except the first 400 lines of each file, scanned only for `Build (\d+)` and
  `VeniceEXT engine v([\d.]+)`.
* JSON: `json.loads` with `parse_constant` raising (no NaN/Infinity), `object_pairs_hook` rejecting duplicate keys,
  depth ≤ 6, ≤ 64 keys per object. Never `eval`.
* Validation against `schema/rows_v1.json`: envelope types; known `e`; required keys and types; numbers finite;
  labels match the label pattern; **PII guard**: any string matching a GUID (`[0-9a-fA-F]{32}` or 8-4-4-4-12), an IPv4
  address, or any key not in the schema → row dropped, counted `pii_suspect`/`unknown_key`.
* Split by `run`; client rows (`r=C`) join the server run named in their envelope. Per run:
  `raw.jsonl` sorted by (`r`, `q`) (gzip to `raw.jsonl.gz` if > 20 MB), `meta.json` {run, sc, hz, VU build, VeniceEXT
  version, meta rows, counts per `e`, malformed counts by reason, `q` gaps per realm, source log basenames and
  SHA-256, extractor version, session manifests' paths and compare result}. Session manifests live in
  `research/measurements/session-<date>/`.
* Deterministic and idempotent: the same inputs give byte-identical outputs; an existing run folder is skipped unless
  `--force`. Exit 0; 2 if malformed > 1 % of `MH1` lines or any `pii_suspect`.

---

## 10. Analysis rules and their code mapping (`analyze.py`)

`python -I analyze.py <run-dir> [<run-dir60>] [--items W1,W3,...]` prints per item: **verdict**, the numbers, the
implied change, and a draft SOURCES row; writes `summary.json` and `sources-draft.md` into the run folder. Data values
for predictions come from `data/weapon_constants.json` (each with its source). Every rule is tested on synthetic logs
for both variants (§13).

| item | data used | statistic and threshold | verdicts → change in our code | oracle |
|---|---|---|---|---|
| **W1** disc | W-A protocol a (single shots, recoil recovered: `gs.rec` at the shot < 2 % of one kick, else the offset is subtracted); impacts on the wall plane (≤ 5 cm off-plane) | θ_i = angle(origin→hit_i, centroid direction) (constant drop and aim bias cancel). Primary, unit-free: R̂ = max θ (both models), ρ = θ/R̂; KS D against F_A(ρ)=ρ² and F_B(ρ)=ρ with critical values from a 5,000-sample parametric bootstrap with the same R̂ estimator (α = 0.01). Secondary: ρ = θ/`da`(shot) (needs L11 degrees) and two-sample KS of ρ against 100,000 model draws (c = 1.628·√((n+m)/nm)); also `gs.rr` normalised by its max. n ≥ 150 hits per config. | A accepted & B rejected → `uniform_area`; B accepted & A rejected → `uniform_radius`; both rejected → `empirical` (16-knot CDF table: needs a new `DiscDistribution` value, code change); both accepted → `inconclusive` (more shots) → `Mechanics::disc` in `src/bf3_sim/include/bf3/sim/weapons/mechanics.h` (m2a-weapons §5.6, §6.9) | new P §7.1 row "spread disc" (MH) |
| **W2** growth/decay | gs `da` after the last shot (b, e), shot jumps (b); S-B MINRISE | decay slope by least squares over ticks with da > dmin (R² > 0.99); per-shot jump; min rise: da(t) after the moving flag: step within 1 tick vs ramp (slope ≈ decay rate ± 10 %) | slope = data DecreasePerSecond ± 5 % → "data confirmed" (C14 closes, no change); = community 8 ± 5 % → record factor (new field `Mechanics::dispersion_decrease_scale`, code change); min rise → `Mechanics::min_spread_rise` `Immediate` / `AtDecreaseRate`; stance transition (S-B POSE firing) → `Mechanics::stance_transition_penalty` | P §7.1 M16A3 "hip 2.5-7 +0.1" row; P §8 C14 |
| **W3** recovery | gs `rec.p(t)` after bursts (b, d, e); during firing (d) | fit linear-to-zero p(t)=max(0, p0 − a t) and exponential p0·e^(−bt) per curve; the winner needs RMS < 0.5 × the loser's and < 5 % of p0 in ≥ 80 % of curves; while-firing decay ratio vs after; lag-1 autocorrelation of yaw increments (d), \|r\| > 2/√n → correlated | `Mechanics::recovery` `Linear` / `Exponential` with the fitted parameter vs `RecoilAmplitudeDecreaseFactor` (ratio printed; unit inference); ShootingRecoilDecreaseScale 0.1 [inferred in spread_recoil] confirmed or replaced; correlated yaw → new open item for D7 (code change) | P §7.1 M16A3 "dec 18" row |
| **W4** first-shot re-arm | protocol c (pairs, or triples per L11) | per gap: ratio Δp(shot 2)/Δp(shot 1) from `gs.rec` (units cancel); armed if ≈ 1 (± 15 %), not armed if ≈ 1/FSRM (± 15 %); t_rec (rec back < 5 % after one shot) and t_min (da back to dmin) from gs | held (0.075) not armed and every released gap armed → `OnTriggerRelease`; armed iff g ≥ t_rec (within one gap step) → `WhenRecovered`; armed iff g ≥ t_min → `WhenSpreadMin` (new enum value); a step at a g matching neither → `AfterTime(T)` (new enum value) → `Mechanics::first_shot_rearm` | P §7.1 M16A3 "FSRM 2.5" row |
| **W5** falloff | W-B chest `dmg` pre values vs `dist` (chest multiplier 1.0 for 5.56/5.45, 1.25 Sniper, 1.06 .338, P §7.1) | piecewise linear with the data knots (start/end distance, start/end damage): RMS residual < 0.1 hp → `linear` confirmed; else free-knot fit (grid 0.5 m) and report knots; M98B end knot free in [100, 175] → 150 (data) vs 120 (community) by the smaller RMS; basis StraightLine vs PathLength is not separable (< 1 mm, m2a-weapons §5.9) → stays | `Mechanics::falloff` (only `Linear` exists; non-linear → new value + table, code change), `Mechanics::falloff_basis` unchanged | P §7.1 M98B and "shots to kill" rows |
| W6 (report) | PASS, PEN | T2 damage from the same `uid`/time window; ratio to T1 | `Mechanics::bullets_pass_soldiers` true/false (+ the ×0.8 cell); penetration through cover; `apply_protection_threshold` if damage below the threshold is zeroed | P §7.1 |
| W7 (report) | W-C | direct damage RPG/SMAW (225 vs 100), blast dmg vs distance from detonation: linear fit inner/outer radius, RMS | `Mechanics::blast_falloff` (only `Linear`), RPG `Damage` row | P §7.1 RPG, M320/M67 rows |
| **S1** regen | S-A REGEN/SUPP `hp` per tick | onset = first tick with hp increase − time of the last damage; rate = slope over the rise | onset 10 ± 0.5 s and 5 ± 0.25 hp/s → data confirmed (`SoldierDef.regeneration_delay/rate`, `bf3_core/gameplay/soldier_def.h`, used by the M2b health module); onset 16 ± 0.5 s → community value: a second timer (suppression) must be modelled (code change in the health module); SUPP onset relative to the last suppression event → `SuppressionAbortsHealthRegeneration` semantics | P §7.2 health row; P §8 C14 |
| S2 (report) | S-A IMMUNE | largest delay with pre dmg > 0 and hp unchanged | `Mechanics::spawn_protection_blocks`; `ImmortalTimeAfterSpawn 2` | P §7.2 |
| **S3** motor | S-B `hp.vel` (and position differences) | fit v(t) after a throttle step: FractionPerReferenceTick (gain g at 30 Hz) vs Linear (constant a) vs first-order lag in continuous time; winner by RMS (< 0.5 × the others); decel likewise; free fall: quadratic fit of y(t) → g (± 0.05); terminal speeds; fall damage vs impact speed table; jump apex | `bf3-opensource/config/motor_tuning.cfg` keys `acceleration_model`, `deceleration_model`, `sprint_model`, `gravity`, `air_control_scale`, `reference_hz` (m1.md §6.4, §6.6, §6.11); fall-damage curve vs `CollisionData` (m1.md §6.8) | P §7.2 speeds, jump/fall rows |
| **M1** bleed | M-A `tk.tf` per state hold | rate per state by least squares (tickets/min, 95 % CI); rule A predicts 15/min iff enemy AreaValue sum > 100 and > own, else 0; rule B: rate grows with the lead (fit rate = 15 × f(lead), f linear and step candidates) | all states within ± 1 ticket/min of A → `rule A`; else `rule B` with the fitted f → the D12 bleed function (planned M4; P §3.7 "Bleed rule"; no code yet) | P §7.3 conquest row |
| **M2** capture | M-B `cp.loc` per tick | T(n) = time from first attacker inside to controlled; model T = 20 / min(1 + 0.34(n−1), 4) (20.0, 14.9, 11.9, 9.9, 8.5 s); flip = lose + get (40 s at n = 1); contested: loc slope 0; leave: return/decay rate | all T(n) within ± 5 % → `model holds`; else fit the per-player multiplier and flip rule → the D12 capture function (planned M4; uses `CapturePointDef` data in `bf3_core/gameplay/mode_def.h`) | P §7.3 conquest row |
| **T1** tick coupling | the same quantities from a 30 Hz and a 60 Hz run | ratio 60/30 per quantity (W2 slope, W3 parameter, W4 re-arm time, S3 gains, gravity); equal if \|ratio − 1\| < max(3·SE, 5 %) | equal → per-second rates reproduce BF3 (C5 holds, D4 stands); different → list the tick-coupled quantities: our 60 Hz sim must convert them (`MotorTuning::reference_hz`; weapons recovery "per whole tick", m2a-weapons §5.6) | P §4 D4 |

**SOURCES.md row** (draft, one per finding):
`| H:<item> | <quantity> | <value> | <unit> | <run id> | VU <build> | <Hz> | n=<n> | <statistic> | <consumer: file
field> | <oracle row> |`. `analyze.py` writes drafts; an agent reviews them before they go into
`research/measurements/SOURCES.md`, and `Mechanics::measured` becomes true only when all of W1-W5 have rows.

---

## 11. PowerShell scripts

### 11.1 `install-manifest.ps1 -Out <csv> [-Root <install>]` and `install-compare.ps1 -Before -After`

* Per item (root included): `Path` (relative), `Dir`, `Length`, `LastWriteUtc`, `CreationUtc`, `Sha256` for `.toc .cat
  .mft .json .xml .exe .dll` and `initfs_Win32` (R §4.2). Never `LastAccessTime`.
* `-Out` check: `[IO.Path]::GetFullPath`, then the deepest existing ancestor resolved through `LinkTarget` (junctions,
  symlinks); refused if equal to or under the install root (separator-aware), or under the normal VU instance.
  Read errors are fatal (no silent gaps).
* Compare: exit 0 with `install unchanged: N items`, else exit 1 with the differing rows (added, removed, changed
  fields). Also exits 1 on a header mismatch or an empty manifest.
* Tests: scratch tree (files with each hashed extension, an `initfs_Win32`, nested dirs); a negative control (touch,
  content change, add, delete) must give exit 1; `-Out` inside the scratch "install", via `..`, and via a junction must
  be refused. Once read-only against the real install with `-Out` in scratch, printing only the summary line.

### 11.2 `setup-instance.ps1` (the user runs it)

```
pwsh -NoProfile -File setup-instance.ps1 [-Target <dir>] [-Session p0|s1|s2|s3] [-Password <pw>] [-NewPassword]
     [-CopyKeyFrom <path>] [-DisablePreRound] [-WhatIf]
```
* Default target `<MyDocuments>\Battlefield 3\ServerMeasure`. Refuses: the normal instance `...\Battlefield 3\Server`
  or anything under it (separator-aware), the BF3 install, `%LOCALAPPDATA%\VeniceUnleashed`, the repo itself.
* Writes `Admin\ModList.txt` (exactly `mharness`), `Admin\MapList.txt` per session (`s1`: `MP_Subway ConquestSmall0 1`
  then `MP_001 ConquestLarge0 1`; `p0` = `s1`; `s2`/`s3` placeholders for later sessions), `Admin\Startup.txt` (neutral
  set of R §3.1: `vars.serverName "mh-measure"`, `vars.gamePassword`, `vars.regenerateHealth true`, `vars.soldierHealth
  100`, `vars.bulletDamage 100`, `vars.playerRespawnTime 100`, `vars.gameModeCounter 100`, `vars.vehicleSpawnAllowed
  true`, `vars.vehicleSpawnDelay 100`, `vars.friendlyFire false`, `vars.autoBalance false`, `vars.idleTimeout 0`,
  `vu.SuppressionMultiplier 100`, `vu.TimeScale 1.0`; `vu.DisablePreRound true` only with `-DisablePreRound`), and
  mirrors `mod\mharness` into `Admin\Mods\mharness` (stale files removed only inside that folder).
* Password: generated locally (`RandomNumberGenerator`, 12 chars `[a-z0-9]`, URL-safe for `vu://join`), shown once;
  later runs keep the password found in the target's own `Startup.txt` unless `-Password`/`-NewPassword`.
* `-CopyKeyFrom`: copies the file bytes to `<target>\server.key` (never printed, never hashed to output; size check
  only: 1-4096 bytes). Without it the script reminds the user that a key is needed.
* Idempotent (second run: "no changes" except the mod mirror), `-WhatIf` via `SupportsShouldProcess` prints every
  intended write. Prints the launch lines for 30 Hz and 60 Hz.
* Tests: `-WhatIf` (no file written), a scratch target (twice: idempotent), refusals (normal-instance path, its
  subfolder, a path that merely starts with "Server", the install, a junction into the normal instance), a fake key
  copied byte-exact.

---

## 12. Static checks of the mod (no Lua interpreter)

### 12.1 `luasyntax.py` (Lua 5.4)

Lexer: names, all keywords incl. `goto`, numerals (decimal, hex, hex floats with `p` exponents, `1e-3`), short
strings with every escape (`\a \b \f \n \r \t \v \\ \" \' \z \xXX \ddd \u{XXX}`, line continuation), long
strings/comments `[==[ ]==]` of any level, operators incl. `// & | ~ << >> ::`. Parser: full 5.4 grammar
(attribs `<const>`/`<close>`, labels, `goto`, varargs, method calls, string/table call sugar, operator precedence and
right-associativity of `^` and `..`). Semantic checks: `break` outside a loop, `goto` to an invisible label,
duplicate labels in a block, `...` outside a vararg function, assignment to a `<const>` local. Scope lint: every free
name must be a Lua 5.4 global of the allowed libraries, a VU global from `api_inventory.json`, or the mod's one global
table `MH`; anything else is an error (catches typos and `io`/`load`/`debug`). Tests: a corpus of valid and invalid
snippets in `tests/fixtures/lua/` with expected error lines.

### 12.2 `apicheck.py`

Over every `.lua` in the mod, with the realm from the path (`ext/server` → server + shared, `ext/client` → client +
shared, `ext/shared` → shared):
1. `Events:Subscribe('<name>'` and `Hooks:Install('<name>'` names exist for the realm; NetEvent names are ours
   (`mh:*`).
2. `Lib:Method(` for inventory libraries: the method exists in that realm.
3. `Enum.Value`: the value exists.
4. `x.field` / `x:method(` on non-mod tables: the name is a member of some inventory type (coarse, catches typos).
5. Forbidden calls and names (§1 row "Observe-only"), forbidden strings (`gamePassword`, `accountGuid`, `onlineId`,
   `.ip`, `.guid` of a player).
6. Engine property assignment outside `mh_bots.lua` → error; inside it only `input`, EntryInput fields and
   `selectedKit`.
7. Fixture calls only in `mh_bots.lua`, and the enclosing function also calls `fx(`.
8. `print(` only in `mh_row.lua`.
9. Literal `row('<e>', { k = ... })`: keys ⊆ schema keys of `<e>`.
10. `require` paths resolve to files of the mod.
11. `--vudocs <dir>` (optional, read-only): every inventory entry marked `doc` has its page and its name in the page;
    prints `confirmed n/N` and fails on a mismatch. Nothing is copied.

---

## 13. Test plan

| suite | content |
|---|---|
| `tests/test_luasyntax.py` | corpus valid/invalid; the whole mod parses with 0 errors |
| `tests/test_apicheck.py` | each rule with a positive and a negative snippet; the whole mod with 0 errors |
| `tests/test_extract.py` | tiny fixture logs (server + client format, header lines); malformed: truncated JSON, 100 KiB line, NaN, invalid UTF-8, duplicate keys, deep nesting, missing envelope keys, wrong types, GUID/IP strings, unknown keys, unknown `e`; idempotence (byte-identical rerun); refusals (install path, normal instance); run split incl. client rows; gzip threshold (synthetic large run) |
| `tests/test_synth_rules.py` | `synth.py` generates W-A/S-B/W-B/S-A/M-A/M-B logs for **both** variants of each rule (disc area/radius; recovery linear/exp; re-arm release/recovered/timer; decay data/community; falloff linear/curved; regen 10 s/16 s; motor fraction/linear, g 9.81/other; bleed A/B; capture model/alt; 30 vs 60 Hz equal/coupled) with seeds; `extract` → `analyze` must return the generating variant; inconclusive when n is too small (W1 n = 20) |
| `tests/test_stats.py` | KS statistic and p-value against known values; bootstrap critical value reproducible with a seed; fits on exact synthetic curves |
| `tests/ps/run-ps-tests.ps1` | §11.1 and §11.2 tests on scratch trees under the scratchpad |
| `check_all.py` | runs all of the above in order (`--with-install` adds the read-only real-install manifest, summary only); exit 0 only if everything passes |

---

## 14. Risks and open questions

* Logs: W-A at 30 Hz ≈ 35 rows/s × ~300 B ≈ 38 MB/h raw, ≈ 140-200 MB for `WA:all`, ≈ 15-25 MB gzipped. Whether runs
  this size go into git (or only `meta.json` + `summary.json` with the raw file kept locally and hashed) is for the
  user to decide.
* `data/weapon_constants.json` holds only values that are in P §7.1-§7.3; DecreasePerSecond per stance and the M416
  values are not there and must be filled from our own `bf3dump` (read-only on the install) before W2 compares.
* The W-A wall must be flat over ~3 × 3 m with ~26 m of clear, flat floor in front, and away from flags (bleed). The
  drop tests need open sky (Metro's outdoor start area).
* If the round ends overnight (ticket guard misses something, or Zeus drops the server), the queue stops; `from=`
  resumes the next evening.
* Everything marked [live:Lnn] is unconfirmed until Phase 0 tonight.

## 15. As-built notes

(empty until implementation; implementers record deviations from this design here)
