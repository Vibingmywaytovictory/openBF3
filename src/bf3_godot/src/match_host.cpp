#include "match_host.h"

#include "bf3_localisation.h"
#include "game_settings.h"
#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"
#include "minimap_hud.h"
#include "native_loading.h"
#include "native_screens.h"
#include "tdm_flow.h"
#include "ui_assets.h"
#include "ui_cursor.h"
#include "video_memory.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/core/ui/kick_popup.h"
#include "bf3/core/ui/ui_drawn_screens.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"
#include "bf3/host/server_level.h"
#include "bf3/net/master.h"
#include "bf3/net/netsim.h"
#include "bf3/render/level_prepare.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace sim = bf3::sim;

namespace {

Bf3MatchHost *g_current = nullptr;

void say(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[match] " + p_line));
}

constexpr double kKickPopupCheckSeconds = 30.0;
}

class NativeKick {
public:
	NativeKick(Node *p_parent, std::string p_reason, bool p_timed_out, bool p_check) :
			reason_(std::move(p_reason)), timed_out_(p_timed_out), check_(p_check) {
		layer_ = memnew(CanvasLayer);
		layer_->set_name("KickPopup");
		layer_->set_layer(kNativeLoadingLayer + 1);
		screens_ = memnew(Bf3UiScreens);
		screens_->set_name("KickScreens");
		screens_->set_manual(true);
		layer_->add_child(screens_);
		p_parent->add_child(layer_);
		cursor_ = Bf3UiCursor::ensure(p_parent);
		if (cursor_ != nullptr) {
			cursor_->set_follow_mouse(true);
			cursor_->show_cursor();
		}
		Input::get_singleton()->set_mouse_mode(
				cursor_ != nullptr ? Input::MOUSE_MODE_HIDDEN : Input::MOUSE_MODE_VISIBLE);
	}

	~NativeKick() {
		if (worker_.joinable()) {
			worker_.join();
		}
		screens_->set_resolver(nullptr);
		screens_->set_widget_listener(nullptr);
	}

	void frame(double p_delta) {
		if (!opened_ && !try_open()) {
			return;
		}
		screens_->step(std::clamp(p_delta, 0.0, kNativeLoadingMaxStepSeconds));
		if (check_ && !clicked_) {
			check(p_delta);
		}
	}

	void input(const Ref<InputEvent> &p_event) {
		if (!opened_) {
			return;
		}
		if (const InputEventKey *k = Object::cast_to<InputEventKey>(p_event.ptr())) {
			if (k->is_pressed() && !k->is_echo() &&
					(k->get_keycode() == KEY_ENTER || k->get_keycode() == KEY_KP_ENTER ||
							k->get_keycode() == KEY_SPACE)) {
				screens_->input_concept(int32_t(bf3::core::ui::UiInputConcept::Activate), true);
			}
		} else if (const InputEventMouseMotion *m = Object::cast_to<InputEventMouseMotion>(p_event.ptr())) {
			screens_->mouse_move(m->get_position());
		} else if (const InputEventMouseButton *b = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
			screens_->mouse_button(b->get_position(), int32_t(b->get_button_index()), b->is_pressed());
		}
	}

