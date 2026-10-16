# VU measurement night: runbook (draft)

**Draft, 2026-10-16.** The commands are final once the harness is implemented on `s5/harness` and
`check_all.py` passes (step 1): it loads the mod in a real Lua 5.4, checks every VU call against the API inventory,
and runs every scenario below end to end in a mock VU. If step 1 fails, stop and tell the agent; do not run VU with a
broken harness. You start VU yourself; the agent never does. Design and background: [DESIGN.md](DESIGN.md).

Times are estimates at 30 Hz. Everything the harness asks you to do also appears in chat as `[mh] ...`.

---

## Checklist (one page)

```
[ ] 1  Checks pass                     python -I ...\check_all.py                         1 min
[ ] 2  Normal VU server NOT running; do not use the "VU Server" Start-menu shortcuts tonight
[ ] 3  Manifest before                 install-manifest.ps1 -Out ...\manifest-before.csv   1 min
[ ] 4  Instance set up                 setup-instance.ps1 (-WhatIf first) -CopyKeyFrom ... 2 min   write down the password
[ ] 5  Server up (30 Hz, unlisted)     vu.exe -serverInstancePath ...ServerMeasure ...     2-4 min "accepting connections"
[ ] 6  Client joined                   console: connect <guid> <password>                  2-3 min chat: "[mh] mharness ... ready"
[ ] 7  Phase 0                         !mh census, p0 tick, wall, p0 aim/bots/fire/damage  30-45 min
[ ] 8  Extraction check                extract.py --dry-run                               2 min
[ ] 9  Attended Session 1              W-B, S-A, W-D, W-C (order below; stop when tired)  60-75 min
[ ] 10 Overnight queue                 !mh flat, !mh wall, !mh queue SB MB MA WA:all nextmap MA
[ ] 11 Leave PC on, client connected, no sleep                                          ~6.5 h
[ ] 12 Morning: 60 Hz repeat           stop, restart with -high60, !mh queue WA:60 SB      5 + 40 min
[ ] 13 Stop safely                     !mh stop, !mh clear, quit client, close server
[ ] 14 Manifest after + compare        install-compare.ps1 -> "install unchanged: N items"
[ ] 15 Extract                         extract.py -> research\measurements\<run>\
[ ] 16 Tell the agent "done"           send nothing else (no logs, no screenshots)
```

---

## 0. Paste these once into PowerShell 7 (`pwsh`)

```powershell
$R = "C:\Users\olku\BF3 reimplementation\wt\s5"
$H = "$R\research\probes\tools\vu-harness"
$S = "$R\research\measurements\session-20261016"
$I = "C:\Users\olku\Documents\Battlefield 3\ServerMeasure"
$VU = "$env:LOCALAPPDATA\VeniceUnleashed\client\vu.exe"
New-Item -ItemType Directory -Force $S | Out-Null
```

## 1. Checks (1 min)

```powershell
python -I "$H\check_all.py"
```
You should see every stage `PASS` and `all checks passed`. Anything else: stop and tell the agent.

## 2. Make sure the normal server is not running

Close any VU server window you have open. Tonight do **not** start the "VU Server 30/60/120 Hz" Start-menu shortcuts:
they use your normal instance (`...\Battlefield 3\Server`), which loads other mods.

## 3. Install manifest before (1 min)

```powershell
pwsh -NoProfile -File "$H\ps\install-manifest.ps1" -Out "$S\manifest-before.csv"
```
Expected: `2101 items -> ...manifest-before.csv` (the count may differ slightly; it only has to match later).

## 4. Set up the measurement instance (2 min)

Dry run first, then for real. `-CopyKeyFrom` copies your existing `server.key` into the new instance (your choice; the
script never prints it). The two instances never run at the same time.

```powershell
pwsh -NoProfile -File "$H\ps\setup-instance.ps1" -Session s1 -WhatIf
pwsh -NoProfile -File "$H\ps\setup-instance.ps1" -Session s1 -CopyKeyFrom "C:\Users\olku\Documents\Battlefield 3\Server\server.key"
```
Expected: a list of written files under `ServerMeasure`, the line `password: <12 characters>` (shown once; it is
also in `ServerMeasure\Admin\Startup.txt`), and the two launch lines. Running it again is safe ("no changes").

## 5. Start the server at 30 Hz (2-4 min)

```powershell
& $VU -serverInstancePath $I -server -dedicated -unlisted
```
Expected in the server window/log, in this order: VU build line, `[VeniceEXT] [mharness] mharness <ver> loaded`,
the backend (Zeus) connection and authentication, `The server is now accepting connections`, and
`mharness: join with the client console: connect <guid> <your password>`.

## 6. Join with the client (2-3 min)

