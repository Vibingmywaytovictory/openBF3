#include "dedicated_main_loop.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "tdm_flow.h"

#include "bf3/core/install.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/map_list.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gd_extension_manager.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <filesystem>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;

namespace {

constexpr const char *kDefaultCacheRoot = "user://bf3cache-server";
constexpr const char *kConfigDirSetting = "openbf3/config/dir";
constexpr double kMiB = 1024.0 * 1024.0;
constexpr int32_t kMainLoopFps = 30;

std::string godot_path(const std::string &p_path) {
	if (p_path.rfind("res://", 0) == 0 || p_path.rfind("user://", 0) == 0) {
		return to_std(ProjectSettings::get_singleton()->globalize_path(to_godot(p_path)));
	}
	return p_path;
}

void say(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[dedicated] " + p_line));
}

std::string config_dir() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	return ps->has_setting(kConfigDirSetting) ? to_std(String(ps->get_setting(kConfigDirSetting))) : std::string();
}

std::string config_file(const char *p_name) {
	return to_std(to_godot(godot_path(config_dir() + "/" + p_name)).simplify_path());
}

std::string extensions_text() {
	const PackedStringArray loaded = GDExtensionManager::get_singleton()->get_loaded_extensions();
	std::string text;
	for (int64_t i = 0; i < loaded.size(); ++i) {
		text += (text.empty() ? "" : ", ") + to_std(loaded[i]);
	}
	std::string classes;
	const PackedStringArray all = ClassDB::get_class_list();
	for (int64_t i = 0; i < all.size(); ++i) {
		if (ClassDB::class_get_api_type(all[i]) == ClassDB::API_EXTENSION) {
			classes += (classes.empty() ? "" : ", ") + to_std(all[i]);
		}
	}
	return (text.empty() ? std::string("none") : text) + ", classes " +
			(classes.empty() ? std::string("none") : classes);
}

bool u32_arg(const char *p_name, double p_min, double p_max, uint32_t &r_value, std::string &r_error) {
	if (!has_user_arg(p_name)) {
		return true;
	}
	const std::string text = user_arg_value(p_name);
	if (core::parse_u32_value(text, p_min, p_max, r_value).fault != core::KeyFault::None) {
		if (r_error.empty()) {
			r_error = core::fmt("--%s=%s: not a whole number %.0f..%.0f", p_name, text.c_str(), p_min, p_max);
		}
		return false;
	}
	return true;
}

bool seconds_arg(const char *p_name, double p_min, double p_max, float &r_value, std::string &r_error) {
	if (!has_user_arg(p_name)) {
		return true;
	}
	const std::string text = user_arg_value(p_name);
	if (core::parse_float_value(text, p_min, p_max, r_value).fault != core::KeyFault::None) {
		if (r_error.empty()) {
			r_error = core::fmt("--%s=%s: not %.0f..%.0f seconds", p_name, text.c_str(), p_min, p_max);
		}
		return false;
	}
	return true;
}

int64_t monitor(Performance::Monitor p_monitor) {
	return int64_t(Performance::get_singleton()->get_monitor(p_monitor));
}

}

DedicatedMainLoop::~DedicatedMainLoop() {
	if (session_) {
		session_->stop();
	}
}