	bool quit() const { return quit_; }
	const std::string &failure() const { return failure_; }

private:
	void check(double p_delta) {
		namespace ui = bf3::core::ui;
		check_s_ += p_delta;
		if (screens_->phase(ui::kPopupGenericPartition) != UiScreenPhase::Shown) {
			if (check_s_ > kKickPopupCheckSeconds) {
				failure_ = bf3::core::fmt("popupgeneric not shown %.0f s after it opened", kKickPopupCheckSeconds);
			}
			return;
		}
		ui::HudScreen *s = screens_->hud_screen(ui::kPopupGenericPartition);
		const std::string root(ui::kHudScreenInstance);
		const std::string button = root + "." + std::string(ui::kKickPopupButtons) + ".holder.btn0";
		const std::optional<Vector2> at = screens_->clip_centre(ui::kPopupGenericPartition, button);
		if (s == nullptr || !at) {
			failure_ = "popupgeneric has no " + button;
			return;
		}
		say("kick popup: title \"" + s->clip(root + ".PopupGenericWidget_01.popupTitle").text() + "\", text \"" +
				s->clip(root + ".PopupGenericWidget_01.popupText").text() + "\", button \"" +
				s->clip(button + ".button.label.txt").text() + "\"" +
				bf3::core::fmt(", shown %.2f s after it opened; clicking it", check_s_));
		clicked_ = true;
		screens_->mouse_move(*at);
		screens_->mouse_button(*at, int32_t(MOUSE_BUTTON_LEFT), true);
		screens_->mouse_button(*at, int32_t(MOUSE_BUTTON_LEFT), false);
	}

	bool try_open() {
		Bf3Localisation *loc = Bf3Localisation::current();
		Bf3UiAssets *assets = Bf3UiAssets::current();
		if (loc == nullptr || assets == nullptr || !failure_.empty()) {
			failure_ = failure_.empty() ? std::string("no localisation or UI assets for the kick popup") : failure_;
			return false;
		}
		if (loc->has_failed()) {
			failure_ = to_std(loc->get_error());
			return false;
		}
		if (!loc->is_loaded() || !assets->is_loaded()) {
			return false;
		}
		if (!worker_.joinable() && !worker_done_.load(std::memory_order_acquire)) {
			worker_ = std::thread([this, install = loc->install()] {
				library_ = bf3::core::EbxLibrary::open(install, { bf3::core::gameplay::kDefaultWeaponLevel });
				worker_done_.store(true, std::memory_order_release);
			});
		}
		if (!worker_done_.load(std::memory_order_acquire)) {
			return false;
		}
		worker_.join();
		if (!library_) {
			failure_ = library_.error().message;
			return false;
		}
		namespace ui = bf3::core::ui;
		screens_->set_client(ui::hud_text_client(loc->database()));
		screens_->set_drawn(ui::kPopupGenericPartition, true);
		screens_->set_resolver(library_->get());
		screens_->set_widget_listener([this](const ui::HudWidgetEvent &p_event) {
			quit_ = quit_ || p_event.instance.find(ui::kKickPopupButtons) != std::string::npos;
		});
		const bf3::core::Status s = screens_->open_popup(
				ui::kPopupGenericPartition, ui::hud_kick_popup(reason_, timed_out_, *loc->database()));
		if (!s.ok()) {
			failure_ = s.error().message;
			return false;
		}
		opened_ = true;
		return true;
	}

	std::string reason_;
	bool timed_out_ = false;
	bool check_ = false;
	bool clicked_ = false;
	double check_s_ = 0.0;
	CanvasLayer *layer_ = nullptr;
	Bf3UiScreens *screens_ = nullptr;
	Bf3UiCursor *cursor_ = nullptr;
	std::thread worker_;
	std::atomic<bool> worker_done_{ false };
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> library_ = bf3::core::make_error("not opened");
	bool opened_ = false;
	bool quit_ = false;
	std::string failure_;
};

Bf3MatchHost::Bf3MatchHost() = default;

Bf3MatchHost::~Bf3MatchHost() {
	kick_.reset();
}

void Bf3MatchHost::_bind_methods() {}

Bf3MatchHost *Bf3MatchHost::current() {
	return g_current;
}

Bf3LevelSession *Bf3MatchHost::session() const {
	return Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
}

std::shared_ptr<host::ClientSession> Bf3MatchHost::client() const {
	std::lock_guard lock(mutex_);
	return client_;
}

bool Bf3MatchHost::parse_connect() {
	const std::string text = user_arg_value("connect");
	bf3::core::Result<bf3::net::ConnectTarget> target = bf3::net::parse_connect_target(text);
	if (!target) {
		end_match("--connect=" + text + ": " + target.error().message + "; expected host:port or name@host:port");
		return false;
	}
	connect_target_ = *target;
	connect_host_ = target->at.host;
	connect_port_ = target->at.port;
	return true;
}

