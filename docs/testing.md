# Testing: test budget policy

Every ctest row is either a functional row that runs on every branch build or a timing, real-time or window row
that runs in the main-merge timing pass. This page sets the rules that keep the branch loop short without losing
what the rows prove. `tools/test_budget.cmake` checks them after every ctest phase that `tools\build.cmd` runs,
and configure-time guards in `tests/CMakeLists.txt` enforce the Godot rules.

## Measuring

* `tools\build.cmd` writes `b\<preset>\ctest-<preset>-<phase>.xml` (ctest `--output-junit`) and runs
  `tools/test_budget.cmake` on it. The report lists the 15 slowest rows (wall seconds, labels, PROCESSORS and
  PROCESSORS x wall), the sum of the row walls next to the phase wall, the skipped rows by name, the failed rows,
  budget warnings and budget violations. `--strict` makes a violation fail the run; acceptance uses it.
* By hand: `cmake -DJUNIT=<file> -DTESTDIR=b\dev [-DSTRICT=1] [-DASAN=1] -P tools\test_budget.cmake`.
* Junit times are wall times. A row measured in a parallel phase reads slower than it does alone, and the budgets
  below are for a row alone on a quiet machine. When a row reads over budget in a parallel phase, time it alone
  (`ctest --preset dev -R "^<row>$"`) before acting on it.
* CPU seconds need the process tree's accounting: run the command inside a Windows Job Object
  (`QueryInformationJobObject`, `JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`). The workflows keep such a harness outside
  the repo. Acceptance notes quote a row's measured wall time, its CPU seconds (or cpu/wall), its peak working set
  and, for memory gates, its peak commit.

## 1. Scheduling labels

A row is unlabelled or carries exactly one scheduling label. Unlabelled means functional: it runs in every branch
dev pass in parallel and must be parallel-safe.

| label | meaning | where it runs |
|---|---|---|
| (none) | functional; gates no time | every branch dev pass, `-j16` |
| `timing` | gates wall or CPU time | the merge pass, serially |
| `realtime` | waits on real time at under 0.5 core and gates latency behaviour | the merge pass's real-time lane, `-j8`, never together with unlabelled, timing or window rows |
| `window` | needs a window, GPU frame time or audible output | the merge pass's user-away tenure, serially, never on branches |

Extra labels: `serial_wallclock` (RUN_SERIAL in the dev pass for a wall-clock CHECK), `merge_also` (also rerun
serially in the merge pass, e.g. memory gates), `bigmem` (peak above 8 GB), `asan_sweep` (asan wall above 600 s).

The test presets carry the filters, so a bare `ctest --preset dev` is always the safe branch selection.
`--all-tests` runs phases, never one mixed ctest call. A branch that changes what a timing row covers runs that row
with its phase's preset, e.g. `ctest --preset dev-timing -R "^host\.gunplay\."`; the `dev-timing` preset also sets
`OPENBF3_CL_TIMING=1`, the opt-in of `host.gunplay.client`'s Metro gates.

A row that waits on wall-clock windows it does not measure (a reply within 0.5 s, a 60 Hz client in real time) is
unlabelled only when it gives its alone results beside a `-j16` branch pass in repeated runs; the branch pass runs
at below-normal priority next to other builds, so RUN_SERIAL there does not restore a quiet machine. Otherwise it is
a `timing` row, split first so its socket-free cases stay on branches (`host.session.unit.pure`,
`host.gunplay.client.unit.pure`). `host.gunplay.server` (1 failure in 3 such runs) and `host.gunplay.client.unit`
(1 in 10) are timing rows for this reason.

## 2. Parallel-safe by default

* Scratch goes to per-PID or `string(RANDOM)` roots, preferably under `${CMAKE_BINARY_DIR}/scratch`. Outputs go to
  `${CMAKE_BINARY_DIR}/shots/<row>`, never a fixed `%TEMP%` name; the documented ui1 shots path is the one exception.
