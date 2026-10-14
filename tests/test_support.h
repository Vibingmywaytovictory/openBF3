#pragma once
// Shared by the doctest executables: install-dependent cases call skip_install_case() when no
// install is present; test_main.cpp then exits with 77 (ctest SKIP_RETURN_CODE).

namespace bf3::test {
void skip_install_case(const char *p_why);
bool any_case_skipped();
} // namespace bf3::test
