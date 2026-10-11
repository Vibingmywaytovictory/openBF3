# gdext-probe

A one-node GDExtension (C++, godot-cpp) that proves the toolchain against Godot **4.8.dev7.official.c971f93e7** and
measures the engine APIs the BF3 reimplementation depends on. Results and analysis:
`..\..\godot-gdextension-probe.md`. It never touches the BF3 install or the user's project.

## Pinned inputs

| input | value |
|---|---|
| Godot | `Godot_v4.8-dev7_win64_console.exe`, version hash `c971f93e7e76b0ef919bf6009e7b868bea04db7f`, precision single |
| godot-cpp | https://github.com/godotengine/godot-cpp `master` @ `b051e694cf493d417f5aa945c656cb77ae9011a9` (2026-10-09; = tag `10.0.0-stable` `507ed9d8` + 8 commits incl. "Fix for hot reload crash") |
| API JSON | `research\godot-api\extension_api.json` (byte-identical to `--dump-extension-api` of the exe; SHA-256 `B9B10796...59AFA1`) |
| interface JSON | `gdextension_interface.json` dumped by the same exe: `Godot_v4.8-dev7_win64_console.exe --headless --dump-extension-api --dump-gdextension-interface-json` (run in an empty scratch dir; writes both files to the CWD) |
| compiler | MSVC 19.51.36260 (VS 18 Community, toolset 14.51.36231), CMake 4.3.3, Ninja 1.13.2 (the one bundled with VS), Python 3.14.4 |

## Build

From `cmd` (not PowerShell), in a **short path** (godot-cpp generates 2,201 files with names such as
`open_xr_spatial_capability_configuration_plane_tracking.cpp.obj`; under a long build path MSVC fails with
`fatal error C1083: Cannot open compiler generated file: '': Invalid argument` at 261 characters). The probe used
`subst Q: <scratch>\probe-godot`; `build-example.bat` shows the exact commands:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S Q:\gdext-probe -B Q:\bd -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DGODOT_CPP_DIR=Q:\godot-cpp ^
  "-DGODOTCPP_CUSTOM_API_FILE=C:/Users/olku/BF3 reimplementation/research/godot-api/extension_api.json" ^
  -DGODOTCPP_GDEXTENSION_DIR=Q:\gdext-api ^
  -DGODOTCPP_TARGET=template_debug ^
  -DPROBE_OUT_DIR=Q:\project\bin
cmake --build Q:\bd
```

`GODOTCPP_TARGET=template_release` (separate build dir) gives the release DLL. Output names follow godot-cpp's
suffix: `libgdext_probe.windows.template_debug.x86_64.dll`.

## Test project

Copy the user's empty project (`bf3-opensource`) to scratch, then add `project-files\bin\gdext_probe.gdextension`,
`project-files\probe.tscn`, set `run/main_scene="res://probe.tscn"` and (for texture streaming)
`rendering/textures/streaming/enabled=true`; change `config/name` so `user://` does not collide with the real
project's. Then **once**: `Godot_v4.8-dev7_win64_console.exe --headless --path <copy> --import` (writes
`.godot\extension_list.cfg`; without it the class is not registered, the scene gets a placeholder node and the run
never quits).

Run: `Godot_v4.8-dev7_win64_console.exe [--headless] --path <copy> -- [args]`. User args:
`--nav=250,500,1000` (sync bake sizes, last one also async + chunked), `--cell=0.25`, `--chunks-only`,
`--border=1`, `--chunk=250`, `--no-monotone`, `--skip-nav`, `--skip-race`, `--frames=120`.
Every result line starts with `[probe]`; the node quits by itself (exit 0) and has a 15-minute watchdog.
`override.cfg` in the project copy is a convenient way to flip `display/window/vsync/vsync_mode=0` or streaming.