* Ports are 0. The install is read only through the readers.
* Godot rows pass their own `--bf3-cache` root, never the shared `user://bf3cache`. Files shared inside a worktree
  get a RESOURCE_LOCK.
* RUN_SERIAL only on rows labelled `timing`, `realtime` (outside the lane), `window`, `bigmem` or
  `serial_wallclock`. The reason is a label because `ctest --show-only=json-v1` carries no CMake comments.
* PROCESSORS = the measured cpu/wall, rounded, whenever it is 2 or more. COST = the measured serial seconds on every
  row over 5 s, so a fresh build directory without `CTestCostData.txt` still starts the critical path first.

## 3. Godot rows

* Every `godot.*` row is registered through `openbf3_godot_row(HEADLESS|WINDOW)`. Anything else, or a windowed row
  without the label `window`, is a configure error.
* Headless rows find the Godot console executable at configure time (`OPENBF3_GODOT_EXE`, next to the checkout) and
  run on every branch; `OPENBF3_GODOT` overrides it.
* Windowed rows run only with `OPENBF3_GODOT_WINDOWED=1` and exit 77 while `OPENBF3_USER_ACTIVE_FLAG` names a file
  that exists.
* The project is set up by the `godot.project_setup` fixture, which writes `.godot/extension_list.cfg` and never runs
  `--import` (the first `--import` of a never-opened project exits 0xC0000005, README "Run").

## 4. Budgets

Dev preset, a row measured alone on a quiet machine; asan is about 10x.

* `*.unit`, `*.contract` and data-free fuzz rows: at most 2 s.
* Install rows: at most 15 s.
* A row above 30 s needs a written reason in its CMake comment and a COST. Remove repeated work first (decode or
  load once per process, shared untimed setup). Then add parallelism inside the test (`parallel_map` over
  independent items, RNG streams pre-drawn serially, CHECKs on the main thread, output in item order), or split the
  cases with `openbf3_case_row`.
