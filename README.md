# openBF3

openBF3 is an independent, from-scratch reimplementation of the multiplayer game of Battlefield 3: infantry, bots,
weapons and vehicles, built on Godot 4.8 with all game code in C++ (a GDExtension, no GDScript). It ships no game
content. At runtime it reads the data of your own retail Battlefield 3 install. The project is not affiliated with
or endorsed by Electronic Arts or DICE. The design is in [research/PORTING-RESEARCH.md](research/PORTING-RESEARCH.md);
this tree implements milestone step S0 (toolchain and skeleton).

## The read-as-is rule

* The install (default `C:\Program Files\EA Games\Battlefield 3`) is read in place and never modified. Only
  `bf3_core::InstallReader` resolves install paths. It opens files with `GENERIC_READ` and full sharing, and nothing
  in the code base opens a write handle there.
* Every write target goes through `PathGuard`, which canonicalises the path (8.3 names, casing, `/`, `..`,
  junctions, trailing dots, streams, hard links) and refuses anything under the install root.
* Derived data goes only to `user://bf3cache/`, keyed by content digest. Nothing from the install goes into this repo:
  no dumps, no decoded assets, no partition text. Tests read the install at runtime and assert only numbers.
* The S0 self-test records size and mtime of `Data/layout.toc`, every `cas.cat` and every `.toc`/`.sb` under
  `Data/Win32` and `Update/*/Data/Win32` before and after a session, and fails on any difference.

Install location, first match wins: `--bf3-install=<dir>` (user arg), env `OPENBF3_INSTALL`, project setting
`openbf3/install/path`, then the default path above.

## Prerequisites (Windows x64)

* Visual Studio 2026 (18) with MSVC 19.51+ (the bundled CMake and Ninja work; CMake ≥ 3.25)
* Python 3 (godot-cpp's binding generator), git
* Godot `4.8.dev7.official.c971f93e7` (console and GUI exe). Other builds need an API re-dump (see below).
* A Battlefield 3 install with Update/Patch, needed only to run the game and the install tests

## Clone, build, test

```bat
git clone --recurse-submodules <repo-url> openBF3
cd openBF3
tools\build.cmd dev            :: = vcvars64 + cmake --preset dev + cmake --build --preset dev + ctest --preset dev
```

From a VS x64 developer prompt the three steps are `cmake --preset dev`, `cmake --build --preset dev` and
`ctest --preset dev`. Presets: `dev` (optimised, godot-cpp `template_debug`, everyday use), `debug` (unoptimised,
`/MTd`) and `release` (`template_release`, for shipping). Build trees go to `b\<preset>`. The short path matters:
godot-cpp's generated object paths break MAX_PATH (C1083) under deep roots. The longest object path is 119
characters below the checkout (`b\release\gcpp\...plane_tracking.cpp.obj`), so keep the checkout path under about
140 characters. The extension DLL is written to `bf3-opensource\bin\` (gitignored).

Every target uses the static CRT (`/MT`, or `/MTd` in `debug`) and builds with C++ exceptions off
(`_HAS_EXCEPTIONS=0`). This covers godot-cpp, Jolt, ENet, Recast and doctest.

`ctest` runs `core.guard`, `core.install`, `sim.jolt`, `net.enet` and `host.server`. `core.install` reports
*Skipped* when no install is found.

## Run

```bat
set "GODOT=C:\path\to\Godot_v4.8-dev7_win64_console.exe"
:: once per fresh checkout, so Godot registers the extension (.godot\extension_list.cfg):
"%GODOT%" --headless --path bf3-opensource --import
:: S0 self-test, headless: prints a PASS/FAIL table and exits with code 0 (all pass) or 1
"%GODOT%" --headless --path bf3-opensource -- --s0-selftest [--selftest-seconds=5]
:: windowed game (D3D12): server thread + overlay; optional timed screenshot
"%GODOT%" --path bf3-opensource [-- --quit-after=10 --screenshot=C:\temp\s0.png]
:: dedicated server: no scene, no renderer
"%GODOT%" --headless --path bf3-opensource --main-loop DedicatedMainLoop -- --dedicated-seconds=10
```

Known issue: on a project that has never been opened, the first `--import` registers the extension and writes
`extension_list.cfg`, then exits with 0xC0000005 during editor shutdown. Later runs exit 0. Writing a
one-line `.godot\extension_list.cfg` containing `res://openbf3.gdextension` has the same effect. If the extension
is not loaded, the scene runs without `GameSession` and never quits, so scripts should run with a timeout and check
for the `S0 SELFTEST: PASS` line.

## Engine pinning

godot-cpp is built from the API of the exact engine build, not from its bundled JSONs:
`engine-api/4.8.dev7.c971f93e7/` holds `extension_api.json` (`--dump-extension-api`) and
`gdextension_interface.json` (`--dump-gdextension-interface-json`), passed as `GODOTCPP_CUSTOM_API_FILE` and
`GODOTCPP_GDEXTENSION_DIR`. A dev7 DLL loads silently into any 4.8.x, and a changed method hash only fails at its
first call. So on every engine bump (dev → RC → stable): dump both JSONs into a new
`engine-api/<version>.<hash>/` folder, run `<godot> --headless --validate-extension-api <old extension_api.json>`,
point `OPENBF3_ENGINE_API` at the new folder and rebuild from clean.

## Layout and pinned third-party code

| path | what |
|---|---|
| `src/bf3_core` | `InstallReader`, `ReadOnlyFile`, `PathGuard`, install snapshot (Godot-free) |
| `src/bf3_sim` | own Jolt world at a fixed 60 Hz, Recast/Detour link check (Godot-free) |
| `src/bf3_net` | upstream ENet wrapper, loopback ping (Godot-free) |
| `src/bf3_host` | `ServerHost` on its own `std::thread`, `LoopbackClient` (Godot-free) |
| `src/bf3_godot` | the GDExtension: `GameSession`, `DedicatedMainLoop`, S0 self-test (the only Godot code) |
| `tests/` | doctest executables run by ctest |
| `bf3-opensource/` | the Godot project (Forward+, D3D12, Jolt); `scenes/main.tscn` is the main scene |

| submodule | pin | licence |
|---|---|---|
| `extern/godot-cpp` | `b051e694` (master, 2026-10-09; includes hot-reload fix `79c37b21`) | MIT |
| `extern/JoltPhysics` | `v5.6.0`, built with `CROSS_PLATFORM_DETERMINISTIC` | MIT |
| `extern/enet` | `v1.3.18` | MIT |
| `extern/recastnavigation` | `v1.6.0` | zlib |
| `extern/doctest` | `v2.5.3` | MIT |

Follow-ups: GitHub Actions CI (configure, build, ctest, headless self-test without an install); the Linux build and
its symbol-visibility test for our Jolt next to Godot's.
