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
add_test(NAME sim.jolt COMMAND test_sim)
add_test(NAME net.enet COMMAND test_net)
add_test(NAME host.server COMMAND test_host)
# bf3dump's JSON number text (the probe's f32/f64 rules) on synthetic values; no install needed.
add_test(NAME tools.numtext COMMAND bf3dump numcheck)
set_tests_properties(core.guard core.formats core.install core.fuzz core.cache sim.jolt net.enet host.server tools.numtext PROPERTIES TIMEOUT 120)
# core.install also reads every bundle manifest and decodes sample payloads (slower in Debug).
set_tests_properties(core.install PROPERTIES TIMEOUT 600)
set_tests_properties(core.cache_data PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
if(OPENBF3_ASAN)
    # The ASan runtime DLL ships next to cl.exe; put it on the tests' PATH.
    get_filename_component(_openbf3_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core.guard core.formats core.install core.fuzz core.cache core.cache_data sim.jolt net.enet host.server tools.numtext
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_openbf3_cl_dir}")
endif()