* No dev row above 60 s (`sim.terrain` is exempt at up to 150 s).
* Timing, real-time and window rows: at most 120 s each unless the gate defines the length (G8's 30 s recording,
  E2's 90 s, M2a cl's E15 10 min loopback in `host.gunplay.client`, about 990 s in all; `host.gunplay.server`'s
  60 Hz play, about 127 s; `godot.fx_ref`'s alternating G1b windows, about 182 s); the design doc then states it.
* Asan: a row over 600 s gets `asan_sweep`. Its full tier runs at acceptance for branches touching what it links,
  and on the latest main after merges.

`test_budget.cmake` reports the 60 s dev cap, RUN_SERIAL without a reason label, a row over 30 s without COST and a
failed `split_cover` row as violations; the other budgets warn.

## 5. Waiting rules

* A test that waits for something it does not measure waits on state (poll or condition, with a 10 s timeout),
  never on a fixed sleep.
* No `sleep_for` below 16 ms as jitter: Windows rounds it up to the 15.6 ms timer tick. Spin with `yield()` instead.
  Where the tick-length life itself is the coverage (instance churn in `host.registry`), keep it on purpose and say so.
* Simulated or stepped time wherever the wall clock is not what the test proves.

## 6. Splits

* `openbf3_case_row(NAME EXE SUITE COUNT n CASES ...)` uses doctest wildcard patterns without spaces or commas and
  always adds the suite's install-unchanged case.
* Its runner (`tests/run_case_row.cmake`) requires exit 0 and exactly `test cases: n | n passed`, and passes exit
  code 77 through, so a mistyped pattern that only runs the install check fails.
* A `<suite>.split_cover` row proves that every case of the suite is in exactly one split row.
* Every install row keeps install.unchanged.
* A Godot check splits by its own switch instead (`--m1-phase` for `godot.m1.e1`, `.e2_gate`, `.e2_stress`); each
  shard prints its own table with install.unchanged. A headless shard may drop the `openbf3_godot_project` lock
  (`openbf3_godot_row(... NO_PROJECT_LOCK)`) only after the project's `.godot` directory hashed identical before and
  after its runs.
* A lane split lands after 5 lane measurements on main (2 of them under branch load) without reruns; a row that
  needed a rerun goes back to `timing`.

## 7. Shared machine

* `build.cmd` runs every build and ctest call at below-normal priority.
* While the file named by `OPENBF3_QUIET_LOCK` exists it uses `-j4` and `/m:4`. It never waits on that lock, never
  creates it and never deletes it.
* The merge pass settles on measured CPU load (under 20 % for 30 s, cap 300 s), not on a fixed sleep, and runs the
  window phase in its own tenure after the user is away.
* A full asan run and the next timing pass never overlap.
* After a merge, the main workflow skips main's dev ctest pass (never the build or the timing pass) when the merged
  tree, the CMakeCache hash (without paths), the compiler and the Godot executable equal a branch pass recorded as
  accepted in the workflows' ledger, that pass had 0 failed rows, and it skipped only rows main's pass skips too.

## 8. Asan

| preset | selection | jobs |
|---|---|---|
| `asan` | every row except `asan_sweep`, `realtime` and `window` (no Godot rows exist under asan) | 16 |
| `asan-realtime` | the `realtime` rows (threaded ServerHost, ClientSession and ENet lifecycles) | 4 |
| `asan-sweep` | the `asan_sweep` rows: asan wall above 600 s after the in-test lanes | 8 |

* `tools\build.cmd asan` runs `asan` then `asan-realtime`, never mixed; `--asan-full` adds `asan-sweep`. Timing
  rows stay in the selection with their gates report-only under `OPENBF3_ASAN` (and RUN_SERIAL).
* The full tier runs at branch acceptance when the diff touches what the sweeps link (src/bf3_core level, res,
  collision, physics, havok or height grid code, src/bf3_sim, third_party) and once on the latest main after a
  burst of merges, coalesced, at below-normal priority; the next timing pass waits for it.
* Asan TIMEOUTs are about 3x the measured asan wall of the row in its phase.
* Rows whose real-time or wall-clock gates still bite under asan run alone inside their phase (RUN_SERIAL under
  `OPENBF3_ASAN`): `host.session.gate` in asan-realtime, `core.gameplay.tables.unit` (serial_wallclock).
* First measurement (2026-10-17, busy shared machine): the asan selection took 3,552 s at -j16, of which 1,200 s
  was `host.scale` timing out alone at the end (now asan_sweep with `sim.nav`, `sim.vehicle`, `sim.terrain` and
  `host.scale.paced`), against about 12,047 s serially before; asan-realtime 608 s with `host.scale.paced`.

## 9. Rerun rule (merge lane only)

* A failed lane row is rerun alone only when every failing line matches its listed real-time checks
  (`tests/realtime_retry.cmake`). Functional failures, crashes and timeouts fail at once.
* Each lane run and each rerun is logged (`OPENBF3_RERUN_LOG`; every lane run writes a `- lane` line, so the log
  holds the clean merges too). A check that needs a rerun on 2 of the last 5 lane runs (the last 5 merges) fails the
  pass.
* The merge pass sets `OPENBF3_TIMING_REQUIRE_RUN=1`: a phase that selects no row, or a row that skipped (77: no
  Godot exe, no `OPENBF3_GODOT_WINDOWED`, the user-active flag), fails, and no rerun clears it. Every merge-pass gate
  has to run on main; without the variable (a branch's `--all-tests`) the windowed rows may skip.
* The merge workflow runs the merge pass only through `tools\build.cmd dev --phase ...` (or `--timing`), never as
  `ctest --preset dev -R <regex>`: the `dev` preset excludes the timing, realtime and window rows, so such a call
  selects almost none of them and still passes.
