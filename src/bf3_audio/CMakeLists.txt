add_library(bf3_audio STATIC
    src/audio_analysis.cpp
    src/audio_check.cpp
    src/audio_config.cpp
    src/audio_load.cpp
    src/dsp.cpp
    src/mixer_core.cpp
    src/patch_instance.cpp
    src/patch_nodes.cpp
    src/patch_sampler.cpp
    src/patch_state.h
    src/wav_io.cpp
    src/weapon_sound.cpp
    include/bf3/audio/audio_config.h
    include/bf3/audio/audio_analysis.h
    include/bf3/audio/audio_check.h
    include/bf3/audio/audio_load.h
    include/bf3/audio/dsp.h
    include/bf3/audio/mixer_core.h
    include/bf3/audio/patch_instance.h
    include/bf3/audio/wav_io.h
    include/bf3/audio/weapon_sound.h
)
target_include_directories(bf3_audio PUBLIC include)
target_link_libraries(bf3_audio PUBLIC bf3_core)
openbf3_module(bf3_audio)

target_sources(bf3_audio PRIVATE
    include/bf3/audio/weapon_audio.h
    include/bf3/audio/impact_audio.h
    include/bf3/audio/game_audio_check.h
)
target_link_libraries(bf3_audio PUBLIC bf3_present)

target_sources(bf3_audio PRIVATE
    src/weapon_audio.cpp
    src/impact_audio.cpp
    src/game_audio_check.cpp
)

target_sources(bf3_audio PRIVATE
    include/bf3/audio/hdr_window.h
    include/bf3/audio/voice_cull.h
)

target_sources(bf3_audio PRIVATE
    src/hdr_window.cpp
    src/voice_cull.cpp
)

target_sources(bf3_audio PRIVATE
    src/weapon_foley.cpp
    include/bf3/audio/weapon_foley.h
)
