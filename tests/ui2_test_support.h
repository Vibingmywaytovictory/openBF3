#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace bf3::test::ui2 {

struct InstallFixture {
	core::InstallReader reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	std::string error;
};

inline InstallFixture *install() {
	static InstallFixture *fixture = [] {
		InstallFixture *f = new InstallFixture();
		if (!f->reader.locate({})) {
			f->error = f->reader.describe();
			return f;
		}
		f->before = f->reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(f->reader);
		if (!opened) {
			f->error = opened.error().message;
			return f;
		}
		f->install = *opened;
		return f;
	}();
	if (!fixture->install) {
		bf3::test::skip_install_case(fixture->error.c_str());
		return nullptr;
	}
	return fixture;
}

inline void row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	std::printf("UI2 ROW %s %s %s\n", p_name.c_str(), p_pass ? "PASS" : "FAIL", p_detail.c_str());
	std::fflush(stdout);
}

inline void check_install_unchanged() {
	InstallFixture *f = install();
	if (f == nullptr) {
		return;
	}
	const core::InstallSnapshot after = f->reader.snapshot();
	const core::SnapshotDiff d = core::diff_snapshots(f->before, after);
	const size_t files = f->before.files.size();
	row("install.unchanged", d.clean() && f->before.valid, std::to_string(files) + " index files (" + std::to_string(f->before.toc_count) + " toc, " +
			std::to_string(f->before.sb_count) + " sb, " + std::to_string(f->before.cat_count) + " cat)" + (d.clean() ? "" : ": " + d.summary()));
	CHECK(f->before.valid);
	CHECK(d.clean());
}

inline size_t fuzz_iters(size_t p_default) {
	char buf[32] = {};
	size_t len = 0;
	if (getenv_s(&len, buf, sizeof(buf), "OPENBF3_FUZZ_ITERS") == 0 && len > 1) {
		const long v = std::strtol(buf, nullptr, 10);
		if (v > 0) {
			return size_t(v);
		}
	}
	return p_default;
}

}