bool DedicatedMainLoop::parse_args(std::string &r_error) {
	DedicatedArgs &a = args_;
	a.level = user_arg_value("level");
	a.mode = user_arg_value("mode");
	a.maplist = user_arg_value("maplist");
	if (!a.maplist.empty() && !a.level.empty()) {
		r_error = "--maplist and --level exclude each other";
	}
	if (a.level.empty() && !a.mode.empty() && r_error.empty()) {
		r_error = "--mode needs --level";
	}
	a.cache_root = has_user_arg("bf3-cache") ? user_arg_value("bf3-cache") : std::string(kDefaultCacheRoot);
	if (a.cache_root.empty() && r_error.empty()) {
		r_error = "--bf3-cache needs a path";
	}
	if (has_user_arg("bind")) {
		a.bind_ip = user_arg_value("bind");
	}
	a.port_file = user_arg_value("port-file");
	u32_arg("port", 0.0, 65535.0, a.port, r_error);
	u32_arg("stop-after-kills", 0.0, 100000.0, a.stop_after_kills, r_error);
	u32_arg("stop-after-levels", 0.0, 1000.0, a.stop_after_levels, r_error);
	seconds_arg("intermission-s", 0.0, 3600.0, a.intermission_s, r_error);
	seconds_arg("load-stall-s", 1.0, 3600.0, a.load_stall_s, r_error);
	if (has_user_arg("dedicated-seconds")) {
		float seconds = 0.0f;
		if (core::parse_float_value(user_arg_value("dedicated-seconds"), 0.0, 1.0e7, seconds).fault !=
						core::KeyFault::None &&
				r_error.empty()) {
			r_error = "--dedicated-seconds=" + user_arg_value("dedicated-seconds") + ": not 0..10000000 seconds";
		}
		a.run_seconds = double(seconds);
	}
	uint32_t max_players = host::kListenMaxPlayers;
	u32_arg("max-players", 1.0, double(host::kServerMaxPlayers), max_players, r_error);
	a.rules.max_players = uint8_t(max_players);
	u32_arg("bots", 0.0, double(host::kServerMaxPlayers), a.rules.bots, r_error);
	if (a.rules.bots > a.rules.max_players && r_error.empty()) {
		r_error = core::fmt("--bots=%u: more than the %u players --max-players allows", a.rules.bots,
				unsigned(a.rules.max_players));
	}
	uint32_t seed = 1;
	u32_arg("seed", 0.0, 4294967295.0, seed, r_error);
	a.rules.seed = seed;
	TdmArgText text = tdm_arg_text();
	text.has_bots = false;
	text.has_deploy_screen = false;
	TdmOptions tdm;
	if (const core::Status s = parse_tdm_options(text, tdm); !s.ok() && r_error.empty()) {
		r_error = s.error().message;
	}
	a.rules.kill_limit = tdm.kill_limit;
	a.rules.preround_s = tdm.preround_s;
	a.rules.min_players = tdm.min_players;
	a.rules.dev_commands = has_user_arg("dev");
	a.rules.bot_tuning_cfg = config_path(config_file("bot_tuning.cfg").c_str());
	if (const std::string mechanics = config_path(config_file("weapon_mechanics.cfg").c_str()); !mechanics.empty()) {
		a.rules.mechanics_cfg = mechanics;
	}
	return r_error.empty();
}

