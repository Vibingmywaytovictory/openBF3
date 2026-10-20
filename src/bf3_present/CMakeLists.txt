add_library(bf3_present STATIC
    src/presentation_events.cpp
    src/effect_seed.cpp
    include/bf3/present/presentation_events.h
    include/bf3/present/presented.h
    include/bf3/present/effect_seed.h
)
target_include_directories(bf3_present PUBLIC include)
target_link_libraries(bf3_present PUBLIC bf3_sim)
openbf3_module(bf3_present)