Start the VU client as usual (Start menu "VU Client"). Open the console with `~` and type
`connect <guid> <password>` (the guid from step 5, 32 characters without dashes). Alternative: Win+R,
`vu://join/<guid>/<password>`. If step 5 printed no guid: the VU key portal where you made your server key lists the
key's guid; the server uses the key you copied in step 4.
Expected: Operation Metro loads; in chat `[mh] mharness <ver> ready - !mh help`.

## 7. Phase 0 shakedown (30-45 min)

Spawn as Assault on the US team (team 1) with the default M16A3 and no attachments. Type each command in chat (`J` or your chat key), wait for the `[mh]` reply before the next one.

| # | do | type | you should see |
|---|---|---|---|
| 1 | — | `!mh census` | `[mh] census: api 60/60, assets 14/14, flags 3, tickets 2` (numbers vary) |
| 2 | — | `!mh p0 tick` | after 10 s: `[mh] tick: 30.0 Hz, 300 ticks / 10.0 s, mode regular` |
| 3 | stand 15-30 m from a large flat wall, look at its middle, away from any flag | `!mh wall` | `[mh] wall ok: 24.7 m, incidence 3 deg, flat 0.6 cm` or a reason to pick another wall |
| 4 | keep looking at the wall | `!mh p0 aim` | `[mh] aim: convention <x>, error 0.1 deg` |
| 5 | stay where you are | `!mh p0 bots` | two bots appear in front of you, crouch, go prone, move, vanish; `[mh] bots ok 6/6` |
| 6 | step 3 m to the side | `!mh p0 fire` | a bot fires 10 rounds at the wall; `[mh] fire: shots 10, hits 10, gs 1.0/tick` |
| 7 | — | `!mh p0 damage` | a bot shoots another bot: `[mh] bot damage: YES` or `NO` (write it down) |
| 8 | fire 3 rounds at the wall yourself | `!mh p0 human` | `[mh] human shots seen: yes/no` |
| 9 | — | `!mh p0 round` | `[mh] round: started` or `preround` (see §11 if preround) |
| 10 | — | `!mh p0 report` | one line per step, all `ok` |

## 8. Extraction check (2 min, server keeps running)

```powershell
python -I "$H\extract.py" --session 20261016 --server-logs "$I\logs" --client-logs "$env:LOCALAPPDATA\VeniceUnleashed" --out "$R\research\measurements" --dry-run
```
Expected: `runs: ...-P0-30-...` with row counts, `malformed 0`, `gaps 0`.

## 9. Attended Session 1 (60-75 min; stop whenever you want, in this order)

If step 7.7 said `bot damage: YES`, W-B and S-A can run unattended (`mode=B`, add them to the queue in step 10) and you
can skip to W-D.

Weapons: use them without attachments (iron sights) unless the harness asks for a scope. M16A3, M9 and SMAW are on
the US side, AK-74M and RPG-7 on the RU side (the M98B, 870 and M67 on both). If a weapon the harness asks for is not in
your loadout, switch team (Esc, team) and respawn; if it is still missing, type `!mh next`.

| order | scenario | set up | type | what happens | time |
|---|---|---|---|---|---|
| 1 | W-B falloff, M16A3 + M98B | stand at the start of the longest straight, open line you can find, look along it | `!mh range` then `!mh run WB mode=U w=M16A3,M98B` | the harness asks you to equip the weapon, places a target at 5, 8, ... m and tells you `chest at 40 m (1/3)` / `head ...`; it respawns the target after every hit | 20 min |
| 2 | W-B, more weapons (optional) | same | `!mh run WB mode=U w=AK74M,M9,870` | same | 25 min |
| 3 | S-A health | stay on the range | `!mh run SA mode=U` | it asks for one chest shot at 8 m, then waits 40 s, five times; then the spawn-immunity part (hold fire on the spawn point) | 12 min |
| 4 | W-D client | face the wall from step 7.3 | `!mh run WD` | the harness tells you: 3 full-auto magazines, reloads at empty/half/90 %, then aim down sights with every optic you have | 15 min |
| 5 | W-C explosives (Engineer for RPG/SMAW, Assault for M320, any for M67) | stand 30 m from an open spot | `!mh target 30`, `!mh run WC` | follow the prompts; for the grenade ring use `!mh ring` while looking at the ground | 15 min |

`!mh status` shows progress at any time; `!mh stop` ends the current scenario cleanly.

## 10. Prepare the overnight queue (5 min)

