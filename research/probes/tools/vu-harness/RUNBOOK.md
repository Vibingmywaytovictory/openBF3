# VU measurement night: runbook (draft)

**Draft, 2026-10-16 (pass 3).** The commands are final once the harness is implemented on `s5/harness` and the checks
of step 1 pass: they load the mod in a real Lua 5.4, check every VU call against the API inventory, and run every
scenario below end to end in a mock VU. If step 1 fails, stop and tell the agent; do not run VU with a broken harness.
You start VU yourself; the agent never does. Design and background: [DESIGN.md](DESIGN.md).

Times are estimates at 30 Hz. Everything the harness asks you to do also appears in chat as `[mh] ...`.

The harness only controls its own test bots, with one exception you control: it restarts the round or changes the map
only when you type `!mh restartround confirm` / `!mh nextmap confirm` or put `rr` / `nextmap` in your queue.

---

## Checklist (one page)

```
[ ] 1  Checks pass                     python -I ...\check_all.py   (or --phase0, see step 1)    1-3 min
[ ] 2  Normal VU server NOT running; no "VU Server" Start-menu shortcuts tonight
[ ] 3  PC ready for the night          Windows Update paused 1 day; sleep "Never"                2 min
[ ] 4  Manifest before                 install-manifest.ps1 -Out ...\manifest-before.csv          1 min
[ ] 5  Instance set up                 setup-instance.ps1 (-WhatIf first) -CopyKeyFrom ...        2 min  note the password
[ ] 6  Server up (30 Hz, unlisted)     vu.exe -serverInstancePath ...ServerMeasure ...            2-4 min "accepting connections"
[ ] 7  Client joined                   console: connect <guid> <password>                         2-3 min "[mh] mharness ... ready"
[ ] 8  Phase 0                         deploy-screen chat, census, p0 round, p0 level, tick,     30-45 min (+10 if preround)
                                       wall, aim, bots, fire, damage, human, report
[ ] 9  Extraction check                extract.py --dry-run                                      2 min
[ ] 10 Attended Session 1              W-B chest pass, S-A, W-D, W-C (stop when tired)           75-90 min
[ ] 11 Overnight queue                 !mh flat, !mh wall, [!mh range], !mh queue rr WA:tier1 ...  5 min
[ ] 12 Leave PC on, client connected                                                              ~7 h
[ ] 13 Morning: 60 Hz repeat           stop, restart with -high60, !mh queue WA:60 SB             5 + 40 min
[ ] 14 Stop safely                     !mh stop, !mh clear, quit client, close server
[ ] 15 Manifest after + compare        install-compare.ps1 -> "install unchanged: N items"
[ ] 16 Extract                         extract.py -> research\measurements\<run>\
[ ] 17 Restore your VU loadout; tell the agent "done" (send nothing else)
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

## 1. Checks (5-10 min; --phase0 about 3 min)

```powershell
python -I "$H\check_all.py"
```
Every stage `PASS` and `all checks passed`: everything below may run. The checks also read the BF3 install once
(file sizes, dates and hashes only, never a write; the `powershell` line shows the item count).

If the agent tells you the harness is only partly finished tonight, run instead:
```powershell
python -I "$H\check_all.py" --phase0
```
`phase0 gate passed`: do Phase 0 (step 8) and the attended items (step 10) only; the harness itself refuses the
overnight queue (`[mh] ... not certified tonight`). Anything else: stop and tell the agent.

## 2. Make sure the normal server is not running

Close any VU server window you have open. Tonight do **not** start the "VU Server 30/60/120 Hz" Start-menu shortcuts:
they use your normal instance (`...\Battlefield 3\Server`), which loads other mods.

## 3. Prepare the PC for the night (2 min, your own settings)

Settings → Windows Update → "Pause updates" for 1 day (an automatic restart would kill the overnight run). Settings →
System → Power → screen and sleep: "Never" when plugged in. Undo both tomorrow if you like.

## 4. Install manifest before (1 min)

```powershell
pwsh -NoProfile -File "$H\ps\install-manifest.ps1" -Out "$S\manifest-before.csv"
```
Expected: `2101 items -> ...manifest-before.csv` (the count may differ slightly; it only has to match later).

## 5. Set up the measurement instance (2 min)

Dry run first, then for real. `-CopyKeyFrom` copies your existing `server.key` into the new instance (your choice; the
script never prints it). The two instances never run at the same time.

```powershell
pwsh -NoProfile -File "$H\ps\setup-instance.ps1" -Session s1 -WhatIf
pwsh -NoProfile -File "$H\ps\setup-instance.ps1" -Session s1 -CopyKeyFrom "C:\Users\olku\Documents\Battlefield 3\Server\server.key"
```
Expected: a list of written files under `ServerMeasure`, the line `password: <12 characters>` (shown once; it is
also in `ServerMeasure\Admin\Startup.txt`), and the two launch lines. Running it again is safe ("no changes").

## 6. Start the server at 30 Hz (2-4 min)

```powershell
& $VU -serverInstancePath $I -server -dedicated -unlisted
```
Expected in the server window/log, in this order: VU build line, `[VeniceEXT] [mharness] mharness <ver> loaded`,
the backend (Zeus) connection and authentication, `The server is now accepting connections`, and
`mharness: join with the client console: connect <guid> <your password>`.

## 7. Join with the client (2-3 min)

Start the VU client as usual (Start menu "VU Client"). Open the console with `~` and type
`connect <guid> <password>` (the guid from step 6, 32 characters without dashes). Alternative: Win+R,
`vu://join/<guid>/<password>`. No guid printed: see "When something fails".
Expected: Operation Metro loads, you are on the deploy screen. **Do not spawn yet.**

