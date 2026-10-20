# openBF3: rules for every agent and contributor

These rules are enforced by the ctest row `hygiene.rules` (`tools/check_hygiene.py`) and by review. A change
that breaks them is not merged, however good the rest of it is. Read `docs/architecture.md` before any work.

## No comments in code

No comments of any kind in C++, shaders or CMake: no `//`, no `/* */`, no `#` lines in CMake. The code says what
it does through names, types, asserts and tests. If something needs explaining, change the code until it does
not, or put the one sentence in `docs/architecture.md`. `tools/check_hygiene.py --strip` removes comments; the
plain run fails on any that remain. Existing comments were removed on 2026-10-20; do not bring any back.

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
install with `bf3dump` and record the number in `docs/architecture.md` or a test. Third-party libraries are the
pinned submodules under `extern/` only.

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
