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

# Derived-data cache (plan §2.1/§2.4, gap load §6.4): digest-keyed entries under an absolute root.
target_sources(bf3_core PRIVATE
    src/cache/cache_fs.h
    src/cache/cache_fs_win32.cpp
    src/cache/cache_journal.h
    src/cache/cache_journal.cpp
    src/cache/cache_key.cpp
    src/cache/digest_win32.cpp
    src/cache/disk_cache.cpp
    include/bf3/core/cache/cache_key.h
    include/bf3/core/cache/digest.h
    include/bf3/core/cache/disk_cache.h
    include/bf3/core/cache/entry_format.h
)
target_link_libraries(bf3_core PRIVATE bcrypt)