void Bf3MatchHost::join_target() {
	if (!connect_target_.name.empty()) {
		say("asking the master " + connect_host_ + ":" + std::to_string(connect_port_) + " for " +
				connect_target_.name);
	}
	bf3::core::Result<bf3::net::Endpoint> at =
			bf3::net::resolve_connect_target(connect_target_, bf3::net::kMasterExchangeMs, &join_cancel_);
	if (!at) {
		std::lock_guard lock(mutex_);
		worker_error_ = "--connect=" + user_arg_value("connect") + ": " + at.error().message;
		return;
	}
	if (!connect_target_.name.empty()) {
		say("the master lists " + connect_target_.name + " at " + at->host + ":" + std::to_string(at->port));
	}
	connect_client(at->host, at->port);
}

void Bf3MatchHost::locate_install() {
	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
}

std::shared_ptr<bf3::core::Cache> Bf3MatchHost::open_cache() {
	const std::string root = user_arg_value("bf3-cache");
	const std::filesystem::path cache_root =
			root.empty() ? default_cache_root() : std::filesystem::path(std::u8string(root.begin(), root.end()));
	AcquiredCache acquired = acquire_cache(cache_root, install_.write_guard());
	if (acquired.status != bf3::core::CacheStatus::Ok) {
		return nullptr;
	}
	std::lock_guard lock(mutex_);
	cache_ = acquired.cache;
	return cache_;
}

std::shared_ptr<bf3::core::Cache> Bf3MatchHost::held_cache() const {
	std::lock_guard lock(mutex_);
	return cache_;
}

host::PrepareJob Bf3MatchHost::prepare_job() const {
	const bool meshes = !headless_;
	const bool shared_level = !has_user_arg("maplist");
	return [this, meshes, shared_level](const host::PrepareTask &p_task) -> bf3::core::Result<host::PrepareOutcome> {
		const std::shared_ptr<bf3::core::Cache> cache = held_cache();
		if (cache == nullptr) {
			return bf3::core::make_error("no cache to prepare into");
		}
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(install_);
		if (!install) {
			return install.error();
		}
		host::PrepareOutcome outcome;
		if (shared_level) {
			host::ServerLevelOptions options;
			options.shared.threads = p_task.threads;
			options.stop = p_task.stop;
			bf3::core::Result<host::ServerLevel> level = host::load_server_level(*install,
					host::LevelEntry{ p_task.next.level, p_task.next.mode, 1 }, options, cache.get(), p_task.progress);
			if (!level) {
				return level.error();
			}
			outcome.source_digest = level->shared->source_digest;
		}
		p_task.permille->store(500, std::memory_order_release);
		if (p_task.stop->load(std::memory_order_acquire)) {
			return bf3::core::make_error(host::kServerLevelStopped);
		}
		bf3::render::PrepareOptions options;
		options.threads = p_task.threads;
		options.meshes = meshes;
		bf3::core::Result<bf3::render::PrepareReport> report =
				bf3::render::prepare_level(*install, p_task.next.level, p_task.next.mode, cache.get(), options);
		if (!report) {
			return report.error();
		}
		outcome.cold = report->meshes.converted > 0 || report->weapons.converted > 0;
		return outcome;
	};
}

