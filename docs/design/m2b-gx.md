# M2b gx: the M2b scene (Godot integration), as built

Track gx of M2b wave 2, on branch `m2b/gx` from the W2.0 contract head 5191059 of `m2b/w2`. Design:
[m2b.md](m2b.md) §1 (E9, E12), §4 and the gx row of §11; the contract is [m2b-w2.md](m2b-w2.md). This file covers
what was built, the named defaults, where the code differs from the design, and the gates, with one command each.

The goal is a full Metro TDM round against bots through `tools\play.cmd`. ui (the TDM screens) and cam (the death
view, the corpse time) are built in parallel. gx builds their parts only when their bodies are in the tree
(`OPENBF3_M2B_HAVE_UI` / `_CAM`). Until then a plain status label stands in for the spawn screen, so the scene can
already be played. No VU session measured anything here: every timing of the scripted check, and the Deploy
resend, is a named default with measured = false.

## 1. What the commits contain

| file | what |
|---|---|
| `src/bf3_godot/src/m2b_game.{h,cpp}` (h: the `// M2b gx` block after the contract) | Godot-free rules: `parse_m2b_options` (the play.cmd args), `m2b_spawn_screen_up`, `m2b_can_deploy`, `M2bDeploy` + `m2b_deploy_step` (the SpawnRequest resend), `M2bPlayTally` and `m2b_play_rows` (E9's rows, verdicts and skips) |
| `src/bf3_godot/src/level_session_m2b.{h,cpp}` | `M2bGame`'s "as built" members; the hook bodies `m2b_gx_args`, `_load`, `_server`, `_presented`, `_frame`, `_shutdown`; `Bf3LevelSession::m2b_join_ready` |
| `src/bf3_godot/src/m2b_check.cpp` | `M2bCheck` (`--m2b-check=play`, E9) |
| `src/bf3_godot/src/input_mapper.{h,cpp}` `// M2b gx [mapper]` blocks at the end | the actions `bf3_gadget` (3), `bf3_scoreboard` (Tab), `bf3_deploy` (Enter, keypad Enter), `bf3_kit_1..4` (1-4); the spawn screen's routing; the scripted presses for the check |
| `bf3-opensource/scenes/m2b_metro.tscn` | the M2b scene: the session node with `openbf3_m2a` and `openbf3_m2b`, plus the minimap |
| `tools/play.cmd` | the default is now the M2b scene with `--bots=31`; `--m2a` (or bb's `--tdm` preview) keeps the M2a scene with 24 bots; `--help` lists the M2b keys |
| `tests/m2b_gx_tests.cpp`, `tests/godot_m2b.cmake`, tests CMake `# M2b gx` block | `m2b.gx.unit` (functional); `godot.m2b.play` (E9) and `godot.m2b_load` (E12) as WINDOW rows |
| `src/bf3_godot/CMakeLists.txt` `# M2b gx` block | the three new sources |

Named insertions in other code (commit af3a8ec, which names each one):

| where | tag | what |
|---|---|---|
| `level_session_m2a.cpp` | `[include]` | `#include "level_session_m2b.h"` |
| `m2a_parse_args`, after bb's `[tdm]` line | `[args]` | `m2b_gx_args(g, <scene openbf3_m2b>, mapper_, bots_, quit_after_, error)` |
| `m2a_load_options`, after bb's line | `[load]` | `m2b_gx_load(*m2a_, r_options)` |
| `m2a_server_config`, after bb's line | `[server]` | `m2b_gx_server(g, r_game, bots_)` |
| `m2a_presented`, before step 5 (K / T) | `[presented]` | `m2b_gx_presented(g, *client_, f, mapper_, p_now_ns)` |
| `m2a_frame`, after `g.ms_hud` | `[frame]` | `if (m2b_gx_frame(...)) { return true; }`: true only on the frame the M2b check called `finish()` |
| `m2a_shutdown`, after `g.check.reset()` | `[shutdown]` | `m2b_gx_shutdown(g)` |
| `level_session.cpp` `try_done` | `[done]` (replacement) | `spawned = (<M1's condition>) \|\| m2b_join_ready()` |
| `level_session.h`, after the M2a hooks | `[done]` | `bool m2b_join_ready() const;` |
| `input_mapper.cpp` `handle_event` | `[mapper]` | `if (m2b_handle_event(p_event)) return;` before the M2a gx line |
| `input_mapper.cpp` `kM2aWheelSlots` | `[wheel]` (replacement) | `{ 0, 1, 2, 6, 7 }`: slot 2 (the gadget) in the wheel. M2a's kits leave it empty, so the wheel skips it there |
| `bf3_fx/src/triggers.cpp` `FxTriggers::sane` | R13 (replacement) | `<= PresentKind::Spawn` becomes `<= PresentKind::Revive` |

## 2. The session

The M2b session is the M2a session with the TDM rules on. The scene carries both metadata keys, so every M2a part
(viewmodel, HUD, crowd, FX, audio) runs unchanged. `M2aGame::m2b` is created in `[args]` when the scene says
`openbf3_m2b` or the command line has `--m2b-check`. Every gx hook returns at once when it is null, so the M2a
scene, M1, S1 and S3 are unchanged (the M2a rows are not affected by gx).

* **[args]** (main thread) parses the following, and a bad value fails the load with the arg named:
  - `--kill-limit=0..100000` (0: the data's 100);
  - `--preround=0..600` (seconds; `<0`/absent: the data's 10 s);
  - `--bots=0..31`;
  - `--deploy-screen=0|1`;
  - `--m2b-play-seconds=5..3600`.

  It replaces M2a's bot count (24) with `M2bOptions::bots` (31). `config/tdm_tuning.cfg` and
  `config/bot_tuning.cfg` are used when present. A check run sets the quit guard.
* **[load]** sets `SharedLoadOptions::tdm` and `tdm_tuning_cfg`.
* **[server]** sets the rules and the bots:
  - `MatchRulesConfig`: mode Tdm, `deploy_screen`, revive on, `kill_limit_override`, `preround_override_s`,
    `max_players` 32;
  - `tdm_bots` (`TdmBotConfig::count`, `tuning_cfg`) instead of bt's firefight bots.

  It also builds the static `CollisionWorld` for cam's pull-in when the M2a checks did not, and reads
  `SoldierDef::time_for_corpse` for the crowd.
* **[presented]** (every frame from the session start):
  - the own slot (MatchInfo's, spawned or not) and team (`own_team`, else the own player row);
  - `slot_team` from the player rows;
  - the own death from the drained Death events (the time, the killer, and `HudKillCardSource` with the killer's
    presented distance), and Revive events for the own slot;
  - each new own life (a changed life number, or alive again after a revive): it counts a deploy and checks
    `own_kit` against the requested kit within 1 s;
  - the round restart (RoundOver to another phase: `spawned_this_round` resets, so the join screen comes back).

  With the deploy screen on, it also handles the spawn screen:
  - `m2b_spawn_screen_up` (no kill card up; PreRound or InRound; not alive or unspawned);
  - `m2b_can_deploy` (server tick >= `own_respawn_tick`);
  - the kit keys (and K, which cycles the kit; T is dropped, since TDM teams are AutoTeam's);
  - Enter or a Deploy click arms the request, and `m2b_deploy_step` sends `send_spawn_request(kit)`.
* **[frame]** (after Done, after the M2a HUD):
  - **First call.** It frees M2a's score and kit labels (and the death label when the deploy screen is on), builds
    `Bf3TdmHud` in a window (ui), creates `Bf3DeathCamera` (cam) and sets the corpse time before the crowd's build.
    Without ui it adds the status label.
  - **Each frame, ui and cam.** `fill_hud_tdm` runs with the same drained events, then `Bf3TdmHud::set_model`, and
    gx takes the kit and Deploy clicks. While dead, the death camera is stepped and its view is kept.
  - **Each frame, camera and mouse.** Before the first own life, the join camera looks over the first spawn point.
    The mouse is released while the spawn screen (or the round end) is up and captured again after.
  - **Each frame, check.** The M2b check is created once the HUD has loaded, then stepped.
* **[done]** makes Done come with the spawn screen at join: the match is known and the client is connected and not
  refused. Without it, Done would wait for a spawn that only Deploy makes (R10). So E12, with the deploy screen,
  measures the load to the spawn screen. The row passes `--deploy-screen=0`, where Done stays M1's first spawned
  frame.

Named defaults (measured = false):

| constant | value | where |
|---|---|---|
| `kM2bSpawnRequestRetryTicks` | 30 server ticks | W2.0 (m2b_game.h): the SpawnRequest resend while Deploy is armed and no life came |
| new-life kit check | 1 s | level_session_m2b.cpp: `own_kit` must equal the requested kit within this time |
| revive window | 2 s | a life within 2 s of a Revive event of the own slot is a revive, not a deploy |
| join camera | 3 m above spawn 0, pitched -0.25 rad | level_session_m2b.cpp |

## 3. Where the code differs from the design

* **[done]** is a named replacement that m2b-w2.md did not list. Without it the deploy screen never reaches Done.
  The integrator should confirm it.
* With `--deploy-screen=0` the M2a death label (the respawn countdown) stays and the spawn screen is hidden, even
  when ui is merged. K and T stay M2a's KitSelect.
* M2a's `[m2a] firefight bots: 31 (free)` line still prints in an M2b session, because M2a's server config runs
  before gx's `[server]`, which then removes the firefight bots. The `[m2b] 31 TDM bots` line that follows is the
  true one.
* The E9 check makes sure of at least one own death. After 20 s alive in a round, a dev Teleport puts the player
  4 m behind the nearest enemy, facing it (the player never fires). After 38 s, a 60 m drop kills it (M1's fall
  damage).

## 4. The play check (E9)

`--m2b-check=play` runs as follows:

1. The scripted player waits on the join spawn screen. It presses a kit key 0.6 s after the screen comes up and
   Enter 1.2 s after, once Deploy is enabled. The kit cycles assault, engineer, support, recon across deploys.
2. Alive, it stands in the open and holds Tab for 0.6 s every 9 s. With ui built, each Tab sample compares the
   scoreboard with MatchState.
3. After each own death it deploys again.
4. The run ends 3 s after RoundOver (`--kill-limit=15` makes the round short), once there is at least one own death
   and two deploys. Otherwise it ends at play_seconds + 45 s.

Screenshots in a window go to `--m2b-shots`: `gx_m2b_join`, `gx_m2b_killcard`, `gx_m2b_spawn`,
`gx_m2b_scoreboard`, `gx_m2b_round_end`.

Rows:

* **gx:**
  - `match_state`, `spawn_screen_join`;
  - `own_deaths >= 1`;
  - `spawn_screen_after_death`: equal to the own deaths that had time to show it, and not revived first; at least one;
  - `deploys >= 2`, `deploy_kit_mismatch == 0`, `round_over`.
* **ui** (skipped "waiting for ui" until merged):
  - `kill_feed_lines` equals the deaths received;
  - `scoreboard_kills_mismatch == 0` (within one kill, since the drawn frame lags by one);
  - `kill_card_bad == 0` (a killer and the weapon's item; in a window also the drawn name);
  - `spawn_screen_shown >= 1 +` the checked deaths;
  - `round_end_screen`;
  - `missing_lookups == 0` (window only).
* **cam** (skipped "waiting for cam"): `death_views >=` the own deaths, with 0 bad inputs.

`godot.m2b.play` fails on any ERROR line, and in the merge pass (`OPENBF3_TIMING_REQUIRE_RUN=1`) on any row still
"waiting for".

## 5. Gates (measured 2026-10-19 on `m2b/gx`, shared machine; times indicative only)

| gate | threshold | measured | command |
|---|---|---|---|
| m2b.gx.unit | all cases pass | 5 cases, 197 assertions, pass (0.09 s) | `ctest --test-dir b\dev -R "^m2b\.gx\.unit$"` |
| R13 | a Revive event is not rejected by `FxTriggers::sane` | rejected 1 of 3 (only the unknown kind) | (the R13 case of m2b.gx.unit) |
| E9 gx rows (headless, by hand) | the 7 gx rows pass, M2B CHECK PASS, install unchanged | 7/7 PASS. Join screen at 0.0 s, deploy assault (life 1 at 1.3 s), seek-enemy teleport at 20 s, own death at 27.0 s, deploy engineer (life 2 at 32.3 s), RoundOver at 39.6 s (US 8 - RU 15), 24 deaths received, 2 requests, 0 resends, run 42.6 s; ui 6 and cam 1 rows SKIP "waiting for"; install unchanged: yes; done at 5.59 s warm (headless) | `"%OPENBF3_GODOT%" --headless --path bf3-opensource res://scenes/m2b_metro.tscn -- --m2b-check=play --kill-limit=15 --preround=0` |
| E9 (godot.m2b.play) | M2B CHECK PASS in a window, 0 ERROR lines, no "waiting for" in the merge pass | not run: a WINDOW row for the wave-2 merge pass (ui and cam not merged here) | `set OPENBF3_GODOT_WINDOWED=1` then `ctest --test-dir b\dev -R "^godot\.m2b\.play$"` |
| E12 (godot.m2b_load) | m2b.md §1 E12 load budget | not run: a WINDOW row for the merge pass | `set OPENBF3_GODOT_WINDOWED=1` then `ctest --test-dir b\dev -R "^godot\.m2b_load$"` |
| play.cmd | the M2b scene by default; `--m2a` / `--tdm` select the M2a scene; `--bots=` wins | 6 argument sets dry-run through a copy that echoes the Godot line: all as expected | `tools\play.cmd`, `tools\play.cmd --m2a`, `tools\play.cmd --help` |
| ui / cam code paths compile | `level_session_m2b.cpp`, `m2b_check.cpp` compile with `-DOPENBF3_M2B_HAVE_UI=1 -DOPENBF3_M2B_HAVE_CAM=1` | both compile, 0 warnings (/W4); not linked (no bodies yet) | the TU compile commands from `b\dev\compile_commands.json` with the two defines added |
| branch pass | every functional row passes, install.unchanged | 148 rows: 146 passed, 2 skipped (`core.cache_data`, `core.res`, as before), 0 failed; 331.7 s of ctest wall time; exit 0. One "budget VIOLATION" line is for `host.tdm.server` (wave 1's row: 63.1 s without COST under the parallel load), not a gx row | `tools\build.cmd dev` |

## 6. Handoff (merge pass)

* ui and cam rows: E9's 6 ui rows and 1 cam row pass only after their merges. A reconfigure is needed after the
  merge, because `OPENBF3_M2B_HAVE_UI` / `_CAM` come from configure-time `EXISTS` checks. The code under them
  (`fill_hud_tdm`, `Bf3TdmHud`, `Bf3DeathCamera`, `set_corpse_seconds`, `TdmHudRecords`) was compiled on this branch
  against the W2.0 headers with both switches forced on: `level_session_m2b.cpp` and `m2b_check.cpp` compile with
  `/W4` and no warnings, into scratch objects. It has not been linked or run, because the bodies are ui's and cam's.
* The windowed run of E9 and E12, and the screenshots for the user's look, are the merge pass's.