## 8. Phase 0 shakedown (30-45 min)

Type each command in chat (`J` or your chat key) and wait for the `[mh]` reply before the next one.

| # | do | type | you should see |
|---|---|---|---|
| 1 | stay on the deploy screen | `!mh help` | a short help in chat (chat works from the deploy screen; write down if not) |
| 2 | spawn as Assault on the US team (team 1), default M16A3, no attachments | `!mh census` | `[mh] census: API 180/180 (2 nil), assets 14/14, entity types 6, flags 5, ticket counters 2, vm same, console mharness.mh` (numbers vary; "nil" members exist but were empty, which is fine; an API count below the total: write the line down) |
| 3 | — | `!mh p0 round` | three bots appear on their bases; after up to 60 s `[mh] round: started, tickets per spawn 1` or `[mh] round: preround` |
| 3a | only if step 3 said `preround` (likely) | — | do "round: preround" in the failure table now (restart with `-DisablePreRound`, about 10 min), then redo steps 1-3 of this table |
| 4 | — | `!mh p0 level confirm` | the round restarts (loading screen); then `[mh] level: state kept`. If instead the greeting `[mh] mharness ... ready` comes again and no `level:` line follows, the Lua state was lost: write down `state lost` (it changes step 11); the 2 bots of this step stay on the server as orphans (`!mh clear` no longer knows them; they are harmless, a server restart removes them) |
| 5 | — | `!mh p0 tick` | after 10 s: `[mh] tick: 30.0 Hz, 300 ticks / 10.0 s, mode regular` |
| 6 | stand 15-30 m from a large flat wall, look at its middle, away from any flag | `!mh wall` | `[mh] wall ok: 24.7 m, incidence 3 deg, flat 0.6 cm` or a reason to pick another wall |
| 7 | anywhere | `!mh p0 aim` | a bot turns and fires a few shots at the ground; `[mh] aim: convention <x>, error 0.1 deg` |
| 8 | stay where you are | `!mh p0 bots` | two bots appear in front of you, crouch, go prone, move, switch to a pistol, vanish; `[mh] bots ok 7/7` |
| 9 | step 3 m to the side | `!mh p0 fire` | a bot fires 10 rounds at the wall; `[mh] fire: shots 10, hits 10, auto yes, gs 1.0/tick` |
| 10 | — | `!mh p0 damage` | a bot shoots another bot: `[mh] bot damage: YES`, `NO (arbitration)` or `NO (aim)` (write it down) |
| 11 | face the wall | `!mh p0 human`, then within 30 s fire 3 rounds at the wall | `[mh] fire 3 rounds at a wall now (30 s)`, then `[mh] human shots seen: true (shots 3, hits 3)` |
| 12 | — | `!mh p0 log`, then step 9 below (extraction check) | `[mh] log: test row written; run extract.py --dry-run (RUNBOOK step 9)` |
| 13 | — | `!mh p0 report` | one line per step, all `ok`, and the queue line to use in step 11 |

## 9. Extraction check (2 min, server keeps running)

```powershell
python -I "$H\extract.py" --session 20261016 --server-logs "$I\logs" --client-logs "$env:LOCALAPPDATA\VeniceUnleashed" --out "$R\research\measurements" --dry-run
```
Expected: `runs: ...-P0-30-...` with row counts, `malformed 0`, `q gaps 0`, `trunc 0`. No runs found: list the log
folder; if the files do not end in `.log`, add `--glob "*.txt"` (or the pattern you see).

## 10. Attended Session 1 (75-90 min; stop whenever you want, in this order)

