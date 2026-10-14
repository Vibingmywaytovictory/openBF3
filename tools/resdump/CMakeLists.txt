# resdump: canonical summary of exported BF3 resources through bf3::core::res (parity tool).
add_executable(resdump resdump.cpp)
target_link_libraries(resdump PRIVATE bf3_core)
openbf3_module(resdump)
