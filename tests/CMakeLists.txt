# doctest executables for the Godot-free libraries. Run with: ctest --preset dev
# Every row follows docs/testing.md (scheduling labels, parallel-safe scratch, PROCESSORS / COST, budgets); build.cmd
# reports violations after each ctest phase (tools/test_budget.cmake).
function(openbf3_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    # Exceptions are off project-wide; a failing REQUIRE then aborts the executable.
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
    openbf3_module(${name})
endfunction()

# docs/testing.md "Splits": one ctest row for a subset of a doctest suite's cases, run by tests/run_case_row.cmake.
#   openbf3_case_row(NAME <row> EXE <target> SUITE <suite> GROUP <split group> COUNT <n> [CASES <pattern>...]
#                    [EXCLUDE <pattern>...] [INSTALL_CASE <pattern>])
# Patterns are doctest wildcards without spaces or commas. INSTALL_CASE (the suite's install-unchanged case) is added
# to CASES; COUNT counts every case the row runs, install case included, and the runner fails unless exactly that
# many ran and passed (a mistyped pattern cannot pass on the install check alone). Exit 77 passes through.
function(openbf3_case_row)
    cmake_parse_arguments(PARSE_ARGV 0 R "" "NAME;EXE;SUITE;GROUP;COUNT;INSTALL_CASE" "CASES;EXCLUDE")
    set(cases ${R_CASES})
    if(R_INSTALL_CASE)
        list(APPEND cases ${R_INSTALL_CASE})
    endif()
    list(JOIN cases "," case_arg)
    list(JOIN R_EXCLUDE "," exclude_arg)
    add_test(NAME ${R_NAME} COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:${R_EXE}>" -DSUITE=${R_SUITE} -DCOUNT=${R_COUNT}
        "-DCASES=${case_arg}" "-DEXCLUDE=${exclude_arg}" -P ${CMAKE_CURRENT_SOURCE_DIR}/run_case_row.cmake)
    set_tests_properties(${R_NAME} PROPERTIES SKIP_RETURN_CODE 77)
    set_property(GLOBAL APPEND PROPERTY OPENBF3_SPLIT_${R_GROUP}_ROWS ${R_NAME})
    set_property(GLOBAL PROPERTY OPENBF3_SPLIT_${R_NAME}_CASES "${case_arg}")
    set_property(GLOBAL PROPERTY OPENBF3_SPLIT_${R_NAME}_EXCLUDE "${exclude_arg}")
endfunction()
# The <NAME> row (a split group's .split_cover) diffs `--list-test-cases` of the whole suite against the group's rows:
# every case in exactly one row, the install case (when given) in all of them. No data, under a second.
function(openbf3_split_cover)
    cmake_parse_arguments(PARSE_ARGV 0 C "" "NAME;EXE;SUITE;GROUP;INSTALL_CASE" "")
    get_property(rows GLOBAL PROPERTY OPENBF3_SPLIT_${C_GROUP}_ROWS)
    set(body "set(ROWS ${rows})\n")
    foreach(row IN LISTS rows)
        get_property(cases GLOBAL PROPERTY OPENBF3_SPLIT_${row}_CASES)
        get_property(exclude GLOBAL PROPERTY OPENBF3_SPLIT_${row}_EXCLUDE)
        string(APPEND body "set(ROW_${row}_CASES \"${cases}\")\nset(ROW_${row}_EXCLUDE \"${exclude}\")\n")
    endforeach()
    set(file "${CMAKE_CURRENT_BINARY_DIR}/split_cover/${C_GROUP}.cmake")
    file(WRITE "${file}" "${body}")
    add_test(NAME ${C_NAME} COMMAND ${CMAKE_COMMAND} -DMODE=cover "-DEXE=$<TARGET_FILE:${C_EXE}>" -DSUITE=${C_SUITE}
        "-DROWS_FILE=${file}" "-DINSTALL_CASE=${C_INSTALL_CASE}" -P ${CMAKE_CURRENT_SOURCE_DIR}/run_case_row.cmake)
    set_tests_properties(${C_NAME} PROPERTIES TIMEOUT 60)
endfunction()

# docs/testing.md "Godot rows": every godot.* row is registered here.
#   openbf3_godot_row(<row> <script> HEADLESS|WINDOW [TIMEOUT <s>] [ARGS <-D...>...])
# The script includes tests/godot_common.cmake: GODOT_EXE (the configure-time OPENBF3_GODOT_EXE) unless OPENBF3_GODOT
# is set, a per-build cache root and log file for headless rows (BUILD_DIR, ROW); a WINDOW script sets GODOT_WINDOWED
# and runs only with OPENBF3_GODOT_WINDOWED=1. WINDOW rows get the label window (the merge pass's user-away phase);
# a HEADLESS row whose script does not run --headless, or a WINDOW row whose script is not opted in, is a configure
# error, and so is a godot.* row registered any other way (the check at the end of this file). Every row requires
# the godot.project_setup fixture and shares the project's .godot directory and DLL (RESOURCE_LOCK).
function(openbf3_godot_row name script mode)
    cmake_parse_arguments(PARSE_ARGV 3 G "NO_PROJECT_LOCK" "TIMEOUT" "ARGS")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${script}" body)
    if(mode STREQUAL "WINDOW")
        if(NOT body MATCHES "set\\(GODOT_WINDOWED TRUE\\)")
            message(FATAL_ERROR "${name}: the WINDOW script ${script} must set(GODOT_WINDOWED TRUE) before godot_common.cmake")
        endif()
    elseif(mode STREQUAL "HEADLESS")
        if(body MATCHES "set\\(GODOT_WINDOWED TRUE\\)" OR NOT body MATCHES "--headless")
            message(FATAL_ERROR "${name}: the HEADLESS script ${script} does not run Godot --headless; register it as WINDOW")
        endif()
    else()
        message(FATAL_ERROR "openbf3_godot_row(${name}): mode must be HEADLESS or WINDOW, not '${mode}'")
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT} "-DGODOT_EXE=${OPENBF3_GODOT_EXE}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}" -DROW=${name} ${G_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/${script})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED godot_project)
    # NO_PROJECT_LOCK: a headless row shown not to write the project's .godot directory (docs/testing.md "Splits").
    if(NOT G_NO_PROJECT_LOCK)
        set_tests_properties(${name} PROPERTIES RESOURCE_LOCK openbf3_godot_project)
    endif()
    if(G_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${G_TIMEOUT})
    endif()
    if(mode STREQUAL "WINDOW")
        set_property(TEST ${name} APPEND PROPERTY LABELS window)
    endif()
    set_property(GLOBAL APPEND PROPERTY OPENBF3_GODOT_ROWS ${name})
endfunction()
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    # The fixture: writes .godot/extension_list.cfg when missing and checks the DLL; never runs --import.
    add_test(NAME godot.project_setup COMMAND ${CMAKE_COMMAND} -DPROJECT_DIR=${OPENBF3_GODOT_PROJECT}
        "-DDLL=$<TARGET_FILE:bf3_godot>" -P ${CMAKE_CURRENT_SOURCE_DIR}/godot_project_setup.cmake)
    set_tests_properties(godot.project_setup PROPERTIES FIXTURES_SETUP godot_project RESOURCE_LOCK openbf3_godot_project TIMEOUT 60)