If step 8.10 said `bot damage: YES`, still do W-B for the M16A3 here (the comparison needs it once by hand), then skip
the M98B and S-A: they run overnight in `mode=B`.

Weapons: use them without attachments (iron sights) unless the harness asks for a scope. M16A3, M9 and SMAW are on
the US side, AK-74M and RPG-7 on the RU side (the M98B, 870 and M67 on both). If a weapon the harness asks for is not in
your loadout, switch team (Esc, team) and respawn; if it is still missing, type `!mh next`.

| order | scenario | set up | type | what happens | time |
|---|---|---|---|---|---|
| 1 | W-B falloff, chest pass, M16A3 + M98B | stand at the start of the longest straight, open line you can find (ideally 160 m or more), look along it | `!mh range` then `!mh run WB mode=U w=M16A3,M98B` | the harness asks you to equip the weapon, places a target at 5, 8, ... m and asks `chest at 40 m (1/3)`; aim at the middle of the chest; the target is replaced only when the next hit could kill it | 30 min |
| 2 | W-B head pass (optional) | same | `!mh run WB mode=U w=M16A3,M98B zones=head` | the same with `head at ...` | 20 min |
| 3 | W-B, more weapons (optional) | same | `!mh run WB mode=U w=AK74M,M9,870` | same (switch to RU for the AK-74M) | 30 min |
| 4 | S-A health | stay on the range | `!mh run SA mode=U` | five times: one chest shot at 8 m, then wait 40 s; three times the same plus, when told, a burst about 2 m to the side of the target (do not hit it); then 60 s of **continuous fire at the target's spawn point** while it respawns every 6 s | 12 min |
| 5 | W-D client | face the wall from step 8.6 | `!mh run WD` | the harness tells you: 3 full-auto magazines, reloads at empty/half/90 %, then aim down sights with every optic you have (this changes your saved loadout: restore it tomorrow) | 15 min |
| 6 | W-C explosives (Engineer for RPG/SMAW, Assault for M320, any for M67) | stand 30 m from an open spot | `!mh target 30`, `!mh run WC` | follow the prompts; for the grenade ring use `!mh ring` while looking at the ground | 15 min |

`!mh status` shows progress and both teams' tickets at any time; `!mh stop` ends the current scenario cleanly. If a
`!mh run` answers `refused: tickets`, type `!mh restartround confirm`, wait for the level (your sites stay valid), and
run it again.

## 11. Prepare the overnight queue (5 min)

1. Go to Metro's outdoor start area (open sky, flat ground, 30 m straight): look along the flat strip, `!mh flat`.
2. Go back to the wall of step 8.6 (or another big flat wall away from the flags): `!mh wall`.
3. Only if bot damage was `YES`: go to the range start of step 10.1, look along it, `!mh range`.
4. Type the queue line that `!mh p0 report` printed. Normally:
   * bot damage NO: `!mh queue rr WA:tier1 SB MB WA:rest rr MA nextmap MA`
   * bot damage YES: `!mh queue rr WA:tier1 WB:B SA:B SB MB WA:rest rr MA nextmap MA`
   * step 8.4 said `state lost`: first `!mh restartround confirm`, wait for the level, redo 1-3 above, then
     `!mh queue WA:tier1 SB MB WA:rest` (no `rr`, no `nextmap`, no M-A tonight)
5. The reply lists every item with its end time and both teams' tickets after it (about 7 h in all). If it answers
   `refused: <item> <reason>`, nothing was queued: fix the reason (usually a missing site) and type the line again.
6. Move out of every line of fire (the harness pauses if you stand in it). Go back to the deploy screen (if step 8.1
   worked there) or stand in a quiet corner.

## 12. Overnight

Leave the PC on with the client connected. Nothing else to do. If you check in: `!mh status`.

## 13. Morning: 60 Hz repeat (5 min attended + 40 min)

1. `!mh status` (the queue should say `done`), then `!mh stop` and `!mh clear`.
2. Quit the client (Esc → Quit), close the server window.
3. Start the server at 60 Hz:
   ```powershell
   & $VU -serverInstancePath $I -server -dedicated -unlisted -high60
   ```
4. Join as in step 7, spawn, `!mh p0 tick` (expect `60.0 Hz ... high60`), `!mh wall`, `!mh flat`,
   `!mh queue WA:60 SB`.
5. About 40 min later `!mh status` says `done`.

## 14. Stop safely

`!mh stop` → wait for `[mh] stopped` → `!mh clear` → quit the client → close the server window.

## 15. Manifest after and compare (2 min)

