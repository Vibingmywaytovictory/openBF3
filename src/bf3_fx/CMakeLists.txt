add_library(bf3_fx STATIC
    src/fx_types.cpp
    src/effect_compiler.cpp
    src/effect_cache.cpp
    src/effect_library.cpp
    src/emitter_atlas.cpp
    src/impact_table.cpp
    src/simulator.cpp
    src/fx_harness.cpp
    src/fx_ebx.h
    src/fx_internal.h
    include/bf3/fx/fx_types.h
    include/bf3/fx/effect_def.h
    include/bf3/fx/effect_compiler.h
    include/bf3/fx/effect_cache.h
    include/bf3/fx/effect_library.h
    include/bf3/fx/emitter_atlas.h
    include/bf3/fx/impact_table.h
    include/bf3/fx/simulator.h
    include/bf3/fx/fx_harness.h
)
target_include_directories(bf3_fx PUBLIC include)
target_link_libraries(bf3_fx PUBLIC bf3_core)
openbf3_module(bf3_fx)

target_sources(bf3_fx PRIVATE
    include/bf3/fx/render_prep.h
    include/bf3/fx/atlas_pixels.h
)

target_sources(bf3_fx PRIVATE
    src/render_prep.cpp
    src/atlas_pixels.cpp
)

target_sources(bf3_fx PRIVATE
    include/bf3/fx/triggers.h
)
target_link_libraries(bf3_fx PUBLIC bf3_host)

target_sources(bf3_fx PRIVATE
    src/triggers.cpp
)