bool Bf3MatchHost::wait_for_prepare(const bf3::net::LevelChange &p_change) {
	if (prepare_ == nullptr) {
		return false;
	}
	const bool holds = prepare_->holds(p_change);
	if (waited_round_ != p_change.round_id) {
		waited_round_ = p_change.round_id;
		change_seen_at_ = std::chrono::steady_clock::now();
		prepare_held_ = holds;
	}
	if (holds) {
		return true;
	}
	const host::PrepareStatus s = prepare_->status();
	if (s.serial == 0 || !bf3::core::iequals_ascii(s.next.level, p_change.level) ||
			!bf3::core::iequals_ascii(s.next.mode, p_change.mode)) {
		say("prepare: none for " + p_change.level + " " + p_change.mode);
		return false;
	}
	std::string line = "prepare " + p_change.level + " " + p_change.mode + ": " +
			(s.state == bf3::net::PrepareState::Ready ? "ready" : "not ready") +
			bf3::core::fmt(" in %.2f s, ", s.seconds) +
			(s.skipped ? std::string("nothing to prepare")
					   : bf3::core::fmt("%s, target %.0f s", s.cold ? "cold" : "warm",
								 s.cold ? host::kColdPrepareTargetSeconds : host::kWarmLoadTargetSeconds));
	if (!s.error.empty()) {
		line += " (" + s.error + ")";
	}
	line += prepare_held_
			? bf3::core::fmt(", finished %.2f s after the level change", bf3::core::seconds_since(change_seen_at_))
			: std::string(", finished before the level change");
	say(line);
	return false;
}

std::optional<MatchLevel> Bf3MatchHost::first_level() const {
	return first_level_;
}

std::shared_ptr<GameSettings> Bf3MatchHost::settings() const {
	std::lock_guard lock(mutex_);
	return settings_;
}

void Bf3MatchHost::keep_settings(std::shared_ptr<GameSettings> p_settings) {
	std::lock_guard lock(mutex_);
	settings_ = std::move(p_settings);
}

bool Bf3MatchHost::prepare_listen_server() {
	const std::string text = user_arg_value("maplist");
	std::vector<std::string> tried = { text };
	if (text.rfind("res://", 0) == 0) {
		tried[0] = to_std(ProjectSettings::get_singleton()->globalize_path(to_godot(text)));
	} else {
		const std::string project = to_std(ProjectSettings::get_singleton()->globalize_path("res://"));
		tried.push_back(project + text);
		tried.push_back(project + "../" + text);
	}
	std::optional<bf3::core::Result<host::MapList>> maps;
	for (const std::string &path : tried) {
		if (std::error_code ec;
				std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())), ec)) {
			maps = host::load_map_list(path);
			break;
		}
	}
	if (!maps || !*maps) {
		end_match("--maplist=" + text + ": " + (maps ? maps->error().message : std::string("no such file")));
		return false;
	}
	server_config_.maps = std::move(**maps);
	if (server_config_.maps.entries.empty()) {
		end_match("--maplist=" + text + ": no entries");
		return false;
	}
	TdmOptions options;
	options.tdm_tuning_cfg = config_path("res://config/tdm_tuning.cfg");
	options.bot_tuning_cfg = config_path("res://config/bot_tuning.cfg");
	if (const bf3::core::Status s = parse_tdm_options(tdm_arg_text(), options); !s.ok()) {
		end_match(s.error().message);
		return false;
	}
	locate_install();
	if (!install_.is_mounted()) {
		end_match("no usable install: " + install_.describe());
		return false;
	}
	server_config_.level.shared.gunplay = true;
	server_config_.level.shared.tdm = true;
	server_config_.level.shared.tdm_tuning_cfg = options.tdm_tuning_cfg;
	const String motor = "res://config/motor_tuning.cfg";
	if (FileAccess::file_exists(motor)) {
		bf3::core::Result<sim::MotorTuning> tuning =
				sim::parse_motor_tuning(to_std(FileAccess::get_file_as_string(motor)));
		if (!tuning) {
			end_match(to_std(motor) + ": " + tuning.error().message);
			return false;
		}
		server_config_.level.shared.tuning = *tuning;
	}
	server_config_.level.seed = seed_;
	host::ServerRules &rules = server_config_.rules;
	rules.deploy_screen = options.deploy_screen;
	rules.kill_limit = options.kill_limit;
	rules.preround_s = options.preround_s;
	rules.min_players = options.min_players;
	rules.max_players = host::kListenMaxPlayers;
	rules.bots = std::min<uint32_t>(options.bots, kTdmMaxBots);
	rules.bot_tuning_cfg = options.bot_tuning_cfg;
	rules.dev_commands = true;
	rules.seed = seed_;
	server_config_.prebake = false;
	server_config_.load = host::kOwnClientLoadDeadlines;
	if (has_user_arg("bind")) {
		server_config_.bind_ip = user_arg_value("bind");
	}
	if (uint32_t port = 0; has_user_arg("port")) {
		if (bf3::core::parse_u32_value(user_arg_value("port"), 0.0, 65535.0, port).fault != bf3::core::KeyFault::None) {
			end_match("--port=" + user_arg_value("port") + ": not a port 0..65535");
			return false;
		}
		server_config_.port = uint16_t(port);
	}
	server_config_.master = user_arg_value("master");
	server_config_.name = has_user_arg("name") ? user_arg_value("name") : std::string("openBF3");
	if (has_user_arg("intermission-s")) {
		server_config_.intermission_s =
				float(std::clamp(std::atof(user_arg_value("intermission-s").c_str()), 0.0, 600.0));
	}
	const host::LevelEntry &first = server_config_.maps.entries.front();
	MatchLevel level;
	level.level = first.level;
	level.mode = first.mode;
	level.rules_flags =
			uint8_t(bf3::net::rules_flags::kTdm | (rules.deploy_screen ? bf3::net::rules_flags::kDeployScreen : 0u));
	level.rules_known = true;
	first_level_ = level;
	rules_flags_ = level.rules_flags;
	phase_ = Phase::Playing;
	std::string rotation;
	for (const host::LevelEntry &e : server_config_.maps.entries) {
		rotation += (rotation.empty() ? "" : ", ") + e.level + " " + e.mode + " x" + std::to_string(e.rounds);
	}
	say("listen server rotates " + rotation);
	return true;
}

