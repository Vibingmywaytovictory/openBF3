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

openbf3_test(test_res test_main.cpp res_tests.cpp test_support.h)
target_link_libraries(test_res PRIVATE bf3_core)

add_test(NAME core.guard COMMAND test_core --test-suite-exclude=install)
# Reads the retail install (read-only); exits 77 = skipped when no install is found.
add_test(NAME core.install COMMAND test_core --test-suite=install)
set_tests_properties(core.install PROPERTIES SKIP_RETURN_CODE 77)
# Resource decoders: synthetic cases, then the sample the parity oracle exported from the install
# (OPENBF3_RES_DATA, see README "Resource decoders"); exits 77 = skipped without it.
add_test(NAME core.res.unit COMMAND test_res --test-suite=res_unit)
add_test(NAME core.res COMMAND test_res --test-suite=res)
set_tests_properties(core.res PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(core.res.unit core.res PROPERTIES TIMEOUT 300)
add_test(NAME sim.jolt COMMAND test_sim)
add_test(NAME net.enet COMMAND test_net)
add_test(NAME host.server COMMAND test_host)
set_tests_properties(core.guard core.install sim.jolt net.enet host.server PROPERTIES TIMEOUT 120)
