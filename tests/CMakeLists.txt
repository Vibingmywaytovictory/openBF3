# doctest executables for the Godot-free libraries. Run with: ctest --preset dev
function(openbf3_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    # Exceptions are off project-wide; a failing REQUIRE then aborts the executable.
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
    openbf3_module(${name})
endfunction()

openbf3_test(test_core test_main.cpp core_guard_tests.cpp core_install_tests.cpp test_support.h)
target_link_libraries(test_core PRIVATE bf3_core)
openbf3_test(test_sim test_main.cpp sim_tests.cpp test_support.h)
target_link_libraries(test_sim PRIVATE bf3_sim)
openbf3_test(test_net test_main.cpp net_tests.cpp test_support.h)
target_link_libraries(test_net PRIVATE bf3_net)
openbf3_test(test_host test_main.cpp host_tests.cpp test_support.h)
target_link_libraries(test_host PRIVATE bf3_host)

# Ant animation reader (bf3_core/ant) + the antdump support library used for census and JSON comparison.
openbf3_test(test_ant test_main.cpp ant_tests.cpp test_support.h)
target_link_libraries(test_ant PRIVATE bf3_core antdump_lib)

add_test(NAME core.guard COMMAND test_core --test-suite-exclude=install)
# Reads the retail install (read-only); exits 77 = skipped when no install is found.
add_test(NAME core.install COMMAND test_core --test-suite=install)
set_tests_properties(core.install PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME sim.jolt COMMAND test_sim)
add_test(NAME net.enet COMMAND test_net)
add_test(NAME host.server COMMAND test_host)
# core.ant reads an Ant export directory (OPENBF3_ANT_DATA) and the probe's clip JSONs (OPENBF3_ANT_ORACLE);
# exits 77 = skipped when they are absent. core.ant.unit needs no data.
add_test(NAME core.ant.unit COMMAND test_ant --test-suite=ant-unit)
add_test(NAME core.ant COMMAND test_ant --test-suite=ant)
set_tests_properties(core.ant PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.guard core.install sim.jolt net.enet host.server core.ant.unit core.ant PROPERTIES TIMEOUT 120)
