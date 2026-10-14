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

# Resource decoders (MeshSet, DxTexture, Havok physics) over caller-supplied bytes; see
# include/bf3/core/res/res_types.h. Port of IW4L resource_bf3.
target_sources(bf3_core PRIVATE
    src/res/res_common.cpp
    src/res/res_reader.h
    src/res/mesh_set.cpp
    src/res/dx_texture.cpp
    src/res/havok.cpp
    src/res/havok_geometry.cpp
    include/bf3/core/res/res_types.h
    include/bf3/core/res/res_math.h
    include/bf3/core/res/mesh_set.h
    include/bf3/core/res/dx_texture.h
    include/bf3/core/res/havok.h
)
