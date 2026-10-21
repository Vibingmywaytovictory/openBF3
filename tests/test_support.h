#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace bf3::test {

void skip_install_case(const char *p_why);
bool any_case_skipped();

inline bool wait_until(const std::function<bool()> &p_done, std::chrono::milliseconds p_timeout,
		std::chrono::milliseconds p_poll = std::chrono::milliseconds(0)) {
	const auto end = std::chrono::steady_clock::now() + p_timeout;
	while (!p_done()) {
		if (std::chrono::steady_clock::now() > end) {
			return false;
		}
		if (p_poll.count() > 0) {
			std::this_thread::sleep_for(p_poll);
		} else {
			std::this_thread::yield();
		}
	}
	return true;
}

struct Mounted {
	core::InstallReader reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
};

inline bool open_install(Mounted &r_m) {
	if (!r_m.reader.locate({})) {
		return false;
	}
	r_m.before = r_m.reader.snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(r_m.reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	r_m.install = *opened;
	return true;
}

inline bool open_or_skip(Mounted &r_m) {
	if (open_install(r_m)) {
		return true;
	}
	skip_install_case(r_m.reader.describe().c_str());
	return false;
}

inline Mounted *shared_mount_or_skip() {
	static Mounted m;
	static const bool opened = open_install(m);
	if (!opened) {
		skip_install_case(m.reader.describe().c_str());
		return nullptr;
	}
	return &m;
}

}
