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
