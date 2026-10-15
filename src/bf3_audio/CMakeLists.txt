# M2a audio (docs/design/m2a-audio.md): bf3_audio, the Godot-free client audio runtime: patch instances (the A2
# interpreter), the mixer core the Godot AudioStreamPlayback pulls from, DSP, WAV recording and analysis, the
# weapon seam. Reads compiled programs and wave banks from bf3_core's audio/ readers.
add_library(bf3_audio STATIC
    src/audio_config.cpp
    include/bf3/audio/audio_config.h
    include/bf3/audio/audio_analysis.h
    include/bf3/audio/dsp.h
    include/bf3/audio/mixer_core.h
    include/bf3/audio/patch_instance.h
    include/bf3/audio/wav_io.h
    include/bf3/audio/weapon_sound.h
)
target_include_directories(bf3_audio PUBLIC include)
target_link_libraries(bf3_audio PUBLIC bf3_core)
openbf3_module(bf3_audio)
