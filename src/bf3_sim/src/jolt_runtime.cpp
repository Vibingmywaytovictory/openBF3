#include "jolt_runtime.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace bf3::sim {

namespace {

std::mutex g_jolt_mutex;
int g_jolt_users = 0;
int g_jolt_token = 0; // the handles point here (never dereferenced)

void jolt_trace(const char *p_fmt, ...) {
	va_list args;
	va_start(args, p_fmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	std::fprintf(stderr, "[jolt] %s\n", buf);
}

void jolt_acquire() {
	std::lock_guard lock(g_jolt_mutex);
	if (g_jolt_users++ == 0) {
		JPH::RegisterDefaultAllocator();
		JPH::Trace = jolt_trace;
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();
	}
}

void jolt_release() {
	std::lock_guard lock(g_jolt_mutex);
	if (--g_jolt_users == 0) {
		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

} // namespace

std::shared_ptr<void> jolt_runtime_acquire() {
	jolt_acquire();
	return std::shared_ptr<void>(&g_jolt_token, [](void *) { jolt_release(); });
}

} // namespace bf3::sim