```powershell
pwsh -NoProfile -File "$H\ps\install-manifest.ps1" -Out "$S\manifest-after.csv"
pwsh -NoProfile -File "$H\ps\install-compare.ps1" -Before "$S\manifest-before.csv" -After "$S\manifest-after.csv" | Tee-Object "$S\compare.txt"
```
Expected: `install unchanged: N items` (exit 0). Anything else: keep the files, tell the agent, run nothing more.

## 16. Extract (2-5 min)

```powershell
python -I "$H\extract.py" --session 20261016 --server-logs "$I\logs" --client-logs "$env:LOCALAPPDATA\VeniceUnleashed" --out "$R\research\measurements" --manifest-before "$S\manifest-before.csv" --manifest-after "$S\manifest-after.csv"
```
Expected: one folder per run under `research\measurements\` (`raw.jsonl` or `raw.jsonl.gz`, `meta.json`), `malformed 0`.

## 17. Afterwards

Restore your normal VU loadout (W-B asked for no attachments, W-D for optics). Then only say "done" to the agent, plus
anything that surprised you (one line each, e.g. "bot damage NO (aim)", "prompt for M9 never came"). The agent reads
`research\measurements\` on this PC (run folders and `session-20261016`). Do not send or commit the VU log files,
screenshots or videos: they hold names and addresses; the run folders hold numbers and small ids only.

---

## When something fails

| symptom | do |
|---|---|
| Server stops before "accepting connections", log mentions the backend / Zeus / authentication | Zeus is down or unreachable. Wait 10 min and start again. Still down: stop for tonight (nothing in the harness can replace it). |
| Client or server asks for BF3 activation | Run your own "Activate BF3" Start-menu shortcut (it asks for your account in its own window), then start again. |
| VU updates itself at start | Let it finish; server and client must show the same build. The build is recorded per run; carry on. |
| No `join with ... connect <guid>` line | Close the server, start it again **without** `-unlisted`, and join `mh-measure` from the server browser with the password. Or take the key's guid from the VU key portal where you made your server key (it is the same key as your normal server's). |
| No `[mh]` greeting, no `[mharness]` lines in the server log | Check `ServerMeasure\Admin\ModList.txt` says `mharness` and `Admin\Mods\mharness\mod.json` exists; rerun step 5; restart the server. |
| No log file at all in `ServerMeasure\logs` | Restart the server with `-debuglog` added to the launch line. |
| Chat commands get no answer | Open the client console (`~`) and type `mh status`; if unknown, `mharness.mh status` (the greeting names the right one; `!mh census` reports it as `console ...`). If that works, type every command there without the `!`; the replies appear in the console. |
| `[mh] step out of the line of fire` | You stand between a bot and its target or wall. Walk a few metres to the side or back; the scenario continues by itself (it waits up to 120 s, then skips the config). |
| `bot damage: NO (arbitration)` | Expected possibility. Run W-B, S-A and W-C with `mode=U` (you shoot). The overnight queue is unaffected. |
| `bot damage: NO (aim)` | `!mh p0 aim`, then `!mh p0 damage` again. Still `NO (aim)`: carry on in `mode=U`. |
| `round: preround` | Expected. Close the server window first (the client can stay in the menu). Run `pwsh -NoProfile -File "$H\ps\setup-instance.ps1" -Session s1 -DisablePreRound` (no `-CopyKeyFrom`: it keeps your key and your password), start the server again (step 6), rejoin (step 7), redo Phase 0 from `!mh census`. |
| A scenario seems stuck | `!mh status` shows the state and its timeout (every wait has one). `!mh next` skips the current config; `!mh stop` ends the scenario. After three skipped configs a scenario aborts itself (`[mh] aborted: <reason>`) and the queue moves on. |
| `[mh] waiting for a human` | You left or the client dropped: rejoin (step 7); the queue resumes by itself. |
| The harness asks for a weapon you do not have | Switch team (US: M16A3, M9, SMAW; RU: AK-74M, RPG-7), respawn; still missing: `!mh next`. |
| Server or client crashed | Restart (steps 6-7), redo the sites (`!mh wall`, `!mh flat`, `!mh range`), run `extract.py ... --dry-run` (step 9): it prints `WA last cfg-done 7/17 -> !mh run WA from=8, queue item WA:all:from=8`; type `!mh queue` with that item first and the items that had not run yet after it (the chat also showed each `cfg done` line). |
| `[mh] tickets low, queue paused` | The round is close to ending. `!mh restartround confirm`, wait for the level, `!mh queue resume` (sites stay valid). |
| `[mh] ... not certified tonight` | The checks of step 1 did not certify that scenario; skip it. |
| `install-compare` exit 1 | Stop everything; keep both CSVs; tell the agent. |
