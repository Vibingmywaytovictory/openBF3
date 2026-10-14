# bf3_core: install mount and readers. Godot-free. S0: InstallReader, PathGuard, install snapshot.
add_library(bf3_core STATIC
    src/install_reader.cpp
    src/install_snapshot.cpp
    src/path_guard.cpp
    src/read_only_file.cpp
    src/win32_text.cpp
    src/win32_text.h
    include/bf3/core/install_reader.h
    include/bf3/core/install_snapshot.h
    include/bf3/core/path_guard.h
    include/bf3/core/read_only_file.h
)
target_include_directories(bf3_core PUBLIC include)
openbf3_module(bf3_core)
