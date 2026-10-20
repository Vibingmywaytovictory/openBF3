add_library(bf3_anim STATIC
    src/pose_math.cpp
    src/skeleton.cpp
    src/pose.cpp
    src/skinning.cpp
    src/clip_asset.cpp
    include/bf3/anim/pose_math.h
    include/bf3/anim/skeleton.h
    include/bf3/anim/pose.h
    include/bf3/anim/skinning.h
    include/bf3/anim/clip_asset.h
)
target_include_directories(bf3_anim PUBLIC include)
target_link_libraries(bf3_anim PUBLIC bf3_core)
openbf3_module(bf3_anim)

target_sources(bf3_anim PRIVATE
    src/crowd_clips.cpp
    src/crowd_eval.cpp
    src/crowd_stage.cpp
    src/layers.cpp
    src/locomotion.cpp
    src/temporal_lod.cpp
    include/bf3/anim/crowd_clips.h
    include/bf3/anim/crowd_eval.h
    include/bf3/anim/crowd_stage.h
    include/bf3/anim/layers.h
    include/bf3/anim/locomotion.h
    include/bf3/anim/temporal_lod.h
)

target_sources(bf3_anim PRIVATE
    include/bf3/anim/server_pose.h
    include/bf3/anim/viewmodel_anim.h
)

target_sources(bf3_anim PRIVATE
    src/server_pose.cpp
    include/bf3/anim/server_pose_reference.h
)

target_sources(bf3_anim PRIVATE
    src/viewmodel_anim.cpp
)

target_sources(bf3_anim PRIVATE
    src/move_group.cpp
    include/bf3/anim/move_group.h
)