void Bf3MatchHost::run_listen_server() {
	bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(install_);
	if (!install) {
		std::lock_guard lock(mutex_);
		worker_error_ = install.error().message;
		return;
	}
	server_config_.install = *install;
	if (!has_user_arg("no-cache")) {
		server_config_.cache = open_cache();
	}
	const std::string master = server_config_.master;
	const std::string name = server_config_.name;
	const std::string bind = server_config_.bind_ip;
	const std::string own = bind == "0.0.0.0" ? std::string("127.0.0.1") : bind;
	const bf3::core::Status s = server_->start(std::move(server_config_));
	if (!s.ok()) {
		std::lock_guard lock(mutex_);
		worker_error_ = s.error().message;
		return;
	}
	if (!master.empty()) {
		say("listen server on port " + std::to_string(server_->port()) + " heartbeats to the master " + master +
				" as " + name);
	}
	if (has_user_arg("upnp")) {
		const bf3::core::Status mapped = upnp_.open(bind, server_->port());
		say(mapped.ok() ? bf3::core::fmt("UPnP: UDP %u mapped on the gateway, external address %s",
								  unsigned(upnp_.port()), upnp_.external_address().c_str())
						: mapped.error().message);
	}
	connect_client(own, server_->port());
}

void Bf3MatchHost::_ready() {
	if (Engine::get_singleton()->is_editor_hint() || !(has_user_arg("connect") || has_user_arg("maplist"))) {
		set_process(false);
		set_process_unhandled_input(false);
		return;
	}
	g_current = this;
	headless_ = DisplayServer::get_singleton()->get_name() == String("headless");
	if (!user_arg_value("netsim").empty()) {
		netsim_ = user_arg_value("netsim");
	}
	if (has_user_arg("seed")) {
		seed_ = uint64_t(std::strtoull(user_arg_value("seed").c_str(), nullptr, 10));
	}
	if (const Bf3LevelSession *s = session()) {
		scene_game_ = s->has_meta("openbf3_game") && bool(s->get_meta("openbf3_game"));
		scene_tdm_ = s->has_meta("openbf3_tdm") && bool(s->get_meta("openbf3_tdm"));
	}
	if (has_user_arg("game-build")) {
		game_build_ = uint64_t(std::strtoull(user_arg_value("game-build").c_str(), nullptr, 10));
	}
	if (has_user_arg("mechanics")) {
		mechanics_cfg_ =
				to_std(ProjectSettings::get_singleton()->globalize_path(to_godot(user_arg_value("mechanics"))));
	}
	if (has_user_arg("own-test-rewind-ticks")) {
		own_test_rewind_ticks_ = std::clamp(
				std::atoi(user_arg_value("own-test-rewind-ticks").c_str()), 0, int32_t(host::kDefaultMaxRewindTicks));
	}
	if (has_user_arg("match-levels")) {
		levels_to_play_ = uint32_t(std::clamp(std::atoi(user_arg_value("match-levels").c_str()), 1, 1000));
	}
	if (has_user_arg("connect") ? !parse_connect() : !prepare_listen_server()) {
		return;
	}
	persistent_ui();
	if (has_user_arg("connect")) {
		locate_install();
	}
	if (install_.is_mounted() && !has_user_arg("no-cache")) {
		prepare_ = std::make_unique<host::ClientPrepare>(prepare_job());
	}
	if (has_user_arg("connect")) {
		say("connecting to " + user_arg_value("connect") + ": level, mode and rules come from the server");
		worker_ = std::thread([this] {
			if (prepare_ != nullptr) {
				open_cache();
			}
			join_target();
		});
	} else {
		server_ = std::make_unique<host::ServerSession>();
		worker_ = std::thread([this] { run_listen_server(); });
	}
	set_process(true);
}