bool DedicatedMainLoop::start(std::string &r_error) {
	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
	say("engine " + engine_summary());
	const String configs = to_godot(godot_path(config_dir())).simplify_path();
	if (configs.is_empty() || !DirAccess::dir_exists_absolute(configs)) {
		r_error = std::string("the config directory ") + kConfigDirSetting + " = " + config_dir() + " (" +
				to_std(configs) + ") does not exist";
		return false;
	}
	say("config " + to_std(configs));
	say("install " + install_.describe());
	if (!install_.is_mounted()) {
		r_error = "no usable install: " + install_.describe();
		return false;
	}
	install_before_ = install_.snapshot();
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(install_);
	if (!install) {
		r_error = install.error().message;
		return false;
	}

	host::ServerSessionConfig config;
	config.install = *install;
	if (!args_.level.empty()) {
		config.maps.entries.push_back(
				host::LevelEntry{ args_.level, args_.mode.empty() ? std::string("TeamDeathMatch0") : args_.mode, 1 });
	} else {
		const std::string path = godot_path(args_.maplist.empty() ? config_file("maplist.cfg") : args_.maplist);
		core::Result<host::MapList> maps = host::load_map_list(path);
		if (!maps) {
			r_error = maps.error().message;
			return false;
		}
		config.maps = std::move(*maps);
	}
	const bool first_only = args_.level.empty() && args_.maplist.empty() && config.maps.entries.size() > 1;
	if (first_only) {
		config.maps.entries.resize(1);
	}
	std::string rotation;
	for (const host::LevelEntry &e : config.maps.entries) {
		rotation += (rotation.empty() ? "" : ", ") + e.level + " " + e.mode + " x" + std::to_string(e.rounds);
	}
	say("map list " + rotation +
			(first_only ? std::string(" (the first entry of ") + config_file("maplist.cfg") +
									" in place: the game client cannot follow a level change yet; --maplist rotates)"
						: std::string()));

	const std::string root_text = godot_path(args_.cache_root);
	AcquiredCache acquired = acquire_cache(
			std::filesystem::path(std::u8string(root_text.begin(), root_text.end())), install_.write_guard());
	if (acquired.status == core::CacheStatus::Ok) {
		cache_ = acquired.cache;
		say("cache " + root_text);
	} else {
		say("cache " + root_text + " not used (" + core::cache_status_name(acquired.status) +
				(acquired.detail.empty() ? "" : ": ") + acquired.detail + ")");
	}
	config.cache = cache_;

	config.level.shared.gunplay = true;
	config.level.shared.tdm = true;
	config.level.shared.tdm_tuning_cfg = config_path(config_file("tdm_tuning.cfg").c_str());
	if (const std::string motor = config_path(config_file("motor_tuning.cfg").c_str()); !motor.empty()) {
		core::Result<sim::MotorTuning> tuning =
				sim::parse_motor_tuning(to_std(FileAccess::get_file_as_string(to_godot(motor))));
		if (!tuning) {
			r_error = motor + ": " + tuning.error().message;
			return false;
		}
		config.level.shared.tuning = *tuning;
	}
	config.level.seed = args_.rules.seed;
	config.rules = args_.rules;
	config.bind_ip = args_.bind_ip;
	config.port = uint16_t(args_.port);
	config.intermission_s = args_.intermission_s;
	if (args_.load_stall_s > 0.0f) {
		config.load.stall_s = args_.load_stall_s;
	}

	const host::ServerRules &r = config.rules;
	say(core::fmt("rules TDM: %u bots, max players %u%s, kill limit %s, pre-round %s, min players %s, dev commands %s",
			r.bots, unsigned(r.max_players),
			r.max_players != host::kListenMaxPlayers ? " (an override of the listen game's 32)" : "",
			r.kill_limit > 0 ? std::to_string(r.kill_limit).c_str() : "data",
			r.preround_s >= 0.0f ? core::fmt("%.1f s", r.preround_s).c_str() : "data",
			r.min_players >= 0 ? std::to_string(r.min_players).c_str() : "data",
			r.dev_commands ? "on (--dev)" : "off"));
	say(core::fmt("rotation: intermission %s, load deadlines: stalled %.1f s, warm %.1f s, cold %.1f s",
			config.intermission_s >= 0.0f ? core::fmt("%.1f s (--intermission-s)", config.intermission_s).c_str()
										  : "data",
			config.load.stall_s, config.load.warm_s, config.load.cold_s));

	session_ = std::make_unique<host::ServerSession>();
	const auto t0 = std::chrono::steady_clock::now();
	if (const core::Status s = session_->start(config); !s.ok()) {
		session_.reset();
		r_error = s.error().message;
		return false;
	}
	load_seconds_ = core::seconds_since(t0);
	const host::ServerLevel &l = session_->level();
	say(core::fmt("%s %s loaded in %.2f s: database %.2f s, combat areas %.2f s (%s), shared %.2f s (shapes %.2f, "
				  "tables %.2f, gunplay %.2f, logic %.2f), "
				  "bots %.2f s (nav %s)",
			l.entry.level.c_str(), l.entry.mode.c_str(), l.seconds.total, l.seconds.database, l.seconds.combat,
			l.combat_cache_hit ? "cache hit" : "baked", l.seconds.shared, l.shared->seconds_shapes,
			l.shared->seconds_tables, l.shared->seconds_gunplay, l.shared->seconds_logic, l.seconds.bots,
			l.nav_cache_hit ? "cache hit" : "baked"));
	say(core::fmt("ServerHost on %s:%u at 60 Hz%s", args_.bind_ip.c_str(), unsigned(session_->port()),
			args_.run_seconds > 0.0 ? core::fmt(", for %.1f s", args_.run_seconds).c_str() : ", until stopped"));
	if (!args_.port_file.empty()) {
		Ref<FileAccess> out = FileAccess::open(to_godot(godot_path(args_.port_file)), FileAccess::WRITE);
		if (out.is_null() || !out->store_string(to_godot(std::to_string(session_->port()) + "\n"))) {
			r_error = "--port-file=" + args_.port_file + ": cannot write";
			return false;
		}
		out->close();
	}
	resources_after_load_ = monitor(Performance::OBJECT_RESOURCE_COUNT);
	return true;
}

