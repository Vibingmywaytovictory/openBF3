#include "net_commands.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/math.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/load_bots.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/netsim.h"
#include "bf3/present/effect_seed.h"
#include "bf3/sim/soldier_motor.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

namespace bf3dump {

namespace {

using namespace bf3;
using namespace std::chrono_literals;

std::atomic<bool> g_stop{ false };

BOOL WINAPI on_console_ctrl(DWORD) {
	g_stop.store(true, std::memory_order_release);
	return TRUE;
}

void say(const std::string &p_line) {
	std::printf("%s\n", p_line.c_str());
	std::fflush(stdout);
}

enum class Script : uint8_t { Still, Walk, Fire, WalkFire };

struct NetArgs {
	std::string host;
	uint16_t port = 0;
	std::string level;
	std::string mode;
	std::string netsim = "off";
	Script script = Script::Walk;
	std::string script_name = "walk";
	float seconds = 0.0f;
	std::string stats_file;
	std::optional<uint64_t> build;
	std::string cache;
	std::string config_dir = OPENBF3_CONFIG_DIR;
	std::string motor_tuning;
	std::string name;
	uint32_t kit = 0;
	uint32_t count = 1;
	uint32_t stall_level = 0;
	uint32_t threads = 0;
	uint32_t seed = 1;
};

bool parse_script(std::string_view p_text, Script &r_script) {
	static constexpr std::pair<std::string_view, Script> kScripts[] = { { "still", Script::Still },
		{ "walk", Script::Walk }, { "fire", Script::Fire }, { "walkfire", Script::WalkFire } };
	for (const auto &[name, script] : kScripts) {
		if (p_text == name) {
			r_script = script;
			return true;
		}
	}
	return false;
}

std::string parse_net_args(const std::vector<std::string> &p_args, bool p_client, NetArgs &r_args) {
	std::vector<std::string> positional;
	for (const std::string &a : p_args) {
		const size_t eq = a.find('=');
		const std::string key = a.substr(0, eq);
		const std::string_view value =
				eq == std::string::npos ? std::string_view() : std::string_view(a).substr(eq + 1);
		bool ok = eq != std::string::npos;
		if (a.rfind("--", 0) != 0) {
			positional.push_back(a);
			continue;
		}
		if (key == "--netsim") {
			r_args.netsim = std::string(value);
			ok = ok && net::parse_netsim(value).ok();
		} else if (key == "--seconds") {
			ok = ok && core::parse_float_value(value, 0.0, 86400.0, r_args.seconds).fault == core::KeyFault::None;
		} else if (key == "--build-override") {
			uint64_t v = 0;
			const auto res = std::from_chars(value.data(), value.data() + value.size(), v);
			ok = ok && res.ec == std::errc() && res.ptr == value.data() + value.size();
			r_args.build = v;
		} else if (key == "--name") {
			r_args.name = std::string(value);
			ok = ok && !value.empty() && value.size() <= 24;
		} else if (key == "--kit") {
			ok = ok &&
					core::parse_u32_value(value, 0.0, double(net::kMaxKits - 1), r_args.kit).fault ==
							core::KeyFault::None;
		} else if (key == "--seed") {
			ok = ok && core::parse_u32_value(value, 0.0, 4294967295.0, r_args.seed).fault == core::KeyFault::None;
		} else if (p_client && key == "--script") {
			ok = ok && parse_script(value, r_args.script);
			r_args.script_name = std::string(value);
		} else if (p_client && key == "--stats-file") {
			r_args.stats_file = std::string(value);
			ok = ok && !value.empty();
		} else if (p_client && key == "--cache") {
			r_args.cache = std::string(value);
			ok = ok && !value.empty();
		} else if (p_client && key == "--config-dir") {
			r_args.config_dir = std::string(value);
			ok = ok && !value.empty();
		} else if (p_client && key == "--motor-tuning") {
			r_args.motor_tuning = std::string(value);
			ok = ok && !value.empty();
		} else if (p_client && key == "--threads") {
			ok = ok && core::parse_u32_value(value, 0.0, 64.0, r_args.threads).fault == core::KeyFault::None;
		} else if (!p_client && key == "--count") {
			ok = ok &&
					core::parse_u32_value(value, 1.0, double(net::kMaxSlots), r_args.count).fault ==
							core::KeyFault::None;
		} else if (!p_client && key == "--stall-level") {
			ok = ok && core::parse_u32_value(value, 0.0, 1000.0, r_args.stall_level).fault == core::KeyFault::None;
		} else {
			return "unknown option " + a;
		}
		if (!ok) {
			return "bad value in " + a;
		}
	}
	if (positional.size() != 3) {
		return "expected <host:port> <LEVEL> <MODE>";
	}
	const size_t colon = positional[0].rfind(':');
	uint32_t port = 0;
	if (colon == std::string::npos || colon == 0 ||
			core::parse_u32_value(std::string_view(positional[0]).substr(colon + 1), 1.0, 65535.0, port).fault !=
					core::KeyFault::None) {
		return "'" + positional[0] + "' is not <host:port>";
	}
	r_args.host = positional[0].substr(0, colon);
	r_args.port = uint16_t(port);
	r_args.level = positional[1];
	r_args.mode = positional[2];
	return {};
}

net::NetSimConfig netsim_of(const NetArgs &p_args, uint64_t p_salt) {
	net::NetSimConfig c = *net::parse_netsim(p_args.netsim);
	c.seed = uint64_t(p_args.seed) * 7919u + p_salt;
	return c;
}

struct ShotSeen {
	bool remote = false;
	uint8_t slot = 0;
	uint32_t id = 0;
	uint64_t seed = 0;
};

core::Result<std::shared_ptr<const host::SharedLevel>> load_client_level(const core::InstallReader &p_reader,
		const std::shared_ptr<core::Install> &p_install, const NetArgs &p_args, core::Cache &r_cache) {
	host::SharedLoadOptions o;
	o.threads = p_args.threads;
	o.gunplay = true;
	o.tdm = true;
	o.tdm_tuning_cfg = p_args.config_dir + "/tdm_tuning.cfg";
	const std::string motor =
			p_args.motor_tuning.empty() ? p_args.config_dir + "/motor_tuning.cfg" : p_args.motor_tuning;
	std::ifstream in(motor, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	if (!in) {
		return core::make_error(motor + ": cannot read");
	}
	core::Result<sim::MotorTuning> tuning = sim::parse_motor_tuning(text.str());
	if (!tuning) {
		return core::error_at(motor, tuning.error());
	}
	o.tuning = *tuning;
	if (!p_args.cache.empty()) {
		core::CacheOptions co;
		std::error_code ec;
		co.root = std::filesystem::absolute(std::filesystem::path(p_args.cache), ec);
		co.guard = p_reader.write_guard();
		std::string detail;
		if (ec || r_cache.open(std::move(co), &detail) != core::CacheStatus::Ok) {
			return core::make_error("cache " + p_args.cache + ": " + detail);
		}
		o.cache = &r_cache;
	}
	core::Result<host::LevelDatabase> db =
			host::open_level_database(p_install, p_args.level, p_args.mode, p_args.threads, nullptr);
	if (!db) {
		return db.error();
	}
	return host::load_shared_level(*db, o, nullptr);
}

}

const char *net_usage() {
	return "  client <host:port> <LEVEL> <MODE> [--netsim=<profile>] [--script=still|walk|fire|walkfire] "
		   "[--seconds=S]\n"
		   "      [--stats-file=<path>] [--build-override=N] [--cache=<dir>] [--config-dir=<dir>] "
		   "[--motor-tuning=<cfg>]\n"
		   "      [--name=N] [--kit=K] [--seed=N] [--threads=N]\n"
		   "  loadbots <host:port> <LEVEL> <MODE> [--count=N] [--seconds=S] [--netsim=<profile>] [--build-override=N]\n"
		   "      [--name=N] [--kit=K] [--seed=N] [--stall-level=N]\n";
}

int cmd_client(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install,
		const std::vector<std::string> &p_args) {
	NetArgs a;
	a.name = "client";
	if (const std::string error = parse_net_args(p_args, true, a); !error.empty()) {
		std::fprintf(stderr, "client: %s\n%s", error.c_str(), net_usage());
		return 2;
	}
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);
	const core::InstallSnapshot before = p_reader.snapshot();
	const auto t_load = std::chrono::steady_clock::now();
	core::Cache cache;
	core::Result<std::shared_ptr<const host::SharedLevel>> level = load_client_level(p_reader, p_install, a, cache);
	if (!level) {
		say("client: failed: " + level.error().message);
		return 1;
	}
	const host::SharedLevel &l = **level;
	say(core::fmt("client: loaded %s %s in %.2f s (shapes %.2f, tables %.2f, gunplay %.2f, logic %.2f), cache %s",
			l.level.c_str(), l.mode.c_str(), core::seconds_since(t_load), l.seconds_shapes, l.seconds_tables,
			l.seconds_gunplay, l.seconds_logic, a.cache.empty() ? "none" : a.cache.c_str()));

