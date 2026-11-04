#pragma once

#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "test_support.h"
#include "win32_text.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bf3::test::procs {

using namespace std::chrono_literals;
using bf3::core::detail::path_utf8;

constexpr std::string_view kLevel = "MP_Subway";
constexpr std::string_view kMode = "TeamDeathMatch0";
constexpr std::string_view kServerLine = "[dedicated] ServerHost on 127.0.0.1:";

inline bool need(bool p_ok, const std::string &p_why) {
	CHECK_MESSAGE(p_ok, p_why);
	return p_ok;
}

struct Server {
	size_t id = 0;
	uint16_t port = 0;
};

inline std::optional<Server> start_server(ProcDriver &r_driver, uint32_t p_bots, uint32_t p_seconds,
		std::vector<std::string> p_args = { "--level=" + std::string(kLevel), "--mode=" + std::string(kMode) },
		std::string_view p_role = "server") {
	p_args.insert(p_args.end(),
			{ "--bots=" + std::to_string(p_bots), "--preround=0", "--dedicated-seconds=" + std::to_string(p_seconds) });
	const ChildSpec spec = r_driver.server_child(p_role, std::move(p_args));
	if (const std::string why = r_driver.skip_reason(spec); !why.empty()) {
		bf3::test::skip_install_case(why.c_str());
		return std::nullopt;
	}
	bf3::core::Result<size_t> id = r_driver.start(spec);
	if (!need(id.ok(), id.ok() ? std::string() : id.error().message)) {
		return std::nullopt;
	}
	std::string line;
	const bool printed = r_driver.wait_line(
			*id, [](std::string_view l) { return contains(l, kServerLine) || contains(l, "[dedicated] failed: "); },
			300s, &line);
	if (printed && contains(line, "no usable install")) {
		r_driver.finish(*id, 30s);
		bf3::test::skip_install_case(line.c_str());
		return std::nullopt;
	}
	if (!need(printed && contains(line, kServerLine), r_driver.output(*id))) {
		return std::nullopt;
	}
	const std::string_view digits = std::string_view(line).substr(line.find(kServerLine) + kServerLine.size());
	uint32_t port = 0;
	std::from_chars(digits.data(), digits.data() + digits.size(), port);
	if (!need(port > 0 && port < 65536, line)) {
		return std::nullopt;
	}
	return Server{ *id, uint16_t(port) };
}

inline std::vector<std::string> client_args(
		ProcDriver &r_driver, std::string_view p_role, uint16_t p_port, std::vector<std::string> p_extra) {
	const bf3::test::procs::ChildRoots r = r_driver.roots(p_role);
	std::error_code ec;
	std::filesystem::create_directories(r.cache, ec);
	std::vector<std::string> args = { "client", "127.0.0.1:" + std::to_string(p_port), std::string(kLevel),
		std::string(kMode), "--cache=" + path_utf8(r.cache), "--stats-file=" + path_utf8(r.dir / L"stats.txt") };
	args.insert(args.end(), p_extra.begin(), p_extra.end());
	return args;
}

inline std::optional<size_t> start_tool(
		ProcDriver &r_driver, std::string_view p_role, std::vector<std::string> p_args) {
	bf3::core::Result<size_t> id =
			r_driver.start(r_driver.tool_child(p_role, r_driver.config().bf3dump, std::move(p_args)));
	if (!need(id.ok(), id.ok() ? std::string() : id.error().message)) {
		return std::nullopt;
	}
	return *id;
}

inline bool line_starts(ProcDriver &r_driver, size_t p_child, std::string_view p_prefix,
		std::chrono::milliseconds p_timeout, std::string *r_line = nullptr) {
	return r_driver.wait_line(
			p_child, [&](std::string_view l) { return l.rfind(p_prefix, 0) == 0; }, p_timeout, r_line);
}

inline void require_clean_exit(ProcDriver &r_driver, size_t p_child, const ChildEnd &p_end, const char *p_what) {
	CHECK_MESSAGE(p_end.exited, p_what);
	CHECK_MESSAGE(p_end.code == 0, p_what << " exited " << p_end.code << ":\n" << r_driver.output(p_child));
	CHECK(p_end.stragglers == 0);
	CHECK(r_driver.job_processes(p_child) == 0);
}

inline uint64_t summary_number(std::string_view p_line, std::string_view p_key) {
	const size_t at = p_line.find(p_key);
	if (at == std::string_view::npos) {
		return UINT64_MAX;
	}
	const std::string_view tail = p_line.substr(at + p_key.size());
	uint64_t v = 0;
	const auto res = std::from_chars(tail.data(), tail.data() + tail.size(), v);
	return res.ec == std::errc() ? v : UINT64_MAX;
}

inline std::optional<double> number_after(std::string_view p_text, std::string_view p_key) {
	const size_t at = p_text.find(p_key);
	if (at == std::string_view::npos) {
		return std::nullopt;
	}
	const std::string_view tail = p_text.substr(at + p_key.size());
	double v = 0.0;
	const auto res = std::from_chars(tail.data(), tail.data() + tail.size(), v);
	return res.ec == std::errc() ? std::optional<double>(v) : std::nullopt;
}

inline bool kicks_none(std::string_view p_line) {
	const size_t at = p_line.find("| kicks ");
	if (at == std::string_view::npos) {
		return false;
	}
	const size_t start = at + 8;
	const size_t end = p_line.find(" |", start);
	std::istringstream s(std::string(p_line.substr(start, end - start)));
	std::string token;
	bool any = false;
	while (s >> token) {
		any = true;
		if (token.size() < 2 || token.substr(token.size() - 2) != "=0") {
			return false;
		}
	}
	return any;
}

inline ChildSpec godot_client(
		ProcDriver &r_driver, uint16_t p_port, std::vector<std::string> p_extra, std::string_view p_role = "client") {
	std::vector<std::string> args = { "--connect=127.0.0.1:" + std::to_string(p_port) };
	args.insert(args.end(), p_extra.begin(), p_extra.end());
	return r_driver.godot_child(p_role, { "--headless", "res://scenes/metro_tdm.tscn" }, std::move(args));
}

}