void DedicatedMainLoop::_initialize() {
	Engine::get_singleton()->set_max_fps(kMainLoopFps);
	say("extensions " + extensions_text());
	std::string error;
	if (!parse_args(error) || !start(error)) {
		say("failed: " + error);
		UtilityFunctions::push_error(to_godot("[dedicated] " + error));
		failed_ = true;
	}
}

bool DedicatedMainLoop::_physics_process(double p_delta) {
	(void)p_delta;
	return false;
}

void DedicatedMainLoop::report(const host::ServerSessionStats &p_stats) {
	const host::ServerStats &s = p_stats.server;
	const host::GameServerStats &g = p_stats.game;
	std::string prebake;
	for (const host::PrebakeEntry &e : p_stats.prebake) {
		prebake += core::fmt(", %s %s", e.entry.level.c_str(), host::prebake_state_name(e.state));
		if (e.state == host::PrebakeEntry::State::Done) {
			prebake += core::fmt(" %.1f s", e.seconds.total);
		}
	}
	const host::RotationStats &rot = p_stats.rotation;
	if (rot.level_changes > level_changes_seen_) {
		level_changes_seen_ = rot.level_changes;
		say(core::fmt("level change %llu: %s %s round %u, loaded in %.2f s, carried %u clients",
				(unsigned long long)rot.level_changes, rot.current.level.c_str(), rot.current.mode.c_str(),
				unsigned(rot.round_id), rot.last_load_s, g.carried_seats));
	}
	const std::string match = g.rules_active
			? core::fmt("%s %s round %u: %s US %d - RU %d of %d", rot.current.level.c_str(), rot.current.mode.c_str(),
					  unsigned(g.round_id), host::match_phase_name(g.match.phase), g.match.kills[0], g.match.kills[1],
					  g.match.kill_limit)
			: std::string("no rules");
	say(core::fmt("t=%.1f s tick %llu (%.2f Hz) p99 %.3f ms max %.3f ms overruns %llu | stages ms commands %.3f "
				  "sources %.3f rules %.3f tick %.3f | peers %u "
				  "clients %u (carried %u) bots %u | %s | levels changed %llu, in place %llu | stuck %u | squads "
				  "median %.0f m, "
				  "near %.1f %% of %llu, orders %llu, spawns on a mate %llu | private %.1f "
				  "MiB (peak %.1f) | kicks %s | godot nodes %lld resources "
				  "%lld textures %lld B video %lld B | prebake%s",
			elapsed_, (unsigned long long)s.ticks, s.measured_hz, s.p99_step_ms, s.max_step_ms,
			(unsigned long long)s.overruns, g.last_commands_ms, g.last_sources_ms, g.last_rules_ms, g.last_tick_ms,
			s.peers, g.clients, g.carried_seats, p_stats.bots, match.c_str(), (unsigned long long)rot.level_changes,
			(unsigned long long)rot.in_place, p_stats.stuck_bots, double(p_stats.squad_median_m),
			p_stats.squad_samples > 0 ? 100.0 * double(p_stats.squad_near) / double(p_stats.squad_samples) : 0.0,
			(unsigned long long)p_stats.squad_samples, (unsigned long long)p_stats.squad_orders,
			(unsigned long long)p_stats.squad_spawns, double(p_stats.private_bytes) / kMiB,
			double(p_stats.peak_private_bytes) / kMiB, host::kick_counts_text(g.kicks).c_str(),
			(long long)monitor(Performance::OBJECT_NODE_COUNT), (long long)monitor(Performance::OBJECT_RESOURCE_COUNT),
			(long long)monitor(Performance::RENDER_TEXTURE_MEM_USED),
			(long long)monitor(Performance::RENDER_VIDEO_MEM_USED), prebake.empty() ? " none" : prebake.c_str()));
}

