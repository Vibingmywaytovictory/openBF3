# bf3_core: install mount and readers. Godot-free.
# S0: InstallReader, PathGuard, install snapshot.
# M0.1: containers (layout/toc/sb/cas.cat/bundles/delta/chunks) and EBX, ported from IW4L.
add_library(bf3_core STATIC
    src/bundle_manifest.cpp
    src/db_object.cpp
    src/db_record.h
    src/delta_bundle.cpp
    src/ebx.cpp
    src/ebx_db.cpp
    src/format_errors.h
    src/ids.cpp
    src/install.cpp
    src/install_reader.cpp
    src/install_snapshot.cpp
    src/path_guard.cpp
    src/payload.cpp
    src/read_only_file.cpp
    src/res_types.cpp
    src/sb_formats.cpp
    src/sha1_digest.cpp
    src/superbundle_mount.cpp
    src/thread_pool.cpp
    src/win32_text.cpp
    src/win32_text.h
    include/bf3/core/bundle_manifest.h
    include/bf3/core/db_object.h
    include/bf3/core/delta_bundle.h
    include/bf3/core/ebx.h
    include/bf3/core/ebx_db.h
    include/bf3/core/ids.h
    include/bf3/core/install.h
    include/bf3/core/install_file.h
    include/bf3/core/install_reader.h
    include/bf3/core/install_snapshot.h
    include/bf3/core/path_guard.h
    include/bf3/core/payload.h
    include/bf3/core/read_only_file.h
    include/bf3/core/res_types.h
    include/bf3/core/result.h
    include/bf3/core/sb_formats.h
    include/bf3/core/sha1_digest.h
    include/bf3/core/superbundle_mount.h
    include/bf3/core/thread_pool.h
)
target_include_directories(bf3_core PUBLIC include)
target_link_libraries(bf3_core PRIVATE libdeflate::libdeflate_static)
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
