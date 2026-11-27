# openBF3: rules for every agent and contributor

These rules are enforced by the ctest row `hygiene.rules` (`tools/check_hygiene.py`) and by review. A change
that breaks them is not merged, however good the rest of it is. Read `docs/architecture.md` before any work.

## Comments: sources, guesses and reasons only

The code says what it does through names, types, asserts and tests. A comment is allowed only where it says what
the code cannot: where a value or rule comes from, that it is a guess, or why something non-obvious is so. Every
comment is one of exactly these, starts with its tag, sits on its own line(s) above the code it is about, and is one
or two lines (`//` in C++ and shaders, `#` in CMake and `bf3-opensource/config/*.cfg`; no `/* */`, no comment after
code on the same line):

* `SOURCE:` where a value or rule comes from: the BF3 data type and field (`VehicleConfigData.MotionDamping`), a
  `research/` format fact or a `bf3dump` measurement of the install, a clean-room note by its id (`notes R-B4`,
  `notes Q10`), or the OpenBF2 file and pinned commit (`OpenBF2 Code/BF2/AI/... @c47317b`). A value read from a
  named EBX field in the line below needs no comment; the code already names it.
* `INFERRED:` a reading that the data or notes do not state outright (a field's unit, a frame, an order, a value
  read off the retail footage), then `Settle:` and what would settle it.
* `STAND-IN:` a made-up value or simplification standing in for something not built or not known (a hand-picked
  number, a Jolt or engine default in place of BF3 behaviour, a value calculated from the BF2 research install),
  then `Settle:` and what would settle it.
* `WHY:` the reason for something non-obvious that the code itself does not show.

`grep -rn "INFERRED:\|STAND-IN:"` is the list of what is still made up; never write a confident `SOURCE:` for a
value you cannot trace: if unsure, it is `STAND-IN:`. Not allowed: comments that restate what the code does,
change history, test results or measurements of our own build (those go in commit messages), commented-out code,
`TODO`/`FIXME`/`XXX`/`HACK`/`NOTE` markers, closing-namespace comments, IW4L, and anything quoted or paraphrased
from disassembly (cite the note id, never an address or a decompiled name). `hygiene.rules` enforces the tags,
the `Settle:` part, the two-line and 120-column limits and the bans; `hygiene.comment_rule` tests that check.

## No prose as a substitute

Writing is not progress. The only prose that may change is `docs/architecture.md` (as-is description, capped
at 220 lines by the hygiene row), one line per commit in `docs/history/dev-log.md`, and `docs/testing.md` when a
test policy changes. No new markdown, text or notes files anywhere; `docs/design/` and `research/` are frozen
history and are not edited. Commit messages state what changed and why, in a few lines. A report of work is a
diff and a passing ctest pass, not a document.

## Native Battlefield 3 UI only

The HUD, the spawn and death screens, the scoreboard and the menus are the retail SWF movies, rendered: their
display lists, shapes, atlas bitmaps and `DefineFont3` text drawn through a Godot canvas backend, positioned and
timed by the movies' own timelines, fed by the HUD data model (`bf3/core/ui/hud_model.h`). A screen is done when
a screenshot at 1280x720 is indistinguishable from the retail game's at the same moment. Hand-built Godot
`Control` scenes, `Label`s and `TextureRect`s standing in for a BF3 screen are not UI work and are not accepted;
the existing ones in `src/bf3_godot/src/hud_*.cpp` are debts to replace, not patterns to extend. Where a screen's
behaviour is ActionScript, implement what that script does, driven by game state; do not approximate the look.

## No code from other projects

Every reader, decoder, simulation and rule in this tree is written from the format facts in `research/` and
verified against the retail install by the tests. No code is translated, ported or paraphrased from IW4L (the
earlier Rust experiment), Rime, fun-bots, IceBloc, Frosty, Venice Unleashed or any other codebase, whatever its
licence, and no such code is consulted while writing ours. Where a format fact is missing, measure it on the
install with `bf3dump` and record the number in a `SOURCE:` comment beside the value or in a test. Third-party libraries are the
pinned submodules under `extern/` only.

One project is the exception: [OpenBF2](https://github.com/B2BF/OpenBF2), the GPL-3.0 reconstruction of the
Battlefield 2 engine. Its bot AI (`Code/BF2/AI`) may be read, ported and adapted for openBF3's bot, squad and
commander AI; openBF3 is licensed GPL-3.0 (`LICENSE`) so that it can be. Every file that holds ported OpenBF2 code
is named in `docs/architecture.md`'s bots row with the OpenBF2 path it came from (`ai.port.provenance` checks it)
and carries a file-level `SOURCE:` line with that path and pin; each place the port departs from it is marked
`INFERRED:` or `STAND-IN:`.

## No shortcuts

* A feature is done when its ctest row passes against the retail install and, for anything visible, its
  screenshot gate passes. A stub, a `TODO`, a hard-coded value where data exists, or a simplified look is not done
  and is reported as not done.
* Tests are not weakened to pass: no loosened tolerances, no removed checks, no rows moved to `realtime_retry`,
  no `SKIP` where the data is present. A failing test is a bug in the code or the test; say which, fix that.
* Every commit builds (`tools\build.cmd dev`) and passes the branch ctest pass. Rows the change touches are run
  and named in the commit message.
* Nothing from the install is committed: no dumps, no decoded assets, no partition text.
* Say what was not done. A summary that omits the unfinished part is a false report.