void Bf3MatchHost::persistent_ui() {
	if (headless_) {
		return;
	}
	if (Bf3Localisation::current() == nullptr) {
		Bf3Localisation *localisation = memnew(Bf3Localisation);
		localisation->set_name("MatchLocalisation");
		add_child(localisation);
	}
	if (Bf3UiAssets::current() == nullptr) {
		Bf3UiAssets *assets = memnew(Bf3UiAssets);
		assets->set_name("MatchUiAssets");
		add_child(assets);
	}
}

host::ClientSessionConfig Bf3MatchHost::client_config() const {
	host::ClientSessionConfig cc;
	cc.host = connect_host_;
	cc.port = connect_port_;
	cc.name = "player";
	if (bf3::core::Result<bf3::net::NetSimConfig> ns = bf3::net::parse_netsim(netsim_)) {
		cc.netsim = *ns;
	}
	cc.netsim.seed = seed_ * 7919u + 1u;
	cc.request_dev_commands = true;
	cc.game_build = game_build_;
	if (!mechanics_cfg_.empty()) {
		cc.mechanics_cfg = mechanics_cfg_;
	}
	cc.own_test_max_rewind_ticks = own_test_rewind_ticks_;
	return cc;
}

void Bf3MatchHost::connect_client(const std::string &p_host, uint16_t p_port) {
	host::ClientSessionConfig cc = client_config();
	cc.host = p_host;
	cc.port = p_port;
	auto made = std::make_shared<host::ClientSession>();
	const bf3::core::Status s = made->start(cc, nullptr);
	std::lock_guard lock(mutex_);
	if (!s.ok()) {
		worker_error_ = s.error().message;
		return;
	}
	connect_host_ = p_host;
	connect_port_ = p_port;
	client_ = std::move(made);
}

