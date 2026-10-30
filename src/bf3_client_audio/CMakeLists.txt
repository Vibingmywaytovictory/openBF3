add_library(bf3_client_audio STATIC
    src/game_audio_frame.cpp
    src/kit_audio.cpp
    src/sound_fate_ledger.cpp
    src/area_feed.cpp
    src/area_wave_observer.cpp
    include/bf3/client_audio/game_audio_frame.h
    include/bf3/client_audio/kit_audio.h
    include/bf3/client_audio/sound_fate_ledger.h
    include/bf3/client_audio/area_feed.h
    include/bf3/client_audio/area_wave_observer.h
)
target_include_directories(bf3_client_audio PUBLIC include)
target_link_libraries(bf3_client_audio PUBLIC bf3_audio bf3_host)
openbf3_module(bf3_client_audio)
