@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S Q:\gdext-probe -B Q:\bd -G Ninja -DCMAKE_BUILD_TYPE=Release -DGODOT_CPP_DIR=Q:\godot-cpp "-DGODOTCPP_CUSTOM_API_FILE=C:/Users/olku/BF3 reimplementation/research/godot-api/extension_api.json" -DGODOTCPP_GDEXTENSION_DIR=Q:\gdext-api -DGODOTCPP_TARGET=template_debug -DPROBE_OUT_DIR=Q:\project\bin || exit /b 1
cmake --build Q:\bd || exit /b 1