1. Go to Metro's outdoor start area (open sky, flat ground, 30 m straight): look along the flat strip, `!mh flat`.
2. Go back to the wall of step 7.3 (or another big flat wall away from the flags): `!mh wall`.
3. Type `!mh queue SB MB MA WA:all nextmap MA` (add `WB:B SA:B` at the front if bot damage was YES).
4. `!mh queue` shows the items and an estimated end time (about 6.5 h).
5. Move out of every line of fire (the harness pauses if you stand in it). Staying in the spawn screen is fine.

## 11. Overnight

Leave the PC on with the client connected (set Windows sleep to "Never" for tonight yourself if needed). Nothing else
to do. If you check in: `!mh status`.

## 12. Morning: 60 Hz repeat (5 min attended + 40 min)

1. `!mh status` (the queue should say `done`), then `!mh stop` and `!mh clear`.
2. Quit the client (Esc → Quit), close the server window.
3. Start the server at 60 Hz:
   ```powershell
   & $VU -serverInstancePath $I -server -dedicated -unlisted -high60
   ```
4. Join as in step 6, `!mh p0 tick` (expect `60.0 Hz ... high60`), `!mh wall`, `!mh flat`, `!mh queue WA:60 SB`.
5. About 40 min later `!mh status` says `done`.

## 13. Stop safely

`!mh stop` → wait for `[mh] stopped` → `!mh clear` → quit the client → close the server window.

## 14. Manifest after and compare (2 min)

```powershell
pwsh -NoProfile -File "$H\ps\install-manifest.ps1" -Out "$S\manifest-after.csv"
pwsh -NoProfile -File "$H\ps\install-compare.ps1" -Before "$S\manifest-before.csv" -After "$S\manifest-after.csv" | Tee-Object "$S\compare.txt"
```
Expected: `install unchanged: N items` (exit 0). Anything else: keep the files, tell the agent, run nothing more.

## 15. Extract (2-5 min)

```powershell
python -I "$H\extract.py" --session 20261016 --server-logs "$I\logs" --client-logs "$env:LOCALAPPDATA\VeniceUnleashed" --out "$R\research\measurements" --manifest-before "$S\manifest-before.csv" --manifest-after "$S\manifest-after.csv"
```
Expected: one folder per run under `research\measurements\` (`raw.jsonl` or `raw.jsonl.gz`, `meta.json`), `malformed 0`.

## 16. What to send back

Only say "done" to the agent, plus anything that surprised you (one line each, e.g. "bot damage NO", "prompt for M9
never came"). The agent reads `research\measurements\` on this PC (run folders and `session-20261016`). Do not send or
commit the VU log files, screenshots or videos: they hold names and addresses; the run folders hold numbers and small
ids only.

---

## When something fails

| symptom | do |
|---|---|
| Server stops before "accepting connections", log mentions the backend / Zeus / authentication | Zeus is down or unreachable. Wait 10 min and start again. Still down: stop for tonight (nothing in the harness can replace it). |
| Client or server asks for BF3 activation | Run your own "Activate BF3" Start-menu shortcut (it asks for your account in its own window), then start again. |
| VU updates itself at start | Let it finish; server and client must show the same build. The build is recorded per run; carry on. |
| No `[mh]` greeting, no `[mharness]` lines in the server log | Check `ServerMeasure\Admin\ModList.txt` says `mharness` and `Admin\Mods\mharness\mod.json` exists; rerun step 4; restart the server. |
| Chat commands get no answer | Open the client console (`~`) and type `mh status`. If that works, use `mh <command>` instead of chat. |
| `bot damage: NO` | Expected possibility. Run W-B, S-A and W-C with `mode=U` (you shoot). The overnight queue is unaffected. |
| `round: preround` | M-A and M-B need a running round: stop the server, `setup-instance.ps1 -Session s1 -DisablePreRound`, start again, rejoin, redo `!mh wall` / `!mh flat`. |
| A scenario seems stuck | `!mh status` shows the state and its timeout (every wait has one). `!mh next` skips the current config; `!mh stop` ends the scenario. After three skipped configs a scenario aborts itself (`[mh] aborted: <reason>`) and the queue moves on. |
| `[mh] waiting for a human` | You left or the client dropped: rejoin (step 6); the queue resumes by itself. |
| The harness asks for a weapon you do not have | Switch team (US: M16A3, M9, SMAW; RU: AK-74M, RPG-7), respawn; still missing: `!mh next`. |
| Server or client crashed | Restart (steps 5-6), redo the sites (`!mh wall`, `!mh flat`), and resume: `!mh run WA from=<n>` with `<n>` from the last `cfg-done` in `!mh status` before the crash (or ask the agent). |
| `[mh] tickets low, queue paused` | The round is close to ending. `!mh restartround confirm`, wait for the level, redo the sites, `!mh queue resume`. |
| `install-compare` exit 1 | Stop everything; keep both CSVs; tell the agent. |
