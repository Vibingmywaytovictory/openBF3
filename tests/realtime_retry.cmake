# The merge lane's rerun rule (docs/testing.md "Rerun rule"; tools/timing_pass.cmake includes this file). The rows
# labelled realtime run side by side in one ctest call (dev-realtime, -j8). When one fails there, the driver reruns it
# alone only when every failing line of its output is one of the row's real-time checks below: checks with an upper
# bound that a neighbour's scheduling can push over (tick counts over a slept wall interval, RTT, the clock target,
# extrapolation, the rest judge of E2, tick rate and step-time p99). Any other failing line, a crash, a timeout or a
# missing summary fails the pass at once, so the functional checks of these rows (E1 traces, E3, hashes,
# install.unchanged) keep today's single-run rule.
#
# Per row:
#   RT_<row>_FAIL      regex of a failing line (how the row prints a failed check)
#   RT_<row>_SUMMARY   regex that must be present (the row reached its summary: no crash, no early exit)
#   RT_<row>_RETRY     regex a failing line must match to allow the rerun
#   RT_<row>_NO_RETRY  regex (or empty) that forbids the rerun even when RETRY matches (functional failures with the
#                      same check name)
# A failing line names the check: doctest prints "file(line): ERROR: CHECK( <expr> ) is NOT correct!", the Godot
# checks print their table rows ("  FAIL  E2.150/2 ...", "| server.tick_rate | FAIL | ...").
set(RT_ROWS host.server host.session host.session.unit godot.m1 godot.s0_selftest)

set(RT_host.server_FAIL "ERROR: ")
set(RT_host.server_SUMMARY "\\[doctest\\] test cases:")
# tests/host_tests.cpp: the 60 Hz tick count over a 2 s sleep and the measured rate.
set(RT_host.server_RETRY "CHECK\\( s\\.(ticks (>=|<=) 1[13]0|measured_hz == )")
set(RT_host.server_NO_RETRY "")

# tests/host_session_tests.cpp: E2 (judge_rest cycles, the clock target set and its changes, RTT within the bound,
# extrapolated share), the loopback case's clock target, RTT bound and presentation sample counts (frames presented
# and compared over a real-time window), the teleport's smoothing deadline.
set(_rt_session_retry "CHECK(_MESSAGE)?\\( (c\\.ok|\\(t == 2 \\|\\| t == 3\\)|run\\.target_changes <= 2|rtt (>=|<=) nominal|rtt_sum / double\\(rtt_n\\) <= |s\\.target_buffer_ticks == 2|double\\(run\\.interp\\.extrapolated\\) < |double\\(zero_ns - reconcile_ns\\) / 1e6 <= 400|interp\\.(presented > 500|compared > 300))")
set(RT_host.session_FAIL "ERROR: ")
set(RT_host.session_SUMMARY "\\[doctest\\] test cases:")
set(RT_host.session_RETRY "${_rt_session_retry}")
set(RT_host.session_NO_RETRY "")
set(RT_host.session.unit_FAIL "ERROR: ")
set(RT_host.session.unit_SUMMARY "\\[doctest\\] test cases:")
set(RT_host.session.unit_RETRY "${_rt_session_retry}")
set(RT_host.session.unit_NO_RETRY "")

# src/bf3_godot/src/m1_check.cpp: the table rows E2.<profile> (judge_rest over the rest cycles, RTT, clock target,
# extrapolation). Not when the row failed because the client could not restart or spawn (fail_phase).
set(RT_godot.m1_FAIL "^  FAIL  ")
set(RT_godot.m1_SUMMARY "M1 CHECK: (PASS|FAIL)")
set(RT_godot.m1_RETRY "^  FAIL  E2\\.")
set(RT_godot.m1_NO_RETRY "client restart|did not spawn|gave no new life")

# src/bf3_godot/src/s0_selftest.cpp: server.tick_rate and server.step_time (p99 < 5 ms).
set(RT_godot.s0_selftest_FAIL "^\\| [^|]+ \\| FAIL")
set(RT_godot.s0_selftest_SUMMARY "S0 SELFTEST: (PASS|FAIL)")
set(RT_godot.s0_selftest_RETRY "^\\| server\\.(tick_rate|step_time) +\\| FAIL")
set(RT_godot.s0_selftest_NO_RETRY "")