	host::ClientSessionConfig cc;
	cc.host = a.host;
	cc.port = a.port;
	cc.name = a.name;
	cc.game_build = a.build;
	cc.netsim = netsim_of(a, 1);
	cc.mechanics_cfg = a.config_dir + "/weapon_mechanics.cfg";
	host::ClientSession client;
	if (const core::Status s = client.start(cc, *level); !s.ok()) {
		say("client: failed: " + s.error().message);
		return 1;
	}
	say(core::fmt("client: connected to %s:%u as %s, netsim %s, script %s", a.host.c_str(), unsigned(a.port),
			a.name.c_str(), a.netsim.c_str(), a.script_name.c_str()));

	auto frame = std::make_unique<host::PresentationFrame>();
	std::vector<host::PresentEvent> scratch(host::kPresentationEventCapacity);
	std::vector<ShotSeen> shots;
	std::map<uint8_t, uint64_t> seen;
	host::InputSample input;
	std::optional<host::LegWalker> walker;
	sim::Vec3 life_spawn{};
	uint32_t lives = 0;
	float max_walk = 0.0f;
	float base_yaw = 0.0f;
	uint64_t seed_mismatches = 0;
	bool playing = false;
	bool joined = false;
	auto last_spawn_request = std::chrono::steady_clock::time_point();
	auto last_status = std::chrono::steady_clock::now();
	const auto t0 = std::chrono::steady_clock::now();
	const bool walks = a.script == Script::Walk || a.script == Script::WalkFire;
	const bool fires = a.script == Script::Fire || a.script == Script::WalkFire;
	host::ClientStats last{};
	while (!g_stop.load(std::memory_order_acquire)) {
		const double t = core::seconds_since(t0);
		if (a.seconds > 0.0f && t >= double(a.seconds)) {
			break;
		}
		last = client.stats();
		joined = joined || last.connected;
		if (!last.connected && (joined || t > double(cc.connect_timeout_ms) * 1e-3)) {
			break;
		}
		client.read_presentation(*frame);
		const host::PresentationFrame &f = *frame;
		if (!playing && f.valid) {
			playing = true;
			say(core::fmt(
					"client: playing slot %u, match seed %llu", unsigned(last.slot), (unsigned long long)f.match_seed));
		}
		const bool alive = host::present_own_alive(f);
		if (f.valid && f.own_curr.life > input.life) {
			input.life = f.own_curr.life;
			base_yaw = sim::yaw_radians(f.own_curr.yaw);
			input.yaw = base_yaw;
			input.pitch = 0.0f;
			walker.emplace(0u);
			life_spawn = f.own_curr.position;
			++lives;
			say(core::fmt("client: spawned life %u in slot %u at %.1f %.1f %.1f", unsigned(input.life),
					unsigned(last.slot), double(life_spawn[0]), double(life_spawn[1]), double(life_spawn[2])));
		}
		if (f.match_valid && !alive && std::chrono::steady_clock::now() - last_spawn_request >= 1s) {
			client.send_spawn_request(uint8_t(a.kit));
			last_spawn_request = std::chrono::steady_clock::now();
		}
		input.move_forward = 0.0f;
		input.held_buttons = 0;
		if (alive && walks && walker.has_value()) {
			const sim::Command c = walker->command(f.tick, f.own_curr);
			input.yaw = sim::yaw_radians(c.yaw);
			input.move_forward = 1.0f;
		}
		if (alive && fires) {
			if (!walks) {
				input.yaw = float(std::fmod(double(base_yaw) + t * 0.3, 6.283185307179586));
			}
			input.pitch = -0.2f;
			input.held_buttons = std::fmod(t, 3.0) < 0.5 ? sim::buttons::kFire : 0u;
		}
		if (alive && f.server_own_valid && f.server_own.life == input.life) {
			max_walk = std::max(max_walk, core::vec::length(core::vec::sub(life_spawn, f.server_own.position)));
		}
		for (uint32_t k = 0; k < std::min<uint32_t>(f.other_count, uint32_t(f.others.size())); ++k) {
			if (f.others[k].count > 0) {
				++seen[f.others[k].slot];
			}
		}
		host::set_view_sample(input, f, net::monotonic_ns());
		client.set_input(input);
		size_t n = 0;
		while ((n = client.events().drain(scratch)) > 0) {
			for (size_t i = 0; i < n; ++i) {
				const host::PresentEvent &e = scratch[i];
				if (e.kind != host::PresentKind::RemoteShot && e.kind != host::PresentKind::OwnShot) {
					continue;
				}
				const bool remote = e.kind == host::PresentKind::RemoteShot;
				shots.push_back({ remote, e.slot, e.id, e.seed });
				seed_mismatches += remote && e.seed != present::effect_seed(f.match_seed, e.slot, e.id) ? 1u : 0u;
			}
			if (n < scratch.size()) {
				break;
			}
		}
		if (std::chrono::steady_clock::now() - last_status >= 1s) {
			last_status = std::chrono::steady_clock::now();
			say(core::fmt("client: t=%.1f s slot %u life %u alive %d snapshots %llu commands %llu others %zu shots %zu "
						  "rtt %.1f ms",
					t, unsigned(last.slot), unsigned(input.life), int(alive),
					(unsigned long long)last.snapshots_received, (unsigned long long)last.commands_sent, seen.size(),
					shots.size(), last.rtt_ms));
		}
		std::this_thread::sleep_for(4ms);
	}
	const bool connected_at_end = last.connected;
	const uint64_t match_seed = frame->match_seed;
	client.stop();
	const core::SnapshotDiff diff = core::diff_snapshots(before, p_reader.snapshot());
	const size_t remote = size_t(std::count_if(shots.begin(), shots.end(), [](const ShotSeen &s) { return s.remote; }));
	std::string others;
	for (const auto &[slot, frames] : seen) {
		others += (others.empty() ? "" : ",") + std::to_string(slot);
	}
	const std::string error(last.last_error.data());
	const bool ok = connected_at_end && last.snapshots_received > 0 && last.commands_sent > 0 && lives > 0 &&
			!last.refused && before.valid && diff.clean();
	say(core::fmt(
			"client: summary slot %u, lives %u, snapshots %llu, commands %llu, max walk %.1f m, others seen [%s], "
			"remote shots %zu, own shots %zu, seed mismatches %llu, %s, install %s%s%s",
			unsigned(last.slot), lives, (unsigned long long)last.snapshots_received,
			(unsigned long long)last.commands_sent, double(max_walk), others.c_str(), remote, shots.size() - remote,
			(unsigned long long)seed_mismatches, connected_at_end ? "connected to the end" : "disconnected",
			before.valid && diff.clean() ? "unchanged" : ("CHANGED: " + diff.summary()).c_str(),
			error.empty() ? "" : ", error: ", error.c_str()));
	if (!a.stats_file.empty()) {
		std::ofstream out(a.stats_file, std::ios::binary);
		out << "slot " << unsigned(last.slot) << "\nmatch_seed " << match_seed << "\nlives " << lives << "\nsnapshots "
			<< last.snapshots_received << "\ncommands " << last.commands_sent << "\nmax_walk_m " << max_walk
			<< "\nconnected " << (connected_at_end ? 1 : 0) << "\n";
		for (const auto &[slot, frames] : seen) {
			out << "seen " << unsigned(slot) << ' ' << frames << '\n';
		}
		for (const ShotSeen &s : shots) {
			out << "shot " << (s.remote ? "remote " : "own ") << unsigned(s.slot) << ' ' << s.id << ' ' << s.seed
				<< '\n';
		}
		out << "end\n";
		if (!out) {
			say("client: cannot write " + a.stats_file);
			return 1;
		}
	}
	return ok ? 0 : 1;
}

int cmd_loadbots(const std::vector<std::string> &p_args) {
	NetArgs a;
	a.name = "loadbot";
	if (const std::string error = parse_net_args(p_args, false, a); !error.empty()) {
		std::fprintf(stderr, "loadbots: %s\n%s", error.c_str(), net_usage());
		return 2;
	}
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);
	host::LoadBotFleetConfig fc;
	fc.host = a.host;
	fc.port = a.port;
	fc.count = a.count;
	fc.bot.name = a.name;
	fc.bot.level = a.level;
	fc.bot.mode = a.mode;
	fc.bot.game_build = a.build;
	fc.bot.netsim = netsim_of(a, 2);
	fc.bot.kit = uint8_t(a.kit);
	fc.bot.stall_level = a.stall_level;
	host::LoadBotFleet fleet;
	if (const core::Status s = fleet.start(fc); !s.ok()) {
		say("loadbots: failed: " + s.error().message);
		return 1;
	}
	say(core::fmt("loadbots: %u connected to %s:%u", a.count, a.host.c_str(), unsigned(a.port)));
	const auto t0 = std::chrono::steady_clock::now();
	auto last_status = t0;
	while (!g_stop.load(std::memory_order_acquire)) {
		const double t = core::seconds_since(t0);
		if (a.seconds > 0.0f && t >= double(a.seconds)) {
			break;
		}
		const host::LoadBotFleetStats f = fleet.stats();
		if (f.disconnected + f.refused == a.count) {
			break;
		}
		if (std::chrono::steady_clock::now() - last_status >= 1s) {
			last_status = std::chrono::steady_clock::now();
			say(core::fmt("loadbots: t=%.1f s matched %u spawned %u snapshots %llu decode errors %llu disconnected %u "
						  "refused %u in %.0f B/s out %.0f B/s",
					t, f.matched, f.spawned, (unsigned long long)f.snapshots, (unsigned long long)f.decode_errors,
					f.disconnected, f.refused, f.bytes_in_per_s, f.bytes_out_per_s));
		}
		std::this_thread::sleep_for(50ms);
	}
	fleet.stop();
	const host::LoadBotFleetStats f = fleet.stats();
	bool ok = f.bots.size() == a.count;
	for (size_t i = 0; i < f.bots.size(); ++i) {
		const host::LoadBotStats &b = f.bots[i];
		const double s = b.ended_ns > b.connected_ns ? double(b.ended_ns - b.connected_ns) * 1e-9 : 0.0;
		const std::string error(b.last_error.data());
		say(core::fmt(
				"loadbot %zu: slot %u, lives %u, snapshots %llu (lost %llu), others %llu, shots %llu, combat %llu, "
				"reliable %llu, decode errors %llu, commands %llu, spawn requests %llu, max walk %.1f m, in %.0f B/s, "
				"out %.0f B/s, levels %u, level changes %u, round %u%s%s%s%s%s",
				i, unsigned(b.slot), b.lives, (unsigned long long)b.snapshots, (unsigned long long)b.snapshots_lost,
				(unsigned long long)b.snapshot_others, (unsigned long long)b.shot_events,
				(unsigned long long)b.combat_events, (unsigned long long)b.reliable_messages,
				(unsigned long long)b.decode_errors, (unsigned long long)b.commands_sent,
				(unsigned long long)b.spawn_requests, double(b.max_walk_m), s > 0.0 ? double(b.bytes_in) / s : 0.0,
				s > 0.0 ? double(b.bytes_out) / s : 0.0, b.levels, b.level_changes, b.round_id,
				b.stalled ? ", stalled" : "", b.kicked ? ", kicked " : "",
				b.kicked ? host::kick_reason_name(b.kick_reason) : "", b.disconnected ? ", disconnected" : "",
				error.empty() ? "" : ": ", error.c_str()));
		const bool stalled_as_asked = b.stalled && b.kicked && b.kick_reason == net::KickReason::LoadStalled;
		ok = ok && b.matched && b.lives > 0 && b.snapshots > 0 && b.decode_errors == 0 &&
				(!b.disconnected || stalled_as_asked) && !b.refused;
	}
	say(core::fmt(
			"loadbots: summary %zu bots, matched %u, spawned %u, snapshots %llu, decode errors %llu, disconnected %u, "
			"refused %u, in %.0f B/s, out %.0f B/s over %.1f s, level changes %u, stalled %u, kicked %u",
			f.bots.size(), f.matched, f.spawned, (unsigned long long)f.snapshots, (unsigned long long)f.decode_errors,
			f.disconnected, f.refused, f.bytes_in_per_s, f.bytes_out_per_s, f.seconds, f.level_changes, f.stalled,
			f.kicked));
	return ok ? 0 : 1;
}

}
