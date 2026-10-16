# bf3_anim: skeletons, clip sampling to poses, model space, skinning binds (plan §2.2, §3.3). Godot-free.
# Inputs come from bf3_core (Ant rig and clips, EBX SkeletonAssets, skinned MeshSets).
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

# S3 crowd (docs/design/s3.md §5, §11, §15): the 3p crowd clip set, run blend, layers and masks, temporal LOD, the
# pose evaluator and the staged crowd.
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

# M2a w2 (W2.0 contract, docs/design/m2a.md §6.5, §11.1, §15): the server pose of the hit capsules (lc) and the 1p
# viewmodel animator (vm).
target_sources(bf3_anim PRIVATE
    include/bf3/anim/server_pose.h
    include/bf3/anim/viewmodel_anim.h
)
