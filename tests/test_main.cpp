#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "test_support.h"

#include <atomic>
#include <cstdio>

namespace bf3::test {
namespace {
std::atomic<bool> g_skipped{ false };
}
void skip_install_case(const char *p_why) {
	g_skipped = true;
	std::printf("SKIPPED: %s\n", p_why);
}
bool any_case_skipped() {
	return g_skipped;
}
}

int main(int argc, char **argv) {
	doctest::Context ctx(argc, argv);
	const int result = ctx.run();
	if (ctx.shouldExit() || result != 0) {
		return result;
	}
	return bf3::test::any_case_skipped() ? 77 : 0;
}
