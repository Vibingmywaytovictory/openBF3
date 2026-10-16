# doctest executables for the Godot-free libraries. Run with: ctest --preset dev
function(openbf3_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    # Exceptions are off project-wide; a failing REQUIRE then aborts the executable.
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
    openbf3_module(${name})
endfunction()

openbf3_test(test_core test_main.cpp core_guard_tests.cpp core_install_tests.cpp core_format_tests.cpp core_container_tests.cpp core_ebx_tests.cpp core_fuzz_tests.cpp test_support.h)
target_link_libraries(test_core PRIVATE bf3_core)
openbf3_test(test_cache test_main.cpp core_cache_tests.cpp core_cache_data_tests.cpp test_support.h)
target_link_libraries(test_cache PRIVATE bf3_core)
openbf3_test(test_sim test_main.cpp sim_tests.cpp test_support.h)
target_link_libraries(test_sim PRIVATE bf3_sim)
openbf3_test(test_net test_main.cpp net_tests.cpp test_support.h)
target_link_libraries(test_net PRIVATE bf3_net)
openbf3_test(test_host test_main.cpp host_tests.cpp test_support.h)
target_link_libraries(test_host PRIVATE bf3_host)

# Ant animation reader (bf3_core/ant) + the antdump support library used for census and JSON comparison.
openbf3_test(test_ant test_main.cpp ant_tests.cpp test_support.h)
target_link_libraries(test_ant PRIVATE bf3_core antdump_lib)

openbf3_test(test_res test_main.cpp res_tests.cpp test_support.h)
target_link_libraries(test_res PRIVATE bf3_core)

add_test(NAME core.guard COMMAND test_core --test-suite-exclude=install,containers,fuzz)
# Container readers on synthetic bytes (no install).
add_test(NAME core.formats COMMAND test_core --test-suite=containers)
# Reads the retail install (read-only); exits 77 = skipped when no install is found.
add_test(NAME core.install COMMAND test_core --test-suite=install)
set_tests_properties(core.install PROPERTIES SKIP_RETURN_CODE 77)
# Seeded mutations of real install bytes through every parser (meant for the asan preset; skips as above).
add_test(NAME core.fuzz COMMAND test_core --test-suite=fuzz)
set_tests_properties(core.fuzz PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME core.cache COMMAND test_cache --test-suite-exclude=cache_data)
# Real payloads exported outside the repo (OPENBF3_CACHE_TESTDATA); exits 77 = skipped without them.
add_test(NAME core.cache_data COMMAND test_cache --test-suite=cache_data)
# Resource decoders: synthetic cases, then the sample the parity oracle exported from the install
# (OPENBF3_RES_DATA, see README "Resource decoders"); exits 77 = skipped without it.
add_test(NAME core.res.unit COMMAND test_res --test-suite=res_unit)
add_test(NAME core.res COMMAND test_res --test-suite=res)
set_tests_properties(core.res PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME sim.jolt COMMAND test_sim)
add_test(NAME net.enet COMMAND test_net)
add_test(NAME host.server COMMAND test_host)
# bf3dump's JSON number text (the probe's f32/f64 rules) on synthetic values; no install needed.
add_test(NAME tools.numtext COMMAND bf3dump numcheck)
# core.ant reads the Ant banks from the install (or the export directory OPENBF3_ANT_DATA) and, when
# OPENBF3_ANT_ORACLE is set, the probe's clip JSONs; exits 77 = skipped when neither input exists. core.ant.unit
# needs no data.
add_test(NAME core.ant.unit COMMAND test_ant --test-suite=ant-unit)
add_test(NAME core.ant COMMAND test_ant --test-suite=ant)
set_tests_properties(core.ant PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.guard core.formats core.install core.fuzz core.cache sim.jolt net.enet host.server tools.numtext core.ant.unit core.ant PROPERTIES TIMEOUT 120)
# core.install also reads every bundle manifest and decodes sample payloads (slower in Debug).
set_tests_properties(core.install PROPERTIES TIMEOUT 600)
set_tests_properties(core.cache_data PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
set_tests_properties(core.res.unit core.res PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    # The ASan runtime DLL ships next to cl.exe; put it on the tests' PATH.
    get_filename_component(_openbf3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.guard core.formats core.install core.fuzz core.cache core.cache_data sim.jolt net.enet host.server tools.numtext core.ant.unit core.ant core.res.unit core.res
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_cl_dir}")
endif()

# Resource decoders fed straight from the install through bf3/core/res/res_loader.h (read-only):
# every MeshSet / DxTexture / Havok res of MP_Subway and the M9 1p mesh; exits 77 = skipped without
# an install.
openbf3_test(test_res_install test_main.cpp res_install_tests.cpp test_support.h)
target_link_libraries(test_res_install PRIVATE bf3_core)
add_test(NAME core.res.install COMMAND test_res_install)
set_tests_properties(core.res.install PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(core.res.install PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_cl_dir}")
endif()
# Weapon tables (bf3_core/gameplay): unit arithmetic, then the §7.1 oracles read from the install
# (exits 77 = skipped without one) and seeded mutations of the M16A4 partitions (meant for asan).
openbf3_test(test_gameplay test_main.cpp gameplay_tests.cpp test_support.h)
target_link_libraries(test_gameplay PRIVATE bf3_core)
add_test(NAME core.gameplay.unit COMMAND test_gameplay --test-suite=gameplay-unit)
add_test(NAME core.gameplay.weapons COMMAND test_gameplay --test-suite=gameplay)
add_test(NAME core.gameplay.fuzz COMMAND test_gameplay --test-suite=gameplay-fuzz)
set_tests_properties(core.gameplay.weapons core.gameplay.fuzz PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.gameplay.unit core.gameplay.weapons core.gameplay.fuzz PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_gameplay_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.gameplay.unit core.gameplay.weapons core.gameplay.fuzz
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_gameplay_cl_dir}")
endif()
# UITextDatabase reader and EBX SID census (bf3_core ui/). core.ui.text.unit is synthetic (no data);
# core.ui.text checks the retail database (plan §7.8) and core.ui.text.fuzz mutates its chunks in memory;
# both exit 77 = skipped without an install.
openbf3_test(test_text test_main.cpp text_tests.cpp test_support.h)
target_link_libraries(test_text PRIVATE bf3_core)
add_test(NAME core.ui.text.unit COMMAND test_text --test-suite=text-unit)
add_test(NAME core.ui.text COMMAND test_text --test-suite=text)
add_test(NAME core.ui.text.fuzz COMMAND test_text --test-suite=text-fuzz)
set_tests_properties(core.ui.text core.ui.text.fuzz PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.ui.text.unit core.ui.text core.ui.text.fuzz PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_text_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.ui.text.unit core.ui.text core.ui.text.fuzz
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_text_cl_dir}")
endif()
# bf3_anim (skeletons, poses, skinning binds): anim.unit needs no data; anim reads the install (Ant banks,
# EBX skeletons, the 1P arms and M9 meshes) and exits 77 = skipped without one. With OPENBF3_ANT_ORACLE it
# also compares model-space poses against the Ant probe's ReloadEmpty JSON.
openbf3_test(test_anim test_main.cpp anim_tests.cpp test_support.h)
target_link_libraries(test_anim PRIVATE bf3_anim antdump_lib)
add_test(NAME anim.unit COMMAND test_anim --test-suite=anim-unit)
add_test(NAME anim COMMAND test_anim --test-suite=anim)
set_tests_properties(anim PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(anim.unit anim PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_anim_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(anim.unit anim PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_anim_cl_dir}")
endif()
# bf3_render (Godot-free preparation for the Godot bridge): render.unit needs no data; render loads the M9
# viewmodel asset set from the install and exits 77 = skipped without one.
openbf3_test(test_render test_main.cpp render_tests.cpp test_support.h)
target_link_libraries(test_render PRIVATE bf3_render)
add_test(NAME render.unit COMMAND test_render --test-suite=render-unit)
add_test(NAME render COMMAND test_render --test-suite=render)
set_tests_properties(render PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(render.unit render PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_render_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(render.unit render PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_render_cl_dir}")
endif()
# The Godot-side M9 reload check (res://scenes/m9_reload.tscn -- --m9-reload-check): Skeleton3D poses vs bf3_anim
# and the probe JSON, skin, materials, install unchanged. Runs when OPENBF3_GODOT names the Godot console exe;
# exits 77 = skipped otherwise or without an install. Needs the bf3_godot DLL (built with the default preset).
if(OPENBF3_BUILD_GODOT)
    add_test(NAME godot.m9_reload COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_m9_reload.cmake)
    set_tests_properties(godot.m9_reload PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
# WeaponDef block and weapon catalog shared by bf3dump and the in-game inspector (bf3_core gameplay/
# weapon_format.h, weapon_catalog.h). core.gameplay.inspect.unit is synthetic; core.gameplay.inspect reads
# MP_Subway and the text database from the install (exits 77 = skipped without one).
openbf3_test(test_inspect test_main.cpp inspect_tests.cpp test_support.h)
target_link_libraries(test_inspect PRIVATE bf3_core)
add_test(NAME core.gameplay.inspect.unit COMMAND test_inspect --test-suite=inspect-unit)
add_test(NAME core.gameplay.inspect COMMAND test_inspect --test-suite=inspect)
set_tests_properties(core.gameplay.inspect PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.gameplay.inspect.unit core.gameplay.inspect PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_inspect_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.gameplay.inspect.unit core.gameplay.inspect
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_inspect_cl_dir}")
endif()
# The Godot project run headless (OPENBF3_GODOT = the console executable; exits 77 = skipped when unset):
# godot.s0_selftest (S0 self-test) and godot.inspector (M0 exit: the inspector's M16A3/M16A4-burst check
# against plan §7.1 and their localised names). Both use the project and its DLL, so they never run at once.
# cmake_language(EXIT) needs CMake 3.29.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME godot.s0_selftest COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -DUSER_ARGS=--s0-selftest
        "-DPASS_LINE=S0 SELFTEST: PASS" -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_check.cmake)
    add_test(NAME godot.inspector COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -DUSER_ARGS=--m0-inspector-check
        "-DPASS_LINE=M0 INSPECTOR CHECK: PASS" -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_check.cmake)
    set_tests_properties(godot.s0_selftest godot.inspector PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 RESOURCE_LOCK openbf3_godot_project)
    if(TEST godot.m9_reload)
        set_tests_properties(godot.m9_reload PROPERTIES RESOURCE_LOCK openbf3_godot_project)
    endif()
endif()
# Level traversal (bf3_core/level): level_unit on synthetic values; core.level reads the install
# (read-only) against handoff §9 counts and exits 77 = skipped without one.
openbf3_test(test_level test_main.cpp level_tests.cpp test_support.h)
target_link_libraries(test_level PRIVATE bf3_core)
add_test(NAME core.level.unit COMMAND test_level --test-suite=level_unit)
add_test(NAME core.level COMMAND test_level --test-suite=level)
set_tests_properties(core.level PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.level.unit core.level PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(core.level.unit core.level PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_cl_dir}")
endif()
# Soldier, kit, mode and vehicle tables (bf3_core/gameplay). core.gameplay.tables.unit is synthetic (no
# data, incl. the hostile work-budget cases); core.gameplay.tables reads the retail install (plan §7.2-§7.4),
# exits 77 = skipped without one.
openbf3_test(test_gameplay_tables test_main.cpp gameplay_tables_tests.cpp test_support.h)
target_link_libraries(test_gameplay_tables PRIVATE bf3_core)
add_test(NAME core.gameplay.tables.unit COMMAND test_gameplay_tables --test-suite=tables-unit)
add_test(NAME core.gameplay.tables COMMAND test_gameplay_tables --test-suite=tables)
set_tests_properties(core.gameplay.tables PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.gameplay.tables.unit core.gameplay.tables PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_tables_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.gameplay.tables.unit core.gameplay.tables
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_tables_cl_dir}")
endif()
# Collision soup (bf3_core level/collision_soup.h): collision_unit on synthetic shapes and terrain trees; core.level.collision
# reads MP_Subway and XP3_Alborz from the install (read-only) against the level-oracle numbers and exits
# 77 = skipped without one.
openbf3_test(test_collision test_main.cpp collision_tests.cpp terrain_tests.cpp test_support.h)
target_link_libraries(test_collision PRIVATE bf3_core)
add_test(NAME core.level.collision.unit COMMAND test_collision --test-suite=collision_unit)
add_test(NAME core.level.collision COMMAND test_collision --test-suite=collision)
set_tests_properties(core.level.collision PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.level.collision.unit core.level.collision PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(core.level.collision.unit core.level.collision PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_cl_dir}")
    # Under ASan the 32 mutated group res take ~9 min and the two levels ~1 min (dev: 17 s in all).
    set_tests_properties(core.level.collision PROPERTIES TIMEOUT 1800)
endif()
# Godot collision overlay (M0 exit "Metro collision drawn as a debug overlay"; README "Collision overlay"):
# the headless --collision-check of res://scenes/metro_collision.tscn. Exits 77 = skipped without
# OPENBF3_GODOT or an install; shares the project and its DLL with the other godot.* tests.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME godot.collision COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_collision.cmake)
    set_tests_properties(godot.collision PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360 RESOURCE_LOCK openbf3_godot_project)
endif()
# Converters through the derived-data cache and `bf3dump prepare` (bf3_core cache/cached_convert.h, gameplay/
# weapon_blob.h; bf3_render mesh_cache.h, level_prepare.h). prepare.unit: blob round trips, hostile blobs, keys,
# prepare twice / damaged entries on scratch roots (no data). prepare: the M9 viewmodel meshes and weapons from the
# install into a scratch cache root (exits 77 = skipped without an install).
openbf3_test(test_prepare test_main.cpp prepare_tests.cpp test_support.h)
target_link_libraries(test_prepare PRIVATE bf3_render)
add_test(NAME prepare.unit COMMAND test_prepare --test-suite=prepare-unit)
add_test(NAME prepare COMMAND test_prepare --test-suite=prepare)
set_tests_properties(prepare PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(prepare.unit prepare PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_prepare_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(prepare.unit prepare PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_prepare_cl_dir}")
endif()
# M1 interface contract (docs/design/m1.md §14): every M1 cross-track header compiles together, the plain types
# keep their properties, the shared inline helpers (command quantisation, ticks, quantisers) behave. No data.
openbf3_test(test_m1_contract test_main.cpp m1_contract_tests.cpp test_support.h)
target_link_libraries(test_m1_contract PRIVATE bf3_host bf3_render)
add_test(NAME m1.contract COMMAND test_m1_contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_contract_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(m1.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_contract_cl_dir}")
endif()
# M1 T1 (world; docs/design/m1.md §5, §14): sim.world.unit builds synthetic static worlds and checks the
# CollisionWorld queries and the CollisionWorld::Impl seam (it includes bf3_sim's internal headers and links Jolt for
# that), the tile / world-index blobs (round trips, hostile bytes), the cache keys and the content-keyed tile cache on
# scratch roots; no data. sim.world reads Metro from the install (read-only) into a scratch cache: cold then warm,
# the source digest's coverage of the soup's sources, CollisionWorld answers against a brute-force reference over the
# soup triangles, install.unchanged; exits 77 = skipped without an install.
openbf3_test(test_sim_world test_main.cpp sim_world_tests.cpp sim_world_cache_tests.cpp sim_world_install_tests.cpp sim_world_support.h test_support.h)
target_link_libraries(test_sim_world PRIVATE bf3_sim Jolt)
target_include_directories(test_sim_world PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_sim/src)
add_test(NAME sim.world.unit COMMAND test_sim_world --test-suite=sim-world-unit)
set_tests_properties(sim.world.unit PROPERTIES TIMEOUT 300)
add_test(NAME sim.world COMMAND test_sim_world --test-suite=sim-world)
set_tests_properties(sim.world PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_world_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(sim.world.unit sim.world PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_world_cl_dir}")
    set_tests_properties(sim.world PROPERTIES TIMEOUT 1800)
endif()
# tools.m1_prepare (E6, docs §1, §12.4): `bf3dump prepare MP_Subway TeamDeathMatch0 --shapes` on a fresh scratch root,
# then again: the cold peak working set <= 512 MiB, the second run all hits; exits 77 = skipped without an install.
# RUN_SERIAL so a ctest -j neighbour cannot skew the peak. cmake_language(EXIT) needs CMake 3.29.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME tools.m1_prepare COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DE6_REPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/m1_prepare.cmake)
    set_tests_properties(tools.m1_prepare PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 RUN_SERIAL TRUE)
    if(OPENBF3_ASAN)
        get_filename_component(_openbf3_m1prep_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(tools.m1_prepare PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m1prep_cl_dir}")
    endif()
endif()
# M1 T2 (motor; docs/design/m1.md §6, §9.2, §10, §14): sim.motor.unit steps the motor on synthetic static worlds (E1
# numbers, slopes, steps, walls, ceilings, falls, transitions, determinism, interleaving, sim::catch_up, settle, the
# tuning file); sim.motor runs the same on Metro's real static world (E1a-E1d at found courses, the 102-point settle)
# and exits 77 = skipped without an install. Both read the shipped tuning file (OPENBF3_MOTOR_TUNING_CFG, §6.11).
openbf3_test(test_sim_motor test_main.cpp sim_motor_tests.cpp test_support.h)
target_link_libraries(test_sim_motor PRIVATE bf3_sim)
target_compile_definitions(test_sim_motor PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME sim.motor.unit COMMAND test_sim_motor --test-suite=sim-motor-unit)
add_test(NAME sim.motor COMMAND test_sim_motor --test-suite=sim-motor)
set_tests_properties(sim.motor PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(sim.motor.unit PROPERTIES TIMEOUT 300)
set_tests_properties(sim.motor PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_motor_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(sim.motor.unit sim.motor PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_motor_cl_dir}")
endif()

# M1 T3 net (docs/design/m1.md §7, §14): bit packing, the protocol's messages (round trips, every truncation,
# fuzzed bytes, unsequenced ENet loopback) and the seeded netsim. No data; meant for the asan preset too.
openbf3_test(test_net_proto test_main.cpp net_proto_tests.cpp test_support.h)
target_link_libraries(test_net_proto PRIVATE bf3_net)
add_test(NAME net.bits.unit COMMAND test_net_proto --test-suite=net-bits)
add_test(NAME net.proto.unit COMMAND test_net_proto --test-suite=net-proto)
add_test(NAME net.netsim.unit COMMAND test_net_proto --test-suite=net-netsim)
set_tests_properties(net.bits.unit net.proto.unit net.netsim.unit PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_t3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(net.bits.unit net.proto.unit net.netsim.unit
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_t3_cl_dir}")
endif()

# M1 T4 session (docs/design/m1.md §1 E2/E3, §7.4-§9, §14): host.session.unit runs GameServer without sockets,
# judge_rest and the presentation helpers on synthetic input, then a real ServerHost + ClientSession over ENet
# loopback on a synthetic world (spawn, clock resets, a stall, a teleport, Trace, E2 under 150/2 shortened); no
# data. host.session runs the session on Metro's real world through load_shared_level (E2 under 150/2 as the gate
# and 150 ms one way as a reported row, the 128-slot SpawnSweep against in-process rows, install unchanged) and
# exits 77 without an install. Both read the shipped tuning file and are RUN_SERIAL (wall-clock timing).
openbf3_test(test_host_session test_main.cpp host_session_tests.cpp test_support.h)
target_link_libraries(test_host_session PRIVATE bf3_host)
target_compile_definitions(test_host_session PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME host.session.unit COMMAND test_host_session --test-suite=host-session-unit)
add_test(NAME host.session COMMAND test_host_session --test-suite=host-session)
set_tests_properties(host.session.unit PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
set_tests_properties(host.session PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_t4_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(host.session.unit host.session PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_t4_cl_dir}")
endif()

# M1 T5: the level scene (bf3_render level_scene.h; docs/design/m1.md §14). render.scene.unit: synthetic census,
# meshes and heightfields (no data). render.scene: Metro TDM plan and SceneLanes cold and warm on a scratch cache
# root with its timings, texture count and bytes, peak in-flight bytes and peak working set, install.unchanged (exits
# 77 = skipped without an install). RUN_SERIAL so the printed lane timings do not compete with ctest -j neighbours.
openbf3_test(test_render_scene test_main.cpp render_scene_tests.cpp test_support.h)
target_link_libraries(test_render_scene PRIVATE bf3_render psapi)
target_include_directories(test_render_scene PRIVATE ${PROJECT_SOURCE_DIR}/src/bf3_render/src)
add_test(NAME render.scene.unit COMMAND test_render_scene --test-suite=render-scene-unit)
add_test(NAME render.scene COMMAND test_render_scene --test-suite=render-scene)
set_tests_properties(render.scene PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
set_tests_properties(render.scene.unit render.scene PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_scene_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(render.scene.unit render.scene PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_scene_cl_dir}")
endif()

# M1 T6 godot (docs/design/m1.md §1, §11, §12, §14): godot.m1 runs the headless --m1-check of
# res://scenes/m1_metro.tscn (E1 through the loopback server on Trace reports, E2 under both netsim profiles judged by
# judge_rest, E3 SpawnSweep with 128 slots, install unchanged); godot.m1_load the windowed --m1-load-check cold then
# warm on a fresh scratch cache root (E4 / E5, phase times, frame time, the game's peak working set, 8 spawn shots)
# after a probe run, and exits 77 = skipped without a window or rendering device. Both exit 77 without OPENBF3_GODOT
# or an install and share the project and its DLL with the other godot.* tests; both are RUN_SERIAL (E2 wall-clock
# netsim, E4 / E5 timing).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME godot.m1 COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_m1.cmake)
    set_tests_properties(godot.m1 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 RESOURCE_LOCK openbf3_godot_project RUN_SERIAL TRUE)
    add_test(NAME godot.m1_load COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_m1_load.cmake)
    set_tests_properties(godot.m1_load PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 RESOURCE_LOCK openbf3_godot_project RUN_SERIAL TRUE)
endif()

# H hygiene: the stop_all registries of bf3_host (src/bf3_host/src/instance_registry.h). host.registry pins the
# destructor-waits-for-stop_all rule on InstanceRegistry and churns ServerHost, LoopbackClient and ClientSession
# against a thread calling stop_all in a loop (no data; meant for the asan preset, where a use after free fails it).
openbf3_test(test_host_registry test_main.cpp host_registry_tests.cpp test_support.h)
target_link_libraries(test_host_registry PRIVATE bf3_host)
target_include_directories(test_host_registry PRIVATE ${PROJECT_SOURCE_DIR}/src/bf3_host/src)
add_test(NAME host.registry COMMAND test_host_registry --test-suite=host-registry)
set_tests_properties(host.registry PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_hy_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(host.registry PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_hy_cl_dir}")
endif()
# H hygiene: tools.ebx_memory, the EbxLibrary memory gate (tests/ebx_memory.cmake): `bf3dump ebxload @all-mp` peak
# working set <= 760 MiB and Metro's <= 145 MiB, every partition parsed; 77 = skipped without an install; report-only
# under asan. RUN_SERIAL so a ctest -j neighbour cannot skew the peak.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME tools.ebx_memory COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DREPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ebx_memory.cmake)
    set_tests_properties(tools.ebx_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 RUN_SERIAL TRUE)
    if(OPENBF3_ASAN)
        get_filename_component(_openbf3_hymem_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(tools.ebx_memory PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_hymem_cl_dir}")
    endif()
endif()
# S4 logic (docs/design/s4.md §12): s4.contract compiles every S4 level-logic header together and checks the port ids,
# the type table's classification (48 server-capable primitives, 34 in the S4 slice, 25 S4 natives), the lookups and
# the plain types. No data.
openbf3_test(test_s4_contract test_main.cpp s4_contract_tests.cpp test_support.h)
target_link_libraries(test_s4_contract PRIVATE bf3_sim)
add_test(NAME s4.contract COMMAND test_s4_contract --test-suite=s4-contract)
set_tests_properties(s4.contract PROPERTIES TIMEOUT 120)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_s4_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(s4.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s4_cl_dir}")
endif()
# S4 logic (docs/design/s4.md §12): the level-logic interpreter. logic.unit and logic.fuzz need no data (logic.fuzz is
# meant for the asan preset); logic.metro (G1-G4), logic.types (G5), logic.caspian (G6), logic.perf (G7, RUN_SERIAL,
# wall clock) and logic.census (I1, info) read the install and exit 77 without one. logic.metro runs `bf3dump logic
# trace` in a child process (G4), so it gets the bf3dump path in OPENBF3_BF3DUMP.
openbf3_test(test_logic test_main.cpp logic_unit_tests.cpp logic_fuzz_tests.cpp logic_install_tests.cpp logic_test_support.h test_support.h)
target_link_libraries(test_logic PRIVATE bf3_sim)
add_test(NAME logic.unit COMMAND test_logic --test-suite=logic-unit)
add_test(NAME logic.fuzz COMMAND test_logic --test-suite=logic-fuzz)
add_test(NAME logic.metro COMMAND test_logic --test-suite=logic-metro)
add_test(NAME logic.types COMMAND test_logic --test-suite=logic-types)
add_test(NAME logic.caspian COMMAND test_logic --test-suite=logic-caspian)
add_test(NAME logic.perf COMMAND test_logic --test-suite=logic-perf)
add_test(NAME logic.census COMMAND test_logic --test-suite=logic-census)
set_tests_properties(logic.unit logic.fuzz PROPERTIES TIMEOUT 600)
set_tests_properties(logic.metro logic.types logic.caspian logic.perf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
set_tests_properties(logic.census PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
set_tests_properties(logic.perf PROPERTIES RUN_SERIAL TRUE)
if(TARGET bf3dump)
    set_tests_properties(logic.metro PROPERTIES ENVIRONMENT "OPENBF3_BF3DUMP=$<TARGET_FILE:bf3dump>")
endif()
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_logic_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(logic.unit logic.fuzz logic.metro logic.types logic.caspian logic.perf logic.census
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_logic_cl_dir}")
endif()
# S3 crowd (docs/design/s3.md §14): s3.contract compiles every Godot-free S3 header together and checks the plain
# types, the temporal LOD table and interval rule, the phase advance on hostile numbers and the gate constants. No data.
openbf3_test(test_s3_contract test_main.cpp s3_contract_tests.cpp test_support.h)
target_link_libraries(test_s3_contract PRIVATE bf3_render)
add_test(NAME s3.contract COMMAND test_s3_contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_s3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(s3.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s3_cl_dir}")
endif()
# S3 crowd (docs/design/s3.md §14): anim.s3.unit (layers, run blend, WorkerGroup, the evaluator's frame protocol on a
# synthetic rig, the stage schedule; no data), anim.s3 (the clip set, chooser and reload, the additive-order criterion and
# the 64-soldier bench from the install) and render.s3 (the US / RU 3p asset set cold then warm on a scratch cache root).
# The install tests exit 77 = skipped without an install. All three also run under the asan preset.
openbf3_test(test_anim_s3 test_main.cpp anim_s3_tests.cpp test_support.h)
target_link_libraries(test_anim_s3 PRIVATE bf3_anim)
add_test(NAME anim.s3.unit COMMAND test_anim_s3 --test-suite=anim-s3-unit)
add_test(NAME anim.s3 COMMAND test_anim_s3 --test-suite=anim-s3)
openbf3_test(test_render_s3 test_main.cpp render_s3_tests.cpp test_support.h)
target_link_libraries(test_render_s3 PRIVATE bf3_render)
add_test(NAME render.s3 COMMAND test_render_s3 --test-suite=render-s3)
set_tests_properties(anim.s3 render.s3 PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(anim.s3.unit anim.s3 render.s3 PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(anim.s3.unit anim.s3 render.s3 PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s3_cl_dir}")
endif()
# S3 crowd (docs/design/s3.md §1, §12): godot.s3_crowd runs the windowed --s3-check of res://scenes/s3_crowd.tscn (G0 rows,
# G1a / G1b, G2 vsync off and on, G3 shots, install unchanged) after a probe run, with one repeat when only timing gates
# fail. Exits 77 = skipped without OPENBF3_GODOT, a window, an install or a 2560x1440 primary screen.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME godot.s3_crowd COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_s3_crowd.cmake)
    set_tests_properties(godot.s3_crowd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 2000 RESOURCE_LOCK openbf3_godot_project RUN_SERIAL TRUE)
endif()
# L1 look (README "L1 look"): render.look.unit, the shader database reader on a synthetic res (fields, every
# truncation, single-byte mutations, the work budget), the look resolver's tiers and rules on a synthetic lookup, and
# the visual environment arithmetic (fog fit, area weights, blend, grade); no data. render.look: Metro TDM's shader
# database parsed with its work units, the resolver and the environment read, install.unchanged (77 without one).
openbf3_test(test_look test_main.cpp look_tests.cpp test_support.h)
target_link_libraries(test_look PRIVATE bf3_render)
add_test(NAME render.look.unit COMMAND test_look --test-suite=render-look-unit)
add_test(NAME render.look COMMAND test_look --test-suite=render-look)
set_tests_properties(render.look PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(render.look.unit render.look PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_look_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(render.look.unit render.look PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_look_cl_dir}")
endif()

# L1 look (README "L1 look"): godot.l1_look alternates windowed fullscreen --look=m1 / --look=l1 runs of
# res://scenes/m1_metro.tscn (vsync off, 4 free views, 300 timed frames each) for 5 rounds; gate: the median L1 frame
# interval <= 1.25 x the median M1 one and the median L1 p99 frame interval <= 16.7 ms at 2560x1440 (integration
# decision, README "L1 look"; GPU render time reported). Screenshots to $OPENBF3_LOOK_SHOTS when set (never
# commit them). Exits 77 like godot.m1_load; RUN_SERIAL (timing).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME godot.l1_look COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_l1_look.cmake)
    set_tests_properties(godot.l1_look PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1500 RESOURCE_LOCK openbf3_godot_project RUN_SERIAL TRUE)
endif()
# S1 contract (spike S1 server scale; docs/design/s1.md §10, §11): every S1 header compiles together with the M1 ones
# and the plain types keep their properties. No data. The S1 implementation adds its tests to this block: sim.nav.unit,
# sim.nav, sim.nav.bake, host.scale.unit, host.scale (docs §10).
openbf3_test(test_s1_contract test_main.cpp s1_contract_tests.cpp test_support.h)
target_link_libraries(test_s1_contract PRIVATE bf3_host)
add_test(NAME s1.contract COMMAND test_s1_contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_s1_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(s1.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s1_cl_dir}")
endif()
# S1 nav (docs/design/s1.md §5, §10): sim.nav.unit on synthetic worlds (no data; it includes bf3_sim's internal nav
# header and links Detour for the seam). sim.nav: G2 for 7 modes x Metro and Caspian, the input = motor set proofs,
# the Metro split report, cold / warm on a scratch root, install unchanged. sim.nav.bake: G4 (Caspian and Kharg, 16
# threads, uncached; RUN_SERIAL). The install suites exit 77 without an install.
openbf3_test(test_sim_nav test_main.cpp sim_nav_tests.cpp sim_nav_install_tests.cpp test_support.h)
target_link_libraries(test_sim_nav PRIVATE bf3_host Detour)
target_include_directories(test_sim_nav PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_sim/src)
target_compile_definitions(test_sim_nav PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME sim.nav.unit COMMAND test_sim_nav --test-suite=sim-nav-unit)
add_test(NAME sim.nav COMMAND test_sim_nav --test-suite=sim-nav)
add_test(NAME sim.nav.bake COMMAND test_sim_nav --test-suite=sim-nav-bake)
set_tests_properties(sim.nav.unit PROPERTIES TIMEOUT 600)
set_tests_properties(sim.nav PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
set_tests_properties(sim.nav.bake PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    set_tests_properties(sim.nav.unit sim.nav sim.nav.bake PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s1_cl_dir}")
endif()
# S1 bots (docs/design/s1.md §1 G1 / G3, §6-§8, §10): host.scale.unit runs walker bots on the GameServer over synthetic
# worlds (no data); host.scale runs run_scale_bench on Metro and Caspian ConquestLarge0 (G1 p99 <= 8 ms with 64
# walkers over 120 s of sim time, the validity condition, G3's numbers, the observers = 16 row) and exits 77 without an
# install. Both RUN_SERIAL (tick timing). Under asan the timing gates are report only (the test still runs them).
openbf3_test(test_host_scale test_main.cpp host_scale_tests.cpp test_support.h)
target_link_libraries(test_host_scale PRIVATE bf3_host)
target_compile_definitions(test_host_scale PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
if(OPENBF3_ASAN)
    target_compile_definitions(test_host_scale PRIVATE OPENBF3_TIMING_REPORT_ONLY=1)
endif()
add_test(NAME host.scale.unit COMMAND test_host_scale --test-suite=host-scale-unit)
add_test(NAME host.scale COMMAND test_host_scale --test-suite=host-scale)
set_tests_properties(host.scale.unit PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
set_tests_properties(host.scale PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    set_tests_properties(host.scale.unit host.scale PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s1_cl_dir}")
endif()
# S2 vehicle (docs/design/s2.md §11): s2.contract compiles every S2 header together and checks the plain types, the
# restore groups, the ghost widths (equal to the soldier ghost's position quantisers) and the shipped
# vehicle_tuning.cfg keys. No data.
openbf3_test(test_s2_contract test_main.cpp s2_contract_tests.cpp test_support.h)
target_link_libraries(test_s2_contract PRIVATE bf3_sim bf3_net)
target_compile_definitions(test_s2_contract PRIVATE "OPENBF3_VEHICLE_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/vehicle_tuning.cfg\"")
add_test(NAME s2.contract COMMAND test_s2_contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_s2_contract_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(s2.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s2_contract_cl_dir}")
endif()
# S2 vehicle (docs/design/s2.md §1, §11): sim.vehicle.unit drives the Humvee fixture on the synthetic ground (the unit
# contract U, gates D / G1 / G1r / G2x on the 7 synthetic courses with G2 reported, G1m, two threads, report H); sim.vehicle runs MP_007
# RushLarge0 (gates C, D, G1, G1r, G2x, I; reports G2, S and H) and exits 77 = skipped without an install. The test includes
# bf3_sim's internal headers (VehicleWorld::Impl, CollisionWorld::Impl) and links Jolt for the seams. Under asan the
# install run keeps the primary restore mode and ExactPod (G2x) only (OPENBF3_S2_ASAN).
openbf3_test(test_sim_vehicle test_main.cpp sim_vehicle_tests.cpp test_support.h)
target_link_libraries(test_sim_vehicle PRIVATE bf3_sim Jolt)
target_include_directories(test_sim_vehicle PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_sim/src)
target_compile_definitions(test_sim_vehicle PRIVATE "OPENBF3_VEHICLE_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/vehicle_tuning.cfg\"")
add_test(NAME sim.vehicle.unit COMMAND test_sim_vehicle --test-suite=sim-vehicle-unit)
add_test(NAME sim.vehicle COMMAND test_sim_vehicle --test-suite=sim-vehicle)
# TIMEOUTs are 3x the measured runs (docs/design/s2.md §15.1): dev 46 s / 55 s, asan 636 s / 440 s.
set_tests_properties(sim.vehicle PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 150)
set_tests_properties(sim.vehicle.unit PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
if(OPENBF3_ASAN)
    target_compile_definitions(test_sim_vehicle PRIVATE OPENBF3_S2_ASAN=1)
    get_filename_component(_openbf3_vehicle_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(sim.vehicle.unit sim.vehicle PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_vehicle_cl_dir}")
    set_tests_properties(sim.vehicle PROPERTIES TIMEOUT 1950)
    set_tests_properties(sim.vehicle.unit PROPERTIES TIMEOUT 1350)
endif()
