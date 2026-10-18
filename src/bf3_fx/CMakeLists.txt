# bf3_fx: BF3's visual effects, Godot-free and client only (plan §2.2, §3.14, D19; docs/design/m2a-vfx1.md): the
# effect compiler (EffectBlueprint / EmitterDocument -> EffectDef, digest-cached), the CPU particle simulator, the
# per-level emitter atlas and the impact table from the material grid. Reads through bf3_core only. Never linked into
# bf3_sim, bf3_net or bf3_host: a dedicated server runs no effects.
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

# M2a w2 (W2.0 contract, docs/design/m2a.md §12.1, §15): VFX2's render preparation and prepared atlas pixels (fx2).
target_sources(bf3_fx PRIVATE
    include/bf3/fx/render_prep.h
    include/bf3/fx/atlas_pixels.h
)

# M2a fx2 (docs/design/m2a-fx2.md): VFX2's render preparation and the prepared atlas pixels.
target_sources(bf3_fx PRIVATE
    src/render_prep.cpp
    src/atlas_pixels.cpp
)

# M2a w3 (W3.0 contract, docs/design/m2a-w3.md; owner fx3): VFX3's triggers. They read the client's presentation
# types (PresentEvent, PresentClock, the effect seeds of client_projectiles.h), so bf3_fx links bf3_host; bf3_host
# and every server module still never link bf3_fx (VFX4 rule).
target_sources(bf3_fx PRIVATE
    include/bf3/fx/triggers.h
)
target_link_libraries(bf3_fx PUBLIC bf3_host)

# M2a fx3 (docs/design/m2a-fx3.md): VFX3's triggers (bf3/fx/triggers.h).
target_sources(bf3_fx PRIVATE
    src/triggers.cpp
)
