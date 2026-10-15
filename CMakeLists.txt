# openBF3: reimplementation of Battlefield 3 multiplayer on Godot 4.8 (C++ GDExtension).
# Plan: research/PORTING-RESEARCH.md §2.2 (modules) and §2.8 (build and pinning).
#
# Configure and build with the presets (CMakePresets.json) from a VS x64 developer shell,
# or with tools\build.cmd, which sets that shell up. Build trees live under b\ (short on
# purpose: godot-cpp's generated object paths break MAX_PATH from deep roots).
cmake_minimum_required(VERSION 3.25)
project(openBF3 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "S0 supports Windows + MSVC only (the Linux build is a later step).")
endif()

option(OPENBF3_BUILD_GODOT "Build the GDExtension (src/bf3_godot, needs godot-cpp)" ON)
option(OPENBF3_BUILD_TESTS "Build the doctest executables (tests/)" ON)
set(OPENBF3_ENGINE_API "${CMAKE_SOURCE_DIR}/engine-api/4.8.dev7.c971f93e7" CACHE PATH
    "Folder with extension_api.json + gdextension_interface.json dumped from the exact Godot build")
set(OPENBF3_GODOT_PROJECT "${CMAKE_SOURCE_DIR}/bf3-opensource" CACHE PATH
    "Godot project; the extension DLL is written to its bin/ folder")

# ---------------------------------------------------------------------------------------------
# One ABI for everything linked into the extension: static CRT, no C++ exceptions, C++20.
# godot-cpp only sets CMAKE_MSVC_RUNTIME_LIBRARY when it is not cached yet, and Jolt's
# USE_STATIC_MSVC_RUNTIME_LIBRARY produces the same value, so all targets agree (/MT, /MTd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime (static)" FORCE)
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
add_compile_definitions(_HAS_EXCEPTIONS=0)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/wd4577> /utf-8)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# AddressSanitizer build of the Godot-free libraries, tools and tests (preset `asan`). MSVC's
# ASan rejects the /RTC run-time checks, so they are stripped from every configuration's flags.
option(OPENBF3_ASAN "Build with /fsanitize=address (tests and tools only; not with the GDExtension)" OFF)
if(OPENBF3_ASAN)
    if(OPENBF3_BUILD_GODOT)
        message(FATAL_ERROR "OPENBF3_ASAN needs OPENBF3_BUILD_GODOT=OFF (use the asan preset).")
    endif()
    foreach(_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${_var} "${${_var}}")
    endforeach()
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/DEBUG /INCREMENTAL:NO)
endif()

# ---------------------------------------------------------------------------------------------
# Third-party (git submodules, pinned; see README.md). All permissive: MIT / zlib.

# Jolt Physics 5.x: our own simulation world (plan §2.3, §2.5). Deterministic, library only.
set(CROSS_PLATFORM_DETERMINISTIC ON)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON)
set(CPP_EXCEPTIONS_ENABLED OFF)
set(CPP_RTTI_ENABLED OFF)
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(ENABLE_ALL_WARNINGS OFF)
set(ENABLE_INSTALL OFF)
set(GENERATE_DEBUG_SYMBOLS OFF)
set(OVERRIDE_CXX_FLAGS OFF)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK OFF)
set(JPH_USE_MTL OFF)
add_subdirectory(extern/JoltPhysics/Build jolt EXCLUDE_FROM_ALL)

# ENet 1.3.18 (upstream, not Godot's copy: plan §2.5 networking row).
add_subdirectory(extern/enet enet EXCLUDE_FROM_ALL)
target_include_directories(enet INTERFACE "${CMAKE_SOURCE_DIR}/extern/enet/include")
target_link_libraries(enet INTERFACE ws2_32 winmm)
target_compile_options(enet PRIVATE /wd5287 /wd4996) # upstream enum-mix and winsock deprecation noise

# Recast/Detour: built and linked from S0, used from the bots/navigation milestones on.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
# v1.6.0 declares cmake_minimum_required(3.0), which CMake 4 rejects without this floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(extern/recastnavigation recast EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# libdeflate 1.26: inflate of the zlib-framed BF3 payload blocks (bf3_core payload.cpp).
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
# It declares cmake_minimum_required(3.10); 3.15 makes it honour CMAKE_MSVC_RUNTIME_LIBRARY (CMP0091).
set(CMAKE_POLICY_VERSION_MINIMUM 3.15)
add_subdirectory(extern/libdeflate libdeflate EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

if(OPENBF3_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(extern/doctest doctest EXCLUDE_FROM_ALL)
endif()

if(OPENBF3_BUILD_GODOT)
    # Bindings come from the dumps of the exact engine build, both the API and the interface
    # JSON (the custom API option alone keeps godot-cpp's bundled interface JSON; probe §8.1).
    set(GODOTCPP_CUSTOM_API_FILE "${OPENBF3_ENGINE_API}/extension_api.json" CACHE FILEPATH "pinned engine API" FORCE)
    set(GODOTCPP_GDEXTENSION_DIR "${OPENBF3_ENGINE_API}" CACHE PATH "pinned interface JSON" FORCE)
    add_subdirectory(extern/godot-cpp gcpp EXCLUDE_FROM_ALL)
    # godot-cpp points its PDB at ${CMAKE_SOURCE_DIR}/bin; keep the source tree clean.
    set_target_properties(godot-cpp PROPERTIES PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>")
    # Consumers see its headers as system headers (no /W4 noise from generated bindings).
    set_target_properties(godot-cpp PROPERTIES SYSTEM ON)
endif()

# ---------------------------------------------------------------------------------------------
# Our modules. Only bf3_godot sees Godot; the rest also link into tools and tests.
function(openbf3_module target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endfunction()

add_subdirectory(src/bf3_core)
add_subdirectory(src/bf3_anim) # skeletons, poses, skinning binds over bf3_core's Ant/EBX/res readers
add_subdirectory(src/bf3_render) # Godot-free mesh/texture/material preparation for the Godot bridge
add_subdirectory(src/bf3_sim)
add_subdirectory(src/bf3_net)
add_subdirectory(src/bf3_host)
# Command-line tools (link the Godot-free libraries only).
add_subdirectory(tools/bf3dump)
add_subdirectory(tools/antdump) # Godot-free CLI over bf3_core's Ant reader
add_subdirectory(tools/resdump) # Godot-free CLI over bf3_core's resource decoders
if(OPENBF3_BUILD_GODOT)
    add_subdirectory(src/bf3_godot)
endif()

if(OPENBF3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# M2a VFX1 (docs/design/m2a-vfx1.md): bf3_fx, the Godot-free effect compiler and particle simulator (client only).
# Added after the tools and tests on purpose: link edges to it resolve at generate time, and an appended block keeps
# this shared file's existing lines untouched.
add_subdirectory(src/bf3_fx)