bool DedicatedMainLoop::_process(double p_delta) {
	if (failed_) {
		return true;
	}
	elapsed_ += p_delta;
	report_timer_ += p_delta;
	const bool kills_due = args_.stop_after_kills > 0;
	const bool levels_due = args_.stop_after_levels > 0;
	bool levels_played = false;
	if (report_timer_ >= 1.0 || kills_due || levels_due) {
		const host::ServerSessionStats stats = session_->stats();
		levels_played = levels_due && stats.rotation.level_changes + 1u >= args_.stop_after_levels &&
				!stats.rotation.changing && stats.game.clients == 0;
		kills_ = stats.kills;
		stuck_ = stats.stuck_bots;
		private_bytes_ = stats.private_bytes;
		peak_private_bytes_ = stats.peak_private_bytes;
		if (report_timer_ >= 1.0) {
			report_timer_ = 0.0;
			report(stats);
		}
	}
	if (kills_due && kills_ >= int32_t(args_.stop_after_kills)) {
		stopped_by_ = "--stop-after-kills";
		return true;
	}
	if (levels_played) {
		stopped_by_ = "--stop-after-levels";
		return true;
	}
	if (args_.run_seconds > 0.0 && elapsed_ >= args_.run_seconds) {
		stopped_by_ = "--dedicated-seconds";
		return true;
	}
	return false;
}

void DedicatedMainLoop::_finalize() {
	if (!session_) {
		return;
	}
	const host::ServerSessionStats stats = session_->stats();
	report(stats);
	session_->stop();
	const core::SnapshotDiff diff = core::diff_snapshots(install_before_, install_.snapshot());
	const host::ServerLevel &l = session_->level();
	say(core::fmt("summary: %s %s loaded in %.2f s, played %.1f s, %llu ticks (%.2f Hz), tick p99 %.3f ms, overruns "
				  "%llu, kills %d, stuck %u, brain %s, decisions %llu, squad samples %llu, squad median %.1f m, "
				  "squad near %.1f %%, squad orders %llu, squad spawns %llu, private %.1f MiB "
				  "(peak %.1f MiB), godot nodes %lld, resources %+lld since the load, textures %lld B, dev commands "
				  "%s, install %s, stopped by %s",
			l.entry.level.c_str(), l.entry.mode.c_str(), load_seconds_, elapsed_,
			(unsigned long long)stats.server.ticks, stats.server.measured_hz, stats.server.p99_step_ms,
			(unsigned long long)stats.server.overruns, stats.kills, stats.stuck_bots, stats.brain,
			(unsigned long long)stats.bot_decisions, (unsigned long long)stats.squad_samples,
			double(stats.squad_median_m),
			stats.squad_samples > 0 ? 100.0 * double(stats.squad_near) / double(stats.squad_samples) : 0.0,
			(unsigned long long)stats.squad_orders, (unsigned long long)stats.squad_spawns,
			double(stats.private_bytes) / kMiB, double(stats.peak_private_bytes) / kMiB,
			(long long)monitor(Performance::OBJECT_NODE_COUNT),
			(long long)(monitor(Performance::OBJECT_RESOURCE_COUNT) - resources_after_load_),
			(long long)monitor(Performance::RENDER_TEXTURE_MEM_USED), args_.rules.dev_commands ? "on" : "off",
			diff.clean() ? "unchanged" : diff.summary().c_str(), stopped_by_));
	session_.reset();
	cache_.reset();
}

}
