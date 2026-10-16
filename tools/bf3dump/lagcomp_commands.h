#pragma once
// bf3dump M2a lc commands (docs/design/m2a.md §6, E13, E18; as built: docs/design/m2a-lc.md): the capsule pose table
// built from the install (read-only), its deviation from the E18 reference evaluation, and the rewind provider's bench
// rows. Output is BF3-derived data and stays out of the repo. Each run ends with the install.unchanged row and exits 1
// when the install changed.

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

// lagcomp <LEVEL> <MODE> [--deviation] [--perf] [--ticks N] [--warm N] [--rows pairs,long]:
//   always: the table (capsule -> joint map, row tags, aim per stance, hash, build time, stand-idle capsule centres);
//   --deviation: E18 (a) rows, posed_target against the reference evaluation (server_pose_reference.h) at the 32
//     mid-pitches and 7 mid-fractions, the stand rows against S3's CrowdEvaluator, legs and run reported; "E18 (a):
//     PASS|FAIL", exit 1 on FAIL;
//   --perf: E13's rewind rows "pairs" and "long" (64 human shooters at the maximum lag, the level's M16A3, full auto):
//     provider p99 / mean / max per tick, queries, candidates, cache hits, verdicts; "E13 rewind: PASS|WARN|FAIL"
//     (p99 <= 0.5 ms per row; wall clock, run under the quiet lock).
int cmd_lagcomp(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

const char *lagcomp_usage();

} // namespace bf3dump