endif()

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
# Integration (integ/m2a1): host.server sleeps 2 s of wall clock and counts the 60 Hz ticks; under parallel load the
# sleep overshoots (135 ticks against at most 130 once), so it never runs next to the branch pass: label realtime
# (the scheduling block at the end), the merge pass's lane of light real-time rows. The thresholds are unchanged.
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
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m9_reload godot_m9_reload.cmake HEADLESS TIMEOUT 300)
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
# cmake_language(EXIT) needs CMake 3.29. godot.s0_selftest gates the server tick rate and step p99 (label realtime).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.s0_selftest godot_check.cmake HEADLESS TIMEOUT 300
        ARGS -DUSER_ARGS=--s0-selftest "-DPASS_LINE=S0 SELFTEST: PASS")
    openbf3_godot_row(godot.inspector godot_check.cmake HEADLESS TIMEOUT 300
        ARGS -DUSER_ARGS=--m0-inspector-check "-DPASS_LINE=M0 INSPECTOR CHECK: PASS")
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
    # The hostile work-budget cases CHECK a 20 s wall clock; at -j16 under asan one read 25.1 s (2026-10-17).
    set_tests_properties(core.gameplay.tables.unit PROPERTIES RUN_SERIAL TRUE)
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
    openbf3_godot_row(godot.collision godot_collision.cmake HEADLESS TIMEOUT 360)
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
# It runs in the parallel branch pass and, labelled merge_also, again alone in the merge pass's serial timing phase,
# which decides the peak gate (docs/testing.md; a -j neighbour can only trim the working set, so the peak commit is
# printed next to it). cmake_language(EXIT) needs CMake 3.29.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME tools.m1_prepare COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DE6_REPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/m1_prepare.cmake)
    set_tests_properties(tools.m1_prepare PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
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
# exits 77 without an install. Both read the shipped tuning file. The suite host-session-unit is split
# (docs/testing.md "Splits"): host.session.unit.pure runs the 5 socket-free cases (judge_rest, presentation, M2a cl's
# R1 presentation equality, GameServer without sockets, SpawnSweep in batches) on every branch; host.session.unit runs the 5 loopback cases,
# which wait on real time, serially in the merge pass's timing phase (label timing; it needed lane reruns);
# host.session.unit.split_cover proves every case is in exactly one of the two. The suite host-session is split the
# same way into three lane rows that run side by side, each loading Metro itself and checking install unchanged:
# host.session.gate (the load case and E2 under 150/2), host.session.stress (E2 under lat=150-165,loss=2) and
# host.session.e3 (the SpawnSweep); host.session.split_cover proves the cover.
openbf3_test(test_host_session test_main.cpp host_session_tests.cpp test_support.h)
target_link_libraries(test_host_session PRIVATE bf3_host)
target_compile_definitions(test_host_session PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
# *present_others* selects two socket-free cases: M1's presentation helpers and M2a cl's R1 equality with M1's body.
set(_openbf3_session_pure *judge_rest*synthetic* *present_others* *GameServer*without*sockets* *GameServer*SpawnSweep*batches*)
openbf3_case_row(NAME host.session.unit.pure EXE test_host_session SUITE host-session-unit GROUP host-session-unit COUNT 5
    CASES ${_openbf3_session_pure})
openbf3_case_row(NAME host.session.unit EXE test_host_session SUITE host-session-unit GROUP host-session-unit COUNT 5
    EXCLUDE ${_openbf3_session_pure})
openbf3_split_cover(NAME host.session.unit.split_cover EXE test_host_session SUITE host-session-unit GROUP host-session-unit)
set(_openbf3_session_install "*Metro*install*unchanged*")
openbf3_case_row(NAME host.session.gate EXE test_host_session SUITE host-session GROUP host-session COUNT 3
    CASES "*Metro*load_shared_level*cold*warm*" "*Metro*E2*150/2*gate" INSTALL_CASE ${_openbf3_session_install})
openbf3_case_row(NAME host.session.stress EXE test_host_session SUITE host-session GROUP host-session COUNT 2
    CASES "*Metro*E2*lat=150-165*" INSTALL_CASE ${_openbf3_session_install})
openbf3_case_row(NAME host.session.e3 EXE test_host_session SUITE host-session GROUP host-session COUNT 2
    CASES "*Metro*E3*SpawnSweep*" INSTALL_CASE ${_openbf3_session_install})
openbf3_split_cover(NAME host.session.split_cover EXE test_host_session SUITE host-session GROUP host-session
    INSTALL_CASE ${_openbf3_session_install})
set_tests_properties(host.session.unit.pure host.session.unit PROPERTIES TIMEOUT 300)
set_tests_properties(host.session.unit PROPERTIES RUN_SERIAL TRUE)
set_tests_properties(host.session.gate host.session.stress host.session.e3 PROPERTIES TIMEOUT 400)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_t4_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(host.session.unit.pure host.session.unit host.session.unit.split_cover host.session.gate
        host.session.stress host.session.e3 host.session.split_cover PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_t4_cl_dir}")
endif()

# M1 T5: the level scene (bf3_render level_scene.h; docs/design/m1.md §14). render.scene.unit: synthetic census,
# meshes and heightfields (no data). render.scene: Metro TDM plan and SceneLanes cold and warm on a scratch cache
# root with its timings, texture count and bytes, peak in-flight bytes and peak working set, install.unchanged (exits
# 77 = skipped without an install). The lane timings are printed, never gated, so it runs in the parallel branch pass.
openbf3_test(test_render_scene test_main.cpp render_scene_tests.cpp test_support.h)
target_link_libraries(test_render_scene PRIVATE bf3_render psapi)
target_include_directories(test_render_scene PRIVATE ${PROJECT_SOURCE_DIR}/src/bf3_render/src)
add_test(NAME render.scene.unit COMMAND test_render_scene --test-suite=render-scene-unit)
add_test(NAME render.scene COMMAND test_render_scene --test-suite=render-scene)
set_tests_properties(render.scene PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(render.scene.unit render.scene PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_scene_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(render.scene.unit render.scene PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_scene_cl_dir}")
endif()

# M1 T6 godot (docs/design/m1.md §1, §11, §12, §14): the headless --m1-check of res://scenes/m1_metro.tscn runs as
# three shards side by side in the merge pass's real-time lane (--m1-phase, godot_m1.cmake -DPHASE): godot.m1.e1 (E1
# through the loopback server on Trace reports, E1d, E3 SpawnSweep with 128 slots, the snapshot equality,
# upload.surface_format, input.keys), godot.m1.e2_gate (E2 under 150/2 judged by judge_rest) and godot.m1.e2_stress
# (E2 under lat=150-165,loss=2, reported); each loads Metro on its own cache root and checks install unchanged. They
# drop the project lock: a headless m1 run leaves bf3-opensource/.godot unchanged (hashed before and after, S16), and
# godot.project_setup has written extension_list.cfg first. godot.m1_load the windowed --m1-load-check cold then
# warm on a fresh scratch cache root (E4 / E5, phase times, frame time, the game's peak working set, 8 spawn shots)
# after a probe run, and exits 77 = skipped without a window or rendering device. Both exit 77 without OPENBF3_GODOT
# or an install and share the project and its DLL with the other godot.* tests. The godot.m1 shards are labelled
# realtime (E2 wall-clock netsim; the merge pass's lane), godot.m1_load is WINDOW and RUN_SERIAL (E4 / E5 timing).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m1.e1 godot_m1.cmake HEADLESS NO_PROJECT_LOCK TIMEOUT 450 ARGS -DPHASE=e1)
    openbf3_godot_row(godot.m1.e2_gate godot_m1.cmake HEADLESS NO_PROJECT_LOCK TIMEOUT 300 ARGS -DPHASE=e2_gate)
    openbf3_godot_row(godot.m1.e2_stress godot_m1.cmake HEADLESS NO_PROJECT_LOCK TIMEOUT 300 ARGS -DPHASE=e2_stress)
    openbf3_godot_row(godot.m1_load godot_m1_load.cmake WINDOW TIMEOUT 300)
    set_tests_properties(godot.m1_load PROPERTIES RUN_SERIAL TRUE)
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
# under asan. It runs in the parallel branch pass and, labelled merge_also, again alone in the merge pass's serial
# timing phase, which decides the gate (a -j neighbour can only trim the working set; the peak commit is printed too).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME tools.ebx_memory COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DREPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ebx_memory.cmake)
    set_tests_properties(tools.ebx_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
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
    openbf3_godot_row(godot.s3_crowd godot_s3_crowd.cmake WINDOW TIMEOUT 2000)
    set_tests_properties(godot.s3_crowd PROPERTIES RUN_SERIAL TRUE)
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
    openbf3_godot_row(godot.l1_look godot_l1_look.cmake WINDOW TIMEOUT 1500)
    set_tests_properties(godot.l1_look PROPERTIES RUN_SERIAL TRUE)
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
    # G4 is report only under asan, like host.scale's G1 (the first asan run of sim.nav.bake, 2026-10-17: Caspian
    # 19.5 s and Kharg 30.0 s on 16 threads against the 6 / 13 s dev gates).
    target_compile_definitions(test_sim_nav PRIVATE OPENBF3_TIMING_REPORT_ONLY=1)
    set_tests_properties(sim.nav.unit sim.nav sim.nav.bake PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s1_cl_dir}")
endif()
# S1 bots (docs/design/s1.md §1 G1 / G3, §6-§8, §10): host.scale.unit runs walker bots on the GameServer over synthetic
# worlds (no data); host.scale runs run_scale_bench on Metro and Caspian ConquestLarge0 (G1 p99 <= 8 ms with 64
# walkers over 120 s of sim time, the validity condition, G3's numbers, the observers = 16 row) and exits 77 without an
# install. Both RUN_SERIAL (tick timing). Under asan the timing gates are report only (the test still runs them).
# host.scale.paced runs the report-only paced 60 Hz windows (they followed the G1 windows in host.scale) in the merge
# pass's real-time lane (label realtime): after a 2-thread load it ticks under a core.
openbf3_test(test_host_scale test_main.cpp host_scale_tests.cpp test_support.h)
target_link_libraries(test_host_scale PRIVATE bf3_host)
target_compile_definitions(test_host_scale PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
if(OPENBF3_ASAN)
    target_compile_definitions(test_host_scale PRIVATE OPENBF3_TIMING_REPORT_ONLY=1)
endif()
add_test(NAME host.scale.unit COMMAND test_host_scale --test-suite=host-scale-unit)
add_test(NAME host.scale COMMAND test_host_scale --test-suite=host-scale)
add_test(NAME host.scale.paced COMMAND test_host_scale --test-suite=host-scale-paced)
set_tests_properties(host.scale.unit PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
set_tests_properties(host.scale PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 RUN_SERIAL TRUE)
set_tests_properties(host.scale.paced PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(host.scale.unit host.scale host.scale.paced PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_s1_cl_dir}")
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
# TIMEOUTs are 3x the measured runs (docs/design/s2.md §15.1): dev 46 s / 55 s, asan 636 s / 440 s; sim.vehicle 240 s
# since it runs in the parallel branch pass (59 s measured under -j8).
set_tests_properties(sim.vehicle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
set_tests_properties(sim.vehicle.unit PROPERTIES TIMEOUT 180)
if(OPENBF3_ASAN)
    target_compile_definitions(test_sim_vehicle PRIVATE OPENBF3_S2_ASAN=1)
    get_filename_component(_openbf3_vehicle_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(sim.vehicle.unit sim.vehicle PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_vehicle_cl_dir}")
    set_tests_properties(sim.vehicle PROPERTIES TIMEOUT 1950)
    set_tests_properties(sim.vehicle.unit PROPERTIES TIMEOUT 1350)
endif()
# TC terrain collision (track "Terrain collision everywhere", docs/design/terrain-collision.md §13): tc.contract
# compiles the TC header additions together and checks their constants and defaults. No data.
openbf3_test(test_tc_contract test_main.cpp tc_contract_tests.cpp test_support.h)
target_link_libraries(test_tc_contract PRIVATE bf3_sim bf3_render)
add_test(NAME tc.contract COMMAND test_tc_contract)
# sim.terrain.unit (no data); the install suite sim-terrain in two rows (openbf3_case_row; exit 77 without an install):
# sim.terrain = G1 over every level x mode (the dev pass's critical path: its runs and checks on lanes, PROCESSORS 8),
# sim.terrain.worlds = the full cached worlds and G6 Caspian RushLarge0 spawns; both run the install-unchanged case and
# sim.terrain.split_cover proves every case is in exactly one of them. tools.tc_cost (G3, tests/tc_cost.cmake): bf3dump shapes
# cold / warm / warm in child processes on fresh scratch roots, the limits of docs §4.5 (report-only under asan;
# label timing, RUN_SERIAL).
openbf3_test(test_sim_terrain test_main.cpp terrain_collision_tests.cpp terrain_collision_support.h test_support.h)
target_link_libraries(test_sim_terrain PRIVATE bf3_sim Jolt)
target_include_directories(test_sim_terrain PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_sim/src)
add_test(NAME sim.terrain.unit COMMAND test_sim_terrain --test-suite=sim-terrain-unit)
openbf3_case_row(NAME sim.terrain EXE test_sim_terrain SUITE sim-terrain GROUP sim-terrain COUNT 2 CASES "*G1*every*"
    INSTALL_CASE "*install is unchanged*")
openbf3_case_row(NAME sim.terrain.worlds EXE test_sim_terrain SUITE sim-terrain GROUP sim-terrain COUNT 3
    CASES "*G1*full*worlds*" "*G6*Caspian*" INSTALL_CASE "*install is unchanged*")
openbf3_split_cover(NAME sim.terrain.split_cover EXE test_sim_terrain SUITE sim-terrain GROUP sim-terrain
    INSTALL_CASE "*install is unchanged*")
set_tests_properties(sim.terrain sim.terrain.worlds PROPERTIES TIMEOUT 1800)
set_tests_properties(sim.terrain.unit PROPERTIES TIMEOUT 600)
if(TARGET bf3dump)
    add_test(NAME tools.tc_cost COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DREPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tc_cost.cmake)
    set_tests_properties(tools.tc_cost PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 RUN_SERIAL TRUE)
endif()
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_tc_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(tc.contract sim.terrain.unit sim.terrain sim.terrain.worlds PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_tc_cl_dir}")
    set_tests_properties(sim.terrain PROPERTIES TIMEOUT 10800) # G1 over 230 runs: 25 of 29 levels in 5,400 s under asan (serial)
    if(TARGET bf3dump)
        set_tests_properties(tools.tc_cost PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_tc_cl_dir}")
    endif()
endif()

# UI ui1 (docs/design/ui-swf-fonts.md §12, §14): ui1.contract, every ui1 header compiles together and the inline
# helpers behave (no data, no Godot). The implementation adds core.ui.swf(.unit/.fuzz), core.ui.font(.unit) and the
# Godot checks below this line.
openbf3_test(test_ui1_contract test_main.cpp ui1_contract_tests.cpp test_support.h)
target_link_libraries(test_ui1_contract PRIVATE bf3_core)
add_test(NAME ui1.contract COMMAND test_ui1_contract --test-suite=ui1-contract)
set_tests_properties(ui1.contract PROPERTIES TIMEOUT 120)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_ui1_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(ui1.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_ui1_cl_dir}")
endif()
# UI ui1 implementation (docs/design/ui-swf-fonts.md §14): test_ui1 runs the SWF and font suites. core.ui.swf.unit
# and core.ui.font.unit are synthetic (tests/ui_swf_writer.h); core.ui.swf, core.ui.swf.fuzz and core.ui.font read
# the install (read-only) and exit 77 = skipped without one. godot.ui_fonts runs res://scenes/ui_fonts.tscn
# -- --ui-check headless; godot.ui_fonts_shots runs it windowed with --shot-dir (OPENBF3_UI1_SHOTS) and exits 77
# without a window.
openbf3_test(test_ui1 test_main.cpp ui_swf_tests.cpp ui_font_tests.cpp ui_swf_writer.h test_support.h)
target_link_libraries(test_ui1 PRIVATE bf3_core)
add_test(NAME core.ui.swf.unit COMMAND test_ui1 --test-suite=ui-swf-unit)
add_test(NAME core.ui.font.unit COMMAND test_ui1 --test-suite=ui-font-unit)
add_test(NAME core.ui.swf COMMAND test_ui1 --test-suite=ui-swf)
add_test(NAME core.ui.swf.fuzz COMMAND test_ui1 --test-suite=ui-swf-fuzz)
add_test(NAME core.ui.font COMMAND test_ui1 --test-suite=ui-font)
set_tests_properties(core.ui.swf core.ui.swf.fuzz core.ui.font PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.ui.swf.unit core.ui.font.unit core.ui.swf core.ui.swf.fuzz core.ui.font PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_ui1_impl_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.ui.swf.unit core.ui.font.unit core.ui.swf core.ui.swf.fuzz core.ui.font
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_ui1_impl_cl_dir}")
    set_tests_properties(core.ui.swf.unit core.ui.swf.fuzz PROPERTIES TIMEOUT 1800)
endif()
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.ui_fonts godot_ui_fonts.cmake HEADLESS TIMEOUT 360)
    openbf3_godot_row(godot.ui_fonts_shots godot_ui_fonts_shots.cmake WINDOW TIMEOUT 360)
    set_tests_properties(godot.ui_fonts_shots PROPERTIES RUN_SERIAL TRUE)
endif()

# UI ui2 (docs/design/ui-minimap-icons.md §13): ui2.contract compiles the minimap / HUD icon / HUD model headers
# together and checks the binding-row counts and the inline mapping, view and icon helpers; no data, no Godot.
openbf3_test(test_ui2_contract test_main.cpp ui2_contract_tests.cpp)
target_link_libraries(test_ui2_contract PRIVATE bf3_core)
add_test(NAME ui2.contract COMMAND test_ui2_contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_ui2_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(ui2.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_ui2_cl_dir}")
endif()
# UI ui2 (docs/design/ui-minimap-icons.md §13): the minimap, HUD icon and HUD source readers. ui.minimap.unit and
# ui.hud.unit are synthetic (no data). ui.minimap (G1-G5), ui.hud.icons (G6, G7) and ui.hud.sources (G8) read the
# install; ui.minimap.fuzz and ui.hud.fuzz mutate its partitions and texture headers in memory (meant for asan). The
# install ones exit 77 = skipped without an install and end with install.unchanged (G13).
openbf3_test(test_ui2 test_main.cpp ui_minimap_tests.cpp ui_hud_tests.cpp ui_install_tests.cpp ui_fuzz_tests.cpp ui2_test_support.h test_support.h)
target_link_libraries(test_ui2 PRIVATE bf3_core)
add_test(NAME ui.minimap.unit COMMAND test_ui2 --test-suite=ui-minimap-unit)
add_test(NAME ui.hud.unit COMMAND test_ui2 --test-suite=ui-hud-unit)
add_test(NAME ui.minimap COMMAND test_ui2 --test-suite=ui-minimap)
add_test(NAME ui.hud.icons COMMAND test_ui2 --test-suite=ui-hud-icons)
add_test(NAME ui.hud.sources COMMAND test_ui2 --test-suite=ui-hud-sources)
add_test(NAME ui.minimap.fuzz COMMAND test_ui2 --test-suite=ui-minimap-fuzz)
add_test(NAME ui.hud.fuzz COMMAND test_ui2 --test-suite=ui-hud-fuzz)
set_tests_properties(ui.minimap ui.hud.icons ui.hud.sources ui.minimap.fuzz ui.hud.fuzz PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(ui.minimap.unit ui.hud.unit ui.minimap ui.hud.icons ui.hud.sources ui.minimap.fuzz ui.hud.fuzz PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_ui2_impl_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(ui.minimap.unit ui.hud.unit ui.minimap ui.hud.icons ui.hud.sources ui.minimap.fuzz ui.hud.fuzz
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_ui2_impl_cl_dir}" TIMEOUT 1800)
endif()
# UI ui2 godot (docs/design/ui-minimap-icons.md §1 G9-G11, G14, §10.4): godot.ui2 runs the windowed
# --minimap-check --minimap-mode=ConquestSmall0 of res://scenes/m1_metro.tscn after a probe run (exit 77 = skipped
# without OPENBF3_GODOT, an install, a window or a rendering device); OPENBF3_UI2_SHOTS names a directory for the
# six G11 PNGs. It shares the project and its DLL with the other godot.* tests and reads the viewport image (RUN_SERIAL).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.ui2 godot_ui2.cmake WINDOW TIMEOUT 400)
    set_tests_properties(godot.ui2 PROPERTIES RUN_SERIAL TRUE)
endif()

# M2a weapons (docs/design/m2a-weapons.md §12): m2a.weapons.contract compiles every weapons header with the M1
# headers it builds on and checks the inline contract (sub-tick time, the pinned shot seeds, the weapon button
# levels, the mechanics defaults). No data. The implementation wave adds sim.weapons.unit, sim.weapons,
# sim.weapons.range, sim.weapons.fuzz, sim.weapons.perf and tools.weapon_sim in this block.
openbf3_test(test_weapons_contract test_main.cpp weapons_contract_tests.cpp test_support.h)
target_link_libraries(test_weapons_contract PRIVATE bf3_sim)
add_test(NAME m2a.weapons.contract COMMAND test_weapons_contract --test-suite=weapons-contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_wp_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(m2a.weapons.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_wp_cl_dir}")
endif()
# M2a weapons: sim.weapons.unit runs the oracle rows (weapon_checks.h) on synthetic defs carrying the 7.1 numbers plus
# the unit rows (fire modes, spread / recoil, reloads, determinism, hostile bounds, capsules, damage, explosives). No data.
openbf3_test(test_weapons test_main.cpp weapons_tests.cpp weapons_support.h test_support.h)
target_link_libraries(test_weapons PRIVATE bf3_sim)
add_test(NAME sim.weapons.unit COMMAND test_weapons --test-suite=weapons-unit)
set_tests_properties(sim.weapons.unit PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    set_tests_properties(sim.weapons.unit PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_wp_cl_dir}")
endif()
# M2a weapons on the install (read-only; exit 77 = skipped without one; every case prints install.unchanged):
# sim.weapons (grid pairs, materials, the oracle rows on MP_Subway defs, W4's table), sim.weapons.range (Metro's
# static world: W3 end to end, W5, W7, W9 on stand-ins), sim.weapons.fuzz (W8: mutated retail and synthetic blobs;
# meant for asan too) and sim.weapons.perf (W11, wall clock, noisy: RUN_SERIAL, exits 77 when the file named by
# OPENBF3_QUIET_LOCK exists).
openbf3_test(test_weapons_install test_main.cpp weapons_install_tests.cpp weapons_range_tests.cpp weapons_install_support.h weapons_support.h test_support.h)
target_link_libraries(test_weapons_install PRIVATE bf3_sim)
add_test(NAME sim.weapons COMMAND test_weapons_install --test-suite=weapons)
add_test(NAME sim.weapons.range COMMAND test_weapons_install --test-suite=weapons-range)
add_test(NAME sim.weapons.fuzz COMMAND test_weapons_install --test-suite=weapons-fuzz)
add_test(NAME sim.weapons.perf COMMAND test_weapons_install --test-suite=weapons-perf)
set_tests_properties(sim.weapons sim.weapons.range sim.weapons.fuzz sim.weapons.perf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
set_tests_properties(sim.weapons.perf PROPERTIES RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    set_tests_properties(sim.weapons sim.weapons.range sim.weapons.fuzz sim.weapons.perf
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_wp_cl_dir}")
endif()
# M2a weapons: `bf3dump weapon-sim --check` prints the data-backed W1-W6 / W9 rows and exits 0 / 1 (77 without an install).
add_test(NAME tools.weapon_sim COMMAND bf3dump weapon-sim --check)
set_tests_properties(tools.weapon_sim PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    set_tests_properties(tools.weapon_sim PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_wp_cl_dir}")
endif()

# M2a VFX1 (docs/design/m2a-vfx1.md §11): fx.contract pins the bf3_fx interface (record sizes, the 32 processor and 10
# evaluator vocabularies, name tables, the polynomial / colour / random-number goldens). No data, no Godot.
openbf3_test(test_fx_contract test_main.cpp fx_contract_tests.cpp test_support.h)
target_link_libraries(test_fx_contract PRIVATE bf3_fx)
add_test(NAME fx.contract COMMAND test_fx_contract --test-suite=fx-contract)
set_tests_properties(fx.contract PROPERTIES TIMEOUT 120)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_fx_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(fx.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_fx_cl_dir}")
endif()
# M2a VFX1 (docs/design/m2a-vfx1.md §11): fx.unit (compiler on synthetic EBX, blob, cache rules, simulator; G2, G8),
# fx.fuzz (G10; also in the asan preset), fx.install (G1, G3-G7, cold vs warm, G11; 77 without an install) and
# tools.fx_bench (G9: `bf3dump fx bench --seed 1`, median ns per admitted particle <= 50; RUN_SERIAL; 77 without an
# install; report only under asan). Timings are wall clock on a shared machine: noisy.
openbf3_test(test_fx test_main.cpp fx_unit_tests.cpp fx_fuzz_tests.cpp fx_install_tests.cpp fx_test_support.h test_support.h)
target_link_libraries(test_fx PRIVATE bf3_fx)
add_test(NAME fx.unit COMMAND test_fx --test-suite=fx-unit)
add_test(NAME fx.fuzz COMMAND test_fx --test-suite=fx-fuzz)
add_test(NAME fx.install COMMAND test_fx --test-suite=fx-install)
set_tests_properties(fx.unit fx.fuzz PROPERTIES TIMEOUT 600)
set_tests_properties(fx.install PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    add_test(NAME tools.fx_bench COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DREPORT_ONLY=${OPENBF3_ASAN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/fx_bench.cmake)
    set_tests_properties(tools.fx_bench PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
endif()
if(OPENBF3_ASAN)
    set_tests_properties(fx.unit fx.fuzz fx.install PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_fx_cl_dir}")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        set_tests_properties(tools.fx_bench PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_fx_cl_dir}")
    endif()
endif()

# M2a w2 (W2.0 contract, docs/design/m2a.md §15, §15.1): m2a.contract compiles every wave-2 interface header together
# (and tests/gunplay_fixture.h) and pins the sizes of Command, ShotView, PresentEvent and OtherPose,
# weapon_buttons::kAll inside the wire mask, the exact_aim hash change and its field-for-field equality with the range
# post-process, the three step_weapons_m2a rows and the two ack-bit conventions. Rows that need a track's body (nt's
# codecs, sv's R3 and weapon_rules.cpp) print PENDING with the reason until that track turns them on. No install;
# one short loopback session on a synthetic floor (the fixture's smoke case).
openbf3_test(test_m2a_contract test_main.cpp m2a_contract_tests.cpp gunplay_fixture.h weapons_support.h test_support.h)
target_link_libraries(test_m2a_contract PRIVATE bf3_host bf3_render bf3_anim bf3_fx)
# The fixture's metro() reads the shipped motor tuning (compiled here, not run: m2a.contract needs no install).
target_compile_definitions(test_m2a_contract PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME m2a.contract COMMAND test_m2a_contract --test-suite=m2a-contract)
set_tests_properties(m2a.contract PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_m2a_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(m2a.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m2a_cl_dir}")
endif()

# M2a nt (docs/design/m2a-nt.md): net.proto.fuzz feeds seeded mutations and random bytes of every protocol v2 message
# type through its decoder (refused and counted per type, or decode -> encode -> decode stable) and checks the
# structured malformed cases (every truncation, a trailing byte, another type's decoder) are all refused. No data;
# meant for the asan preset too (E17 wire side).
openbf3_test(test_net_proto_fuzz test_main.cpp net_proto_fuzz.cpp)
target_link_libraries(test_net_proto_fuzz PRIVATE bf3_net)
add_test(NAME net.proto.fuzz COMMAND test_net_proto_fuzz --test-suite=net-proto-fuzz)
set_tests_properties(net.proto.fuzz PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_nt_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(net.proto.fuzz PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_nt_cl_dir}")
endif()

# M2a lc (docs/design/m2a.md §6, E12 (a), E18 (a), R4; docs/design/m2a-lc.md): sim.lagcomp.unit (no data: the sampler,
# the time rule, the track rules, LagHistory's acks and shot views, the E12 sample-equality rows with two clients, the
# provider's cache on vs off and broad phase, the capsule table's blob and hostile blobs, posed_target, R4's query
# segments) and anim.server_pose (the install: the capsule pose table, E18 (a) against the reference evaluation and
# CrowdEvaluator, install.unchanged; 77 without one). Both also run under the asan preset.
openbf3_test(test_lagcomp test_main.cpp sim_lagcomp_tests.cpp weapons_support.h test_support.h)
target_link_libraries(test_lagcomp PRIVATE bf3_host)
add_test(NAME sim.lagcomp.unit COMMAND test_lagcomp --test-suite=sim-lagcomp-unit)
openbf3_test(test_server_pose test_main.cpp anim_server_pose_tests.cpp test_support.h)
target_link_libraries(test_server_pose PRIVATE bf3_anim bf3_sim)
add_test(NAME anim.server_pose COMMAND test_server_pose --test-suite=anim-server-pose)
set_tests_properties(anim.server_pose PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(sim.lagcomp.unit anim.server_pose PROPERTIES TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_lc_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(sim.lagcomp.unit anim.server_pose PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_lc_cl_dir}")
endif()

# M2a cl (docs/design/m2a.md §6.1-§6.3, §7.2-§7.4, §8, §13.3; docs/design/m2a-cl.md): host.gunplay.client.unit (no
# data, short: the events ring, present_weapon, the weapon lane under catch_up, ClientProjectiles, the view sample, E6's
# refusal, R2's health rebase, the SnapshotOthers merge, the hostile datagrams) and host.gunplay.client (wall clock,
# minutes: E1 + E5, E12 lagcomp (b) + control + stress, E15 loopback and two clients; runs only with
# OPENBF3_CL_TIMING=1 under the quiet lock, else every case reports skipped, 77). Both run against the real ServerHost
# with sv's gunplay on the synthetic floor (no install).
openbf3_test(test_gunplay_client test_main.cpp host_gunplay_client_tests.cpp gunplay_fixture.h weapons_support.h test_support.h)
target_link_libraries(test_gunplay_client PRIVATE bf3_host)
target_compile_definitions(test_gunplay_client PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\""
    "OPENBF3_MECHANICS_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/weapon_mechanics.cfg\"")
# Scheduling (docs/testing.md; the labels at the end of this file): host.gunplay.client is a timing row (the merge
# pass's serial timing phase under the quiet lock; the dev-timing test preset sets OPENBF3_CL_TIMING=1, so it runs
# there and skips elsewhere). The suite host-gunplay-client-unit is split (docs/testing.md "Splits") like
# host-session-unit: host.gunplay.client.unit.pure runs the 5 socket-free cases (the events ring, present_weapon, the
# weapon lane, ClientProjectiles, the view sample) on every branch; host.gunplay.client.unit runs the 7 cases that
# play over loopback at 60 Hz of wall clock (E6, R2, SnapshotOthers, hostile, the three floor variants; about 90 s),
# which main ran RUN_SERIAL: beside a -j16 branch pass they failed 1 run in 10 (E15 floor 'reconciliations == 0',
# 2026-10-17), so they are a timing row (RUN_SERIAL, the merge pass's serial timing phase).
# host.gunplay.client.unit.split_cover proves every case is in exactly one of the two.
set(_openbf3_cl_pure *PresentationEvents*keeps*order* *present_weapon*from*two*predicted* *weapon*lane*replays*catch_up*
    *ClientProjectiles*own*world*rewound* *view*sample*present_view_quality*)
openbf3_case_row(NAME host.gunplay.client.unit.pure EXE test_gunplay_client SUITE host-gunplay-client-unit
    GROUP host-gunplay-client-unit COUNT 5 CASES ${_openbf3_cl_pure})
openbf3_case_row(NAME host.gunplay.client.unit EXE test_gunplay_client SUITE host-gunplay-client-unit
    GROUP host-gunplay-client-unit COUNT 7 EXCLUDE ${_openbf3_cl_pure})
openbf3_split_cover(NAME host.gunplay.client.unit.split_cover EXE test_gunplay_client SUITE host-gunplay-client-unit
    GROUP host-gunplay-client-unit)
add_test(NAME host.gunplay.client COMMAND test_gunplay_client --test-suite=host-gunplay-client)
set_tests_properties(host.gunplay.client.unit.pure PROPERTIES TIMEOUT 120)
set_tests_properties(host.gunplay.client.unit PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
set_tests_properties(host.gunplay.client PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 2400 RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_m2a_cl_t_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(host.gunplay.client.unit.pure host.gunplay.client.unit host.gunplay.client.unit.split_cover
        host.gunplay.client PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m2a_cl_t_dir}")
endif()

# M2a sv (docs/design/m2a.md §1 E1-E6, E17; docs/design/m2a-sv.md): host.gunplay.server, the server's gunplay criteria
# headless through ServerHost + ClientSession on Metro with the gunplay tables (cases timing, drop, stk, reload,
# variants, hostile, install; exits 77 without an install). The client paces itself at 60 Hz of wall clock (about
# 130 s alone, under a core) and the cases wait on wall-clock windows (a damage record within 0.5 s of its shot, a
# shot within 1 s of the press): beside a -j16 branch pass it failed 1 run in 3 (E3 / E4 'no kill', 2026-10-17), so it
# is a timing row (RUN_SERIAL, the merge pass's serial timing phase under the quiet lock), as main's RUN_SERIAL kept
# it alone. Also runs under the asan preset (the hostile case feeds every new message kind), RUN_SERIAL there too.
openbf3_test(test_host_gunplay_server test_main.cpp host_gunplay_server_tests.cpp gunplay_fixture.h test_support.h)
target_link_libraries(test_host_gunplay_server PRIVATE bf3_host)
target_compile_definitions(test_host_gunplay_server PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\""
    "OPENBF3_MECHANICS_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/weapon_mechanics.cfg\"")
add_test(NAME host.gunplay.server COMMAND test_host_gunplay_server --test-suite=host-gunplay-server)
set_tests_properties(host.gunplay.server PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 RUN_SERIAL TRUE)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_sv_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(host.gunplay.server PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_sv_cl_dir}")
endif()

# M2a vm (docs/design/m2a-vm.md; m2a.md §11.1, §11.2, E19, E5): anim.viewmodel.unit checks the 1p viewmodel
# animator, its springs, viewmodel_tuning.cfg and the ADS camera against the real weapon sim on the synthetic gate
# weapons, presented at 30 / 60 / 144 / 240 fps. No install, no Godot (godot.m2a_viewmodel is the install half).
openbf3_test(test_anim_viewmodel test_main.cpp anim_viewmodel_tests.cpp weapons_support.h test_support.h)
target_link_libraries(test_anim_viewmodel PRIVATE bf3_anim bf3_sim)
target_compile_definitions(test_anim_viewmodel PRIVATE "OPENBF3_VIEWMODEL_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/viewmodel_tuning.cfg\"")
add_test(NAME anim.viewmodel.unit COMMAND test_anim_viewmodel --test-suite=anim-viewmodel-unit)
set_tests_properties(anim.viewmodel.unit PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_vm_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(anim.viewmodel.unit PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_vm_cl_dir}")
endif()
# M2a vm: godot.m2a_viewmodel, the install half (res://scenes/m2a_viewmodel.tscn -- --vm-check; tests/godot_m2a_viewmodel.cmake).
# Headless and functional (the display frames are stepped, the load time is printed, never gated): it runs in the
# branch pass on its per-build cache root.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m2a_viewmodel godot_m2a_viewmodel.cmake HEADLESS TIMEOUT 480)
endif()

# M2a fx2 (docs/design/m2a-fx2.md): fx.render.unit, VFX2's render preparation (batch keys and order, back-to-front sort,
# the instance layout, alpha-level packing, default-texture fallbacks, the light merge and the 64-light pool) and the
# prepared atlas pixels (NormalDXT5RGA -> BC5, the cache blob's round trip, truncations, mutations; also in the asan
# preset). No install, no Godot.
openbf3_test(test_fx_render test_main.cpp fx_render_tests.cpp fx_test_support.h test_support.h)
target_link_libraries(test_fx_render PRIVATE bf3_fx)
add_test(NAME fx.render.unit COMMAND test_fx_render --test-suite=fx-render-unit)
set_tests_properties(fx.render.unit PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_fx2_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(fx.render.unit PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_fx2_cl_dir}")
endif()
# godot.fx_demo: the windowed VFX2 demo check (E8 (3), (4), (6), lights <= 64, the 1p pass, FX main-thread time,
# captures, install unchanged; 77 without OPENBF3_GODOT, a window or an install). godot.fx_ref: the Godot reference
# workloads and VFX2's 16k row (cost <= 1.25 x the MultiMesh 16k lit+soft row, GPU, S3 G1b windows; no install). Both
# are timing / windowed rows: WINDOW (label window, the merge pass's user-away phase, serially under the quiet lock).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.fx_demo godot_fx_demo.cmake WINDOW TIMEOUT 1500)
    openbf3_godot_row(godot.fx_ref godot_fx_ref.cmake WINDOW TIMEOUT 1500)
    set_tests_properties(godot.fx_demo godot.fx_ref PROPERTIES RUN_SERIAL TRUE)
endif()

# M2a hud (docs/design/m2a.md §1 E10, §11.4; docs/design/m2a-hud.md): ui.hud.feed.unit is the Godot-free half of E10
# (fill_hud over scripted frames, make_hud_static, the crosshair gap, the stage-to-window mapping, the screen layout on
# synthetic and hostile trees; no data); ui.hud.feed.install resolves the 12 kit items of §5.6 on the install (names
# 12/12 through the text database, the M67 through its UI description; _small icons 12/12; the hudmpscreen slots;
# install.unchanged; 77 without an install).
openbf3_test(test_ui_hud_feed test_main.cpp ui_hud_feed_tests.cpp weapons_support.h test_support.h)
target_link_libraries(test_ui_hud_feed PRIVATE bf3_host)
add_test(NAME ui.hud.feed.unit COMMAND test_ui_hud_feed --test-suite=ui-hud-feed-unit)
add_test(NAME ui.hud.feed.install COMMAND test_ui_hud_feed --test-suite=ui-hud-feed-install)
set_tests_properties(ui.hud.feed.unit PROPERTIES TIMEOUT 120)
set_tests_properties(ui.hud.feed.install PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_hud_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(ui.hud.feed.unit ui.hud.feed.install PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_hud_cl_dir}")
endif()
# M2a hud godot (docs/design/m2a-hud.md §5): godot.m2a_hud runs the windowed --hud-check of
# res://scenes/m2a_hud.tscn after a probe run (E10 at 1280 x 720, 1920 x 1080, 2560 x 1440 per frame of a scripted
# model; exit 77 = skipped without OPENBF3_GODOT, an install, a window or a rendering device); the PNGs go to
# $ENV{OPENBF3_HUD_SHOTS}, default C:/Users/olku/BF3 reimplementation/openBF3/b/shots/hud (gitignored, outside the
# worktree). It shares the project and its DLL with the other godot.* tests and reads the viewport image (RUN_SERIAL).
# WINDOW: label window, the merge pass's user-away phase.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m2a_hud godot_m2a_hud.cmake WINDOW TIMEOUT 600)
    set_tests_properties(godot.m2a_hud PROPERTIES RUN_SERIAL TRUE)
endif()

# M2a audio (docs/design/m2a-audio.md §10): audio.contract, every audio header compiles together, the node type
# table keeps the census sets, the seam hashes and FourCCs match the data's encoding, the tuning parser and the
# project's audio_tuning.cfg. No data.
openbf3_test(test_audio_contract test_main.cpp audio_contract_tests.cpp test_support.h)
target_link_libraries(test_audio_contract PRIVATE bf3_audio bf3_core)
target_compile_definitions(test_audio_contract PRIVATE OPENBF3_AUDIO_TUNING_CFG="${PROJECT_SOURCE_DIR}/bf3-opensource/config/audio_tuning.cfg")
add_test(NAME audio.contract COMMAND test_audio_contract --test-suite=audio-contract)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_audio_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(audio.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_audio_cl_dir}")
endif()
# M2a audio (docs/design/m2a-audio.md §10.1): the data-free unit suites, one ctest per suite.
openbf3_test(test_audio_unit test_main.cpp audio_unit_tests.cpp test_support.h)
target_link_libraries(test_audio_unit PRIVATE bf3_audio bf3_core)
foreach(_openbf3_audio_suite xas patch dsp mixer)
    add_test(NAME audio.${_openbf3_audio_suite}.unit COMMAND test_audio_unit --test-suite=audio-${_openbf3_audio_suite})
    if(OPENBF3_ASAN)
        set_tests_properties(audio.${_openbf3_audio_suite}.unit PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_audio_cl_dir}")
    endif()
endforeach()
# M2a audio (docs/design/m2a-audio.md §10.2): the install suites (exit 77 without an install).
openbf3_test(test_audio_install test_main.cpp audio_install_tests.cpp test_support.h)
target_link_libraries(test_audio_install PRIVATE bf3_audio bf3_core)
set_tests_properties(audio.xas.unit audio.patch.unit audio.dsp.unit audio.mixer.unit PROPERTIES TIMEOUT 300)
add_test(NAME audio.oracles COMMAND test_audio_install --test-suite=audio-oracles)
add_test(NAME audio.cache COMMAND test_audio_install --test-suite=audio-cache)
add_test(NAME audio.fuzz COMMAND test_audio_install --test-suite=audio-fuzz)
set_tests_properties(audio.oracles audio.cache audio.fuzz PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
if(OPENBF3_ASAN)
    set_tests_properties(audio.oracles audio.cache audio.fuzz PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_audio_cl_dir}")
endif()
# M2a audio gates G1-G4, G6, G7 (tests/audio_gates.cmake drives one bf3dump command each and checks its summary
# line). The timing gates (G3, G6) and the long renders run serially; under asan they only report.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    if(DEFINED ENV{OPENBF3_AUDIO_SHOTS})
        # Forward slashes: a Windows path (C:\...\b\shots\audio) would reach audio_gates.cmake as invalid escapes.
        file(TO_CMAKE_PATH "$ENV{OPENBF3_AUDIO_SHOTS}" _openbf3_audio_shots)
    else()
        set(_openbf3_audio_shots "${PROJECT_SOURCE_DIR}/b/shots/audio")
    endif()
    foreach(_openbf3_audio_gate waves loc decode_perf census bank m16_offline)
        string(REPLACE "_" "." _openbf3_audio_name "${_openbf3_audio_gate}")
        add_test(NAME audio.${_openbf3_audio_name} COMMAND ${CMAKE_COMMAND} -DBF3DUMP=$<TARGET_FILE:bf3dump> -DGATE=${_openbf3_audio_gate}
            -DSHOTS=${_openbf3_audio_shots} -DREPORT_ONLY=${OPENBF3_ASAN} -P ${CMAKE_CURRENT_SOURCE_DIR}/audio_gates.cmake)
        set_tests_properties(audio.${_openbf3_audio_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
        if(OPENBF3_ASAN)
            set_tests_properties(audio.${_openbf3_audio_name} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_audio_cl_dir}")
        endif()
    endforeach()
    # The timing gates run serially (label timing). audio.m16.offline is a long render, not timed: it runs in the
    # branch pass and only shares the shots WAVs with godot.audio (RESOURCE_LOCK).
    set_tests_properties(audio.decode.perf audio.bank PROPERTIES RUN_SERIAL TRUE)
    set_tests_properties(audio.m16.offline PROPERTIES RESOURCE_LOCK openbf3_shots_audio)
endif()
# M2a audio gate G8 (tests/godot_audio.cmake): the windowed M16A3 check of res://scenes/audio_m16.tscn; WAVs to the
# same shots directory as audio.m16.offline. Exit 77 without OPENBF3_GODOT, an install, or a window.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.audio godot_audio.cmake WINDOW TIMEOUT 300 ARGS -DSHOTS=${_openbf3_audio_shots})
    set_tests_properties(godot.audio PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK "openbf3_godot_project;openbf3_shots_audio")
endif()

# ---------------------------------------------------------------------------------------------
# Scheduling (docs/testing.md): the labels that decide which ctest phase runs a row, then PROCESSORS and COST.
# Unlabelled rows run in the parallel branch pass (the dev / debug / release test presets exclude the labels timing,
# realtime and window). Rows that do not exist in this configuration (no Godot, asan) are skipped.
function(openbf3_label p_label)
    foreach(row IN LISTS ARGN)
        if(TEST ${row})
            set_property(TEST ${row} APPEND PROPERTY LABELS ${p_label})
        endif()
    endforeach()
endfunction()
# realtime: waits on real time at under 0.5 core and gates latency behaviour (the merge pass's lane, -j8).
openbf3_label(realtime godot.m1.e1 godot.m1.e2_gate godot.m1.e2_stress host.session.gate host.session.stress host.session.e3
    host.server godot.s0_selftest host.scale.paced)
# timing: gates wall or CPU time (the merge pass, serially; RUN_SERIAL stays on each).
# host.session.unit left the real-time lane for the serial timing phase (S16 landing rule): beside the shards it
# needed a rerun in 2 of 4 lane runs (interp.compared > 300, target_buffer_ticks == 2; 2026-10-17).
# host.gunplay.client (M2a cl): E1 / E5, E12 lagcomp and E15's 10 min loopback on Metro, wall clock (main's build.cmd
# listed it among the timing rows); the dev-timing preset sets its OPENBF3_CL_TIMING=1.
# host.gunplay.server (M2a sv) and host.gunplay.client.unit (M2a cl): real-time play with wall-clock waits that failed
# beside a -j16 branch pass (their CMake comments).
openbf3_label(timing host.scale sim.nav.bake sim.weapons.perf tools.fx_bench audio.decode.perf audio.bank logic.perf
    tools.tc_cost host.gunplay.perf host.session.unit host.gunplay.client host.gunplay.server host.gunplay.client.unit)
# serial_wallclock: RUN_SERIAL in the branch pass for a wall-clock CHECK (host_scale_tests.cpp: the per-tick timer
# parts sum to >= 0.95 x the tick on 99 % of ticks).
openbf3_label(serial_wallclock host.scale.unit)
# merge_also: the memory gates run in the branch pass and again alone in the merge pass's timing phase, which decides.
openbf3_label(merge_also tools.ebx_memory tools.m1_prepare)
# asan_sweep (asan only): asan wall above 600 s after the in-test lanes; the asan-sweep preset, the full tier
# (docs/testing.md "Asan"). bigmem: peak above 8 GB.
# Measured in the first -j16 asan selection (2026-10-17): sim.vehicle 634 s; sim.nav and host.scale over their
# 1,200 s timeouts.
if(OPENBF3_ASAN)
    openbf3_label(asan_sweep sim.terrain sim.vehicle sim.nav host.scale host.scale.paced)
    openbf3_label(bigmem sim.terrain)
    openbf3_label(serial_wallclock core.gameplay.tables.unit)
    set_tests_properties(sim.nav host.scale PROPERTIES TIMEOUT 3600)
    set_tests_properties(host.scale.paced PROPERTIES TIMEOUT 1800)
    # Their real-time gates still bite under asan (asan-realtime at -j4 failed the clock-target, extrapolation and
    # E2 rest checks): they run alone inside the asan-realtime phase.
    set_tests_properties(host.session.gate PROPERTIES RUN_SERIAL TRUE)
endif()

# PROCESSORS = measured cpu / wall (rounded, >= 2); COST = measured serial seconds (rows over 5 s), so a fresh build
# directory without CTestCostData.txt still starts the critical path first. Measured on 925cd51 + S01 (dev, serial, 611 s pass);
# the rows S08-S15 put on lanes (sim.terrain, sim.nav, logic.census, sim.vehicle(.unit), fx.install, core.level.collision,
# core.ant) re-measured alone after those steps (jobrun cpu / wall, 2026-10-17).
function(openbf3_cost p_row p_processors p_cost)
    if(TEST ${p_row})
        if(p_processors GREATER 1)
            set_tests_properties(${p_row} PROPERTIES PROCESSORS ${p_processors})
        endif()
        set_tests_properties(${p_row} PROPERTIES COST ${p_cost})
    endif()
endfunction()
openbf3_cost(sim.terrain 9 117)
openbf3_cost(sim.terrain.worlds 2 25)
openbf3_cost(sim.nav 6 47)
openbf3_cost(logic.census 5 16)
openbf3_cost(sim.vehicle 3 14)
openbf3_cost(fx.install 5 14)
openbf3_cost(sim.vehicle.unit 2 8)
openbf3_cost(core.level.collision 3 10)
openbf3_cost(core.level 1 9)
openbf3_cost(prepare 1 5)
openbf3_cost(core.ant 1 3)
openbf3_cost(core.gameplay.weapons 1 5)
openbf3_cost(fx.fuzz 1 5)
openbf3_cost(audio.loc 1 5)
openbf3_cost(ui.minimap 1 5)
openbf3_cost(sim.world 2 4)
openbf3_cost(sim.motor 2 3)
openbf3_cost(core.install 2 3)
openbf3_cost(core.cache 2 1)
openbf3_cost(core.res.install 2 3)
openbf3_cost(audio.census 2 3)
openbf3_cost(tools.ebx_memory 2 2)
# godot.m2a_viewmodel (M2a vm), measured in the branch pass after the merge of main e1d574d (2026-10-17).
openbf3_cost(godot.m2a_viewmodel 1 7)
# The timing / realtime / window rows: COST only orders them inside their phase.
openbf3_cost(godot.m1.e1 1 150)
openbf3_cost(godot.m1.e2_gate 1 100)
openbf3_cost(godot.m1.e2_stress 1 100)
openbf3_cost(host.session.gate 1 100)
openbf3_cost(host.session.stress 1 100)
openbf3_cost(host.session.e3 1 10)
openbf3_cost(host.session.unit 1 70)
openbf3_cost(host.scale 1 30)
openbf3_cost(host.scale.paced 1 85)
openbf3_cost(sim.nav.bake 1 49)
openbf3_cost(godot.s3_crowd 1 270)
openbf3_cost(godot.l1_look 1 196)
openbf3_cost(godot.m1_load 1 49)
openbf3_cost(godot.audio 1 33)
openbf3_cost(audio.decode.perf 1 11)
openbf3_cost(tools.tc_cost 1 9)
# The M2a w2 timing rows, measured in the timing phase after the merge of main e1d574d (2026-10-17).
openbf3_cost(host.gunplay.client 1 988)
openbf3_cost(host.gunplay.server 1 127)
openbf3_cost(host.gunplay.client.unit 1 91)
openbf3_cost(godot.fx_ref 1 182)
openbf3_cost(godot.fx_demo 1 27)

# Every godot.* row goes through openbf3_godot_row (docs/testing.md "Godot rows").
get_property(_openbf3_all_tests DIRECTORY PROPERTY TESTS)
get_property(_openbf3_godot_rows GLOBAL PROPERTY OPENBF3_GODOT_ROWS)
foreach(_openbf3_row IN LISTS _openbf3_all_tests)
    if(_openbf3_row MATCHES "^godot[.]" AND NOT _openbf3_row STREQUAL "godot.project_setup"
            AND NOT _openbf3_row IN_LIST _openbf3_godot_rows)
        message(FATAL_ERROR "${_openbf3_row}: register godot.* rows with openbf3_godot_row(HEADLESS|WINDOW) (docs/testing.md)")
    endif()
endforeach()

# M2a w3 (W3.0 contract, docs/design/m2a-w3.md): m2a.contract3 compiles every Godot-free wave-3 interface header together
# (bf3_audio's weapon_audio / impact_audio / game_audio_check, bf3_fx's triggers, bf3_host's client_wiring and
# firefight_brain, the crowd additions, bf3_godot's m2a_game.h and m2a_check.h) and pins the POD shapes, enum values,
# defaults, the lane rule and the seed hashes, plus G1-G5 and the check's case parser. Rows that need a track's body
# print PENDING until the track sets its switch. No install; a contract row (<= 2 s).
openbf3_test(test_m2a_contract3 test_main.cpp m2a_contract3_tests.cpp gunplay_fixture.h test_support.h)
target_link_libraries(test_m2a_contract3 PRIVATE bf3_host bf3_anim bf3_fx bf3_audio bf3_render)
target_include_directories(test_m2a_contract3 PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_godot/src)
target_compile_definitions(test_m2a_contract3 PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME m2a.contract3 COMMAND test_m2a_contract3 --test-suite=m2a-contract3)
set_tests_properties(m2a.contract3 PROPERTIES TIMEOUT 300)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_m2a_w3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(m2a.contract3 PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m2a_w3_cl_dir}")
endif()

# M2a bt (docs/design/m2a.md §1 E13, E15, §10.2; docs/design/m2a-bt.md): the firefight bots on the real GameServer,
# headless on Metro TDM (load_firefight_level once per process, a per-PID scratch cache root). host.gunplay.firefight
# (functional, untimed: 64 Free bots for 60 s of simulated time, kills on both teams, every bot fires, no stuck bot, no
# soldier inside geometry or below the ground, no NaN; the idle observer peer is engaged) and host.gunplay.replay (E15,
# functional: three 3,600-tick runs on three threads) stay out of the timing label. host.gunplay.perf (E13, timing,
# RUN_SERIAL: 10 s warm-up + 120 s of 64 Pairs bots and one observer) runs in the merge pass only; report only under asan.
openbf3_test(test_host_gunplay_perf test_main.cpp host_gunplay_perf.cpp test_support.h)
target_link_libraries(test_host_gunplay_perf PRIVATE bf3_host)
target_compile_definitions(test_host_gunplay_perf PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\""
    "OPENBF3_MECHANICS_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/weapon_mechanics.cfg\"")
if(OPENBF3_ASAN)
    target_compile_definitions(test_host_gunplay_perf PRIVATE OPENBF3_TIMING_REPORT_ONLY=1)
endif()
add_test(NAME host.gunplay.firefight COMMAND test_host_gunplay_perf --test-suite=host-gunplay-firefight)
add_test(NAME host.gunplay.replay COMMAND test_host_gunplay_perf --test-suite=host-gunplay-replay)
add_test(NAME host.gunplay.perf COMMAND test_host_gunplay_perf --test-suite=host-gunplay-perf)
set_tests_properties(host.gunplay.firefight host.gunplay.replay PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
set_tests_properties(host.gunplay.perf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
openbf3_label(timing host.gunplay.perf)
# Measured alone (dev, 2026-10-18, warm machine): firefight 13.8 s wall / 30.9 s CPU (two runs and the Metro load with
# its cold per-PID cache), replay 11.4 s / 38.0 s (three runs on three threads), perf 12.4 s.
openbf3_cost(host.gunplay.firefight 2 14)
openbf3_cost(host.gunplay.replay 3 12)
openbf3_cost(host.gunplay.perf 1 13)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_m2a_bt_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # Under asan (2026-10-18, the two rows side by side on a loaded machine): firefight 563 s, replay 431 s.
    set_tests_properties(host.gunplay.firefight host.gunplay.replay host.gunplay.perf PROPERTIES TIMEOUT 1800)
    set_tests_properties(host.gunplay.firefight host.gunplay.replay host.gunplay.perf PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m2a_bt_cl_dir}")
endif()

# M2a tp (docs/design/m2a-tp.md): case "capsules" of anim.s3 (E18 (b)) builds the capsule pose table from the install
# through bf3_sim (weapon_install.h, posed_target.h).
target_link_libraries(test_anim_s3 PRIVATE bf3_sim)
# render.crowd_weapons (install, exits 77 without one): S3's soldier assets, the Metro TDM kits and every kit item's 3p
# weapon through load_crowd_weapons (game_crowd_items.h builds the lists), the binds, reload, muzzle, strip and views.
openbf3_test(test_render_crowd_weapons test_main.cpp render_crowd_weapons_tests.cpp test_support.h)
target_link_libraries(test_render_crowd_weapons PRIVATE bf3_render bf3_host)
target_include_directories(test_render_crowd_weapons PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_godot/src)
add_test(NAME render.crowd_weapons COMMAND test_render_crowd_weapons --test-suite=render-crowd-weapons)
set_tests_properties(render.crowd_weapons PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_tp_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(render.crowd_weapons PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_tp_cl_dir}")
endif()
# godot.m2a_tp_crowd (headless, functional; tests/godot_m2a_tp_crowd.cmake): Bf3GameCrowd in the level through
# Bf3TpCrowdCheck (res://scenes/m2a_tp_crowd.tscn -- --tp-check).
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m2a_tp_crowd godot_m2a_tp_crowd.cmake HEADLESS TIMEOUT 480)
endif()
# COST (docs/testing.md §4): both rows load the soldier assets, the level's gunplay tables and every kit item's 3p
# weapon from the install (no lighter subset proves the per-item meshes); alone 2.9 s (render.crowd_weapons) and 9.4 s
# (godot.m2a_tp_crowd, the crowd load 3.4 s of it), but 34 s and 39 s inside the 16-job branch pass (2026-10-18), so
# they start early.
openbf3_cost(render.crowd_weapons 1 5)
openbf3_cost(godot.m2a_tp_crowd 1 10)

# M2a au3 (docs/design/m2a-au3.md): the game's weapon audio. audio.router.unit runs the data-free audio-router suite
# (synthetic programs through WeaponAudioRouter, ImpactAudio and SimMixerClock on an offline MixerCore; a unit row,
# <= 2 s). The audio-game install rows (exit 77 without an install, install.unchanged in each): audio.game runs the
# kit items (E7 item 1), the offline E7 rehearsal and the scene for the ear (own fire, a reload, others by distance,
# impacts: gate 4 offline), WAVs and tables to shots/audio.game; audio.game.crowd (64 shooters for 60 s, functional
# counters); audio.game.underruns is the same render gating late_blocks = 0, a wall-clock gate (timing, main only).
openbf3_test(test_audio_router test_main.cpp audio_router_tests.cpp test_support.h)
target_link_libraries(test_audio_router PRIVATE bf3_audio bf3_host bf3_fx bf3_core)
target_compile_definitions(test_audio_router PRIVATE OPENBF3_AUDIO_TUNING_CFG="${PROJECT_SOURCE_DIR}/bf3-opensource/config/audio_tuning.cfg"
    OPENBF3_AU3_SHOTS="${CMAKE_BINARY_DIR}/shots")
add_test(NAME audio.router.unit COMMAND test_audio_router --test-suite=audio-router)
set_tests_properties(audio.router.unit PROPERTIES TIMEOUT 300)
# One process for the three cases that share the load (the fixture loads Metro's gunplay tables and the bank once).
add_test(NAME audio.game COMMAND test_audio_router --test-suite=audio-game "--test-case-exclude=64 shooters*")
add_test(NAME audio.game.crowd COMMAND test_audio_router --test-suite=audio-game "--test-case=64 shooters*")
add_test(NAME audio.game.underruns COMMAND test_audio_router --test-suite=audio-game "--test-case=64 shooters*")
set_tests_properties(audio.game audio.game.crowd audio.game.underruns PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
set_tests_properties(audio.game.underruns PROPERTIES RUN_SERIAL TRUE)
if(NOT OPENBF3_ASAN)
    set_tests_properties(audio.game.underruns PROPERTIES ENVIRONMENT "OPENBF3_AU3_GATE_LATE=1")
endif()
openbf3_label(timing audio.game.underruns)
openbf3_cost(audio.game 1 8)
openbf3_cost(audio.game.crowd 1 16)
openbf3_cost(audio.game.underruns 1 16)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_au3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(audio.router.unit audio.game audio.game.crowd audio.game.underruns PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_au3_cl_dir}")
endif()

# M2a fx3 (docs/design/m2a-fx3.md): VFX3's triggers. fx.triggers.unit: install-free (synthetic EBX effects and grid
# through the real library, cl's ClientProjectiles for case "late join"), a unit row (<= 2 s). fx.install.cells: the
# retail in-game cell selection (Metro's fire effects and collision cells, the E8 (1) / (3) / (5) / (7) items, the
# Caspian water fallback; install unchanged last), an install row; 77 without an install. Both in the asan preset.
openbf3_test(test_fx_triggers test_main.cpp fx_triggers_tests.cpp fx_test_support.h weapons_support.h test_support.h)
target_link_libraries(test_fx_triggers PRIVATE bf3_fx bf3_host bf3_render) # bf3_render: the decal textures as Bf3GameFx prepares them
add_test(NAME fx.triggers.unit COMMAND test_fx_triggers --test-suite=fx-triggers-unit)
add_test(NAME fx.install.cells COMMAND test_fx_triggers --test-suite=fx-install-cells)
set_tests_properties(fx.install.cells PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
# COST: 3-5.6 s alone (Metro's gunplay level, fx sources, collision and Caspian), so it starts early in a parallel pass.
openbf3_cost(fx.install.cells 1 5)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_fx3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(fx.triggers.unit fx.install.cells PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_fx3_cl_dir}")
endif()

# godot.fx3_demo: the windowed VFX3 demo check (Bf3GameFx end to end on Metro: load, draw, shots, impacts, decals, hit,
# 7 captures, install unchanged; docs/design/m2a-fx3.md §6). A window row (label window: the merge pass's user-away
# phase, serially); over 30 s because the level session loads Metro (warm on the row's own cache root) before the
# demo's scripted 15 s.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.fx3_demo godot_fx3_demo.cmake WINDOW TIMEOUT 900)
    set_tests_properties(godot.fx3_demo PROPERTIES RUN_SERIAL TRUE)
    openbf3_cost(godot.fx3_demo 1 31)
endif()

# M2a gx (docs/design/m2a-gx.md §5): the playable scene's in-game checks. godot.m2a runs --m2a-check on
# res://scenes/m2a_metro.tscn (range, reload, hud, ads, audio, fx; a skipped case names its track), godot.m2a.kits
# the 8 loadouts (E11), godot.m2a.play the scripted 5-minute play run (all three: 0 ERROR lines). Screenshots go to
# $ENV{OPENBF3_GX_SHOTS}, default C:/Users/olku/BF3 reimplementation/openBF3/b/shots/gx (gitignored, outside the
# worktree). Exit 77 = skipped without OPENBF3_GODOT, an install, a window or a rendering device, or while
# OPENBF3_USER_ACTIVE_FLAG exists. godot.m2a_firefight (E13 / E14 with bt's bots) and godot.m2a_load (E16 on the M2a
# scene: godot_m1_load.cmake with -DSCENE) are the integrator's merge-pass rows. All share the project and its DLL
# with the other godot.* rows and read the viewport (RUN_SERIAL). WINDOW: label window, the user-away tenure.
if(OPENBF3_BUILD_GODOT AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    openbf3_godot_row(godot.m2a godot_m2a.cmake WINDOW TIMEOUT 900 ARGS -DNO_ERRORS=1)
    openbf3_godot_row(godot.m2a.kits godot_m2a.cmake WINDOW TIMEOUT 900 ARGS -DCASES=kits -DNO_ERRORS=1)
    openbf3_godot_row(godot.m2a.play godot_m2a.cmake WINDOW TIMEOUT 1500 ARGS -DCASES=play -DNO_ERRORS=1 "-DARGS=--respawn=3")
    openbf3_godot_row(godot.m2a_firefight godot_m2a_firefight.cmake WINDOW TIMEOUT 900)
    openbf3_godot_row(godot.m2a_load godot_m1_load.cmake WINDOW TIMEOUT 300 ARGS -DSCENE=res://scenes/m2a_metro.tscn -DUSER_ARGS=--bots=0)
    set_tests_properties(godot.m2a godot.m2a.kits godot.m2a.play godot.m2a_firefight godot.m2a_load PROPERTIES RUN_SERIAL TRUE)
    openbf3_cost(godot.m2a 1 400)
    openbf3_cost(godot.m2a.kits 1 170)
    openbf3_cost(godot.m2a.play 1 330)
    openbf3_cost(godot.m2a_firefight 1 80)
    openbf3_cost(godot.m2a_load 1 50)
endif()

# M2b W1.0 (W1.0 contract, docs/design/m2b.md §11, §2.6; docs/design/m2b-w1.md): m2b.contract compiles every wave-1
# interface header together with tests/tdm_fixture.h and pins the shapes (PresentEvent 64 B, net::PlayerRow 32 B), the
# enum values of §2.6 #5 and #11, the W1.0 defaults, net::restrict_command, the M2a hashes a default load keeps and the
# W1.0 stubs (the v3 codecs, add_tdm_bots). Rows that need a track's body print PENDING until the track sets its
# switch. No install; a contract row (< 2 s), functional, parallel-safe.
openbf3_test(test_m2b_contract test_main.cpp m2b_contract_tests.cpp tdm_fixture.h gunplay_fixture.h test_support.h)
target_link_libraries(test_m2b_contract PRIVATE bf3_host bf3_audio)
target_compile_definitions(test_m2b_contract PRIVATE "OPENBF3_MOTOR_TUNING_CFG=\"${CMAKE_SOURCE_DIR}/bf3-opensource/config/motor_tuning.cfg\"")
add_test(NAME m2b.contract COMMAND test_m2b_contract --test-suite=m2b-contract)
set_tests_properties(m2b.contract PROPERTIES TIMEOUT 120)
if(OPENBF3_ASAN)
    get_filename_component(_openbf3_m2b_w1_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(m2b.contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_m2b_w1_cl_dir}")
endif()