void Bf3MatchHost::_process(double p_delta) {
	if (kick_ != nullptr) {
		kick_->frame(p_delta);
		if (kick_->quit()) {
			say("QUIT from the kick popup");
			get_tree()->quit(0);
		} else if (!kick_->failure().empty()) {
			say("no kick popup: " + kick_->failure());
			get_tree()->quit(1);
		}
		return;
	}
	if (ended_) {
		return;
	}
	watch_link();
	if (ended_) {
		return;
	}
	if (phase_ == Phase::Swapping) {
		make_session(pending_);
		phase_ = Phase::Playing;
		return;
	}
	const std::shared_ptr<host::ClientSession> c = client();
	if (c == nullptr) {
		return;
	}
	if (phase_ == Phase::Playing) {
		if (prepare_ != nullptr) {
			prepare_->poll(*c);
		}
		watch_changes();
		return;
	}
	bf3::net::LevelChange change;
	const bool changed = c->level_change(change);
	bf3::net::MatchInfo m;
	if (!c->lobby_match(m)) {
		return;
	}
	if (changed) {
		handled_round_ = change.round_id;
	}
	MatchLevel level;
	level.level = m.level;
	level.mode = m.mode;
	level.rules_flags = m.rules_flags;
	level.rules_known = m.phase != bf3::net::MatchPhase::LevelChange;
	level.index = level_index_;
	level.round_id = m.round_id;
	if (level.rules_known) {
		rules_flags_ = level.rules_flags;
	}
	free_session();
	make_session(level);
	phase_ = Phase::Playing;
}

void Bf3MatchHost::watch_changes() {
	const std::shared_ptr<host::ClientSession> c = client();
	bf3::net::LevelChange change;
	if (!c->level_change(change) || handled_round_ == change.round_id || wait_for_prepare(change)) {
		return;
	}
	handled_round_ = change.round_id;
	say("level change " + std::to_string(level_index_) + ": " + change.level + " " + change.mode + " round " +
			std::to_string(change.round_id) + ", load deadline " + std::to_string(change.load_deadline_s) + " s");
	++level_index_;
	free_session();
	host::tally_level(prediction_, c->stats());
	if (const bf3::core::Status s = c->detach_level(); !s.ok()) {
		end_match(s.error().message);
		return;
	}
	pending_ = MatchLevel{};
	pending_.level = change.level;
	pending_.mode = change.mode;
	pending_.rules_flags = rules_flags_.value_or(0);
	pending_.rules_known = rules_flags_.has_value();
	pending_.index = level_index_;
	pending_.round_id = change.round_id;
	phase_ = Phase::Swapping;
}

void Bf3MatchHost::watch_link() {
	std::string error;
	{
		std::lock_guard lock(mutex_);
		error = worker_error_;
		worker_error_.clear();
	}
	if (!error.empty()) {
		end_match(error);
		return;
	}
	const std::shared_ptr<host::ClientSession> c = client();
	if (c == nullptr) {
		return;
	}
	const host::ClientStats link = c->stats();
	if (!link.connected && link.last_error[0] != '\0') {
		timed_out_kick_ = link.kicked &&
				(link.kick_reason == bf3::net::KickReason::LoadTimeout ||
						link.kick_reason == bf3::net::KickReason::LoadStalled);
		kick_text_ = link.kicked ? std::string(link.kick_detail.data()) : std::string(link.last_error.data());
		end_match(std::string("the connection to ") + connect_host_ + ":" + std::to_string(connect_port_) +
				" ended: " + link.last_error.data());
	}
}

void Bf3MatchHost::free_session() {
	Node *parent = get_parent();
	if (Bf3LevelSession *s = session()) {
		s->shutdown_level();
		parent->remove_child(s);
		s->queue_free();
	}
	if (Node *minimap = get_node_or_null(NodePath("../MinimapHud"))) {
		parent->remove_child(minimap);
		minimap->queue_free();
	}
}

void Bf3MatchHost::make_session(const MatchLevel &p_level) {
	Node *parent = get_parent();
	Bf3LevelSession *s = memnew(Bf3LevelSession);
	s->set_name("Session");
	s->set_meta("openbf3_game", scene_game_);
	s->set_meta("openbf3_tdm", scene_tdm_);
	s->assign_level(p_level);
	parent->add_child(s);
	parent->move_child(s, get_index() + 1);
	Bf3MinimapHud *minimap = memnew(Bf3MinimapHud);
	minimap->set_name("MinimapHud");
	parent->add_child(minimap);
}

void Bf3MatchHost::end_match(const std::string &p_reason) {
	if (ended_) {
		return;
	}
	ended_ = true;
	UtilityFunctions::print(to_godot("[level] " + p_reason));
	if (!headless_ && Bf3Localisation::current() != nullptr) {
		free_session();
		set_process_unhandled_input(true);
		kick_ = std::make_unique<NativeKick>(
				this, kick_text_.empty() ? p_reason : kick_text_, timed_out_kick_, has_check_arg("kick-popup-check"));
		return;
	}
	if (Bf3LevelSession *s = session(); s != nullptr && s->level_assigned()) {
		s->finish(1);
		return;
	}
	get_tree()->quit(1);
}

std::string Bf3MatchHost::load_text(const Bf3LevelSession &p_session) const {
	const bool warm = host::CacheWarmth(p_session.progress().warmth.load()) == host::CacheWarmth::Warm;
	std::string text = bf3::core::fmt(", load %.2f s (%s, target %.0f s)", p_session.timings().done_from_ready_s,
			warm ? "warm" : "cold", warm ? host::kWarmLoadTargetSeconds : host::kColdLoadTargetSeconds);
	if (const std::shared_ptr<bf3::core::Cache> cache = held_cache()) {
		text += bf3::core::fmt(
				", cache %llu MiB", static_cast<unsigned long long>(cache->stats().bytes / (1024u * 1024u)));
	}
	return text;
}

void Bf3MatchHost::level_done(const Bf3LevelSession &p_session) {
	Performance *perf = Performance::get_singleton();
	const int64_t orphans = int64_t(perf->get_monitor(Performance::OBJECT_ORPHAN_NODE_COUNT));
	int64_t freeing = 0;
	const TypedArray<int> ids = Node::get_orphan_node_ids();
	for (int64_t i = 0; i < ids.size(); ++i) {
		for (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(uint64_t(int64_t(ids[i])))); n != nullptr;
				n = n->get_parent()) {
			if (n->is_queued_for_deletion()) {
				++freeing;
				break;
			}
		}
	}
	say("level " + std::to_string(level_index_) + " " + p_session.level_id() + " " + p_session.mode_id() +
			" done: private " + std::to_string(host::process_private_bytes() / (1024u * 1024u)) + " MiB, " +
			process_video_memory_text() + ", nodes " +
			std::to_string(int64_t(perf->get_monitor(Performance::OBJECT_NODE_COUNT))) + ", orphan nodes " +
			std::to_string(std::max<int64_t>(orphans - freeing, 0)) + ", " + std::to_string(freeing) +
			" queued for deletion, objects " + std::to_string(int64_t(perf->get_monitor(Performance::OBJECT_COUNT))) +
			", resources " + std::to_string(int64_t(perf->get_monitor(Performance::OBJECT_RESOURCE_COUNT))) +
			load_text(p_session));
}

void Bf3MatchHost::_unhandled_input(const Ref<InputEvent> &p_event) {
	if (kick_ != nullptr) {
		kick_->input(p_event);
	}
}

void Bf3MatchHost::_exit_tree() {
	if (g_current == this) {
		g_current = nullptr;
	}
	if (server_ != nullptr) {
		server_->request_stop();
	}
	join_cancel_.store(true, std::memory_order_release);
	if (worker_.joinable()) {
		worker_.join();
	}
	prepare_.reset();
	std::shared_ptr<host::ClientSession> c = client();
	if (c != nullptr) {
		host::tally_level(prediction_, c->stats());
		say(host::prediction_tally_text(prediction_));
		c->stop();
	}
	kick_.reset();
	upnp_.close();
	server_.reset();
}

}
