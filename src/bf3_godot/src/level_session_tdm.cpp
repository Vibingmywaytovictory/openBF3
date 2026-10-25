#include "level_session_tdm.h"

#include "level_session_game.h"

#include "bf3_localisation.h"
#include "fp_camera.h"
#include "game_crowd.h"
#include "godot_util.h"
#include "hud_items.h"
#include "input_mapper.h"
#include "native_flow.h"
#include "native_hud.h"
#include "native_screens.h"
#include "ui_assets.h"
#if OPENBF3_BUILD_CHECKS
#include "native_screens_check.h"
#endif

#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/game_server.h"
#include "bf3/host/kits.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace net = bf3::net;
namespace sim = bf3::sim;

Bf3UiScreens *tdm_ui_screens(const TdmState &p_m) {
	return p_m.ui_screens_id != 0 ? Object::cast_to<Bf3UiScreens>(ObjectDB::get_instance(p_m.ui_screens_id)) : nullptr;
}

namespace {

const char *phase_name(net::MatchPhase p_phase) {
	switch (p_phase) {
		case net::MatchPhase::Loading:
			return "loading";
		case net::MatchPhase::PreRound:
			return "pre-round";
		case net::MatchPhase::InRound:
			return "in round";
		case net::MatchPhase::RoundOver:
			return "round over";
		case net::MatchPhase::Restarting:
			return "restarting";
	}
	return "?";
}

std::string config_path(const char *p_res) {
	const std::string path = to_std(ProjectSettings::get_singleton()->globalize_path(p_res));
	std::error_code ec;
	return std::filesystem::exists(utf8_path(path), ec) ? path : std::string();
}

void print_match(const net::MatchState &p_match) {
	char line[192];
	std::snprintf(line, sizeof(line), "[tdm] match: %s, US %d - RU %d of %d%s", phase_name(p_match.phase), p_match.kills[0], p_match.kills[1], p_match.kill_limit,
			p_match.winner == 1 ? ", US wins" : (p_match.winner == 2 ? ", RU wins" : ""));
	UtilityFunctions::print(line);
}

TdmArgText tdm_arg_text() {
	TdmArgText a;
	a.has_kill_limit = has_user_arg("kill-limit");
	a.kill_limit = user_arg_value("kill-limit");
	a.has_preround = has_user_arg("preround");
	a.preround = user_arg_value("preround");
	a.has_min_players = has_user_arg("min-players");
	a.min_players = user_arg_value("min-players");
	a.has_bots = has_user_arg("bots");
	a.bots = user_arg_value("bots");
	a.has_deploy_screen = has_user_arg("deploy-screen");
	a.deploy_screen = user_arg_value("deploy-screen");
	return a;
}

std::string limits_text(const TdmOptions &p_options) {
	return "kill limit " + (p_options.kill_limit > 0 ? std::to_string(p_options.kill_limit) : std::string("data (100)")) + ", pre-round " +
			(p_options.preround_s >= 0.0f ? std::to_string(p_options.preround_s) + " s" : std::string("data (10 s)")) + ", min players " +
			(p_options.min_players >= 0 ? std::to_string(p_options.min_players) : std::string("data"));
}

bool presented_position(const host::PresentationFrame &p_frame, uint64_t p_now_ns, uint8_t p_slot, sim::Vec3 &r_position) {
	if (!p_frame.valid || p_slot == 0xFF) {
		return false;
	}
	std::array<host::PresentedSoldier, net::kMaxSlots> others{};
	const size_t n = host::present_others(p_frame, p_now_ns, others);
	for (size_t i = 0; i < n; ++i) {
		if (others[i].slot == p_slot) {
			r_position = others[i].position;
			return true;
		}
	}
	return false;
}

const net::PlayerRow *player_row_of(const host::PresentationFrame &p_frame, uint8_t p_slot) {
	const size_t n = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	for (size_t i = 0; i < n; ++i) {
		if (p_frame.players[i].slot == p_slot) {
			return &p_frame.players[i];
		}
	}
	return nullptr;
}

void free_label(Label *&r_label) {
	if (r_label != nullptr) {
		r_label->queue_free();
		r_label = nullptr;
	}
}

Transform3D camera_transform(const fx::Transform &p_t) {
	Basis b;
	b.set_column(0, -Vector3(p_t.right.x, p_t.right.y, p_t.right.z));
	b.set_column(1, Vector3(p_t.up.x, p_t.up.y, p_t.up.z));
	b.set_column(2, -Vector3(p_t.forward.x, p_t.forward.y, p_t.forward.z));
	return Transform3D(b, Vector3(p_t.trans.x, p_t.trans.y, p_t.trans.z));
}

void keep_view(TdmState &r_m, const Transform3D &p_t) {
	for (int c = 0; c < 3; ++c) {
		const Vector3 col = p_t.basis.get_column(c);
		r_m.view_basis[c * 3 + 0] = col.x;
		r_m.view_basis[c * 3 + 1] = col.y;
		r_m.view_basis[c * 3 + 2] = col.z;
	}
	r_m.view_origin[0] = p_t.origin.x;
	r_m.view_origin[1] = p_t.origin.y;
	r_m.view_origin[2] = p_t.origin.z;
	r_m.view_valid = true;
}

Transform3D kept_view(const TdmState &p_m) {
	Basis b;
	for (int c = 0; c < 3; ++c) {
		b.set_column(c, Vector3(p_m.view_basis[c * 3 + 0], p_m.view_basis[c * 3 + 1], p_m.view_basis[c * 3 + 2]));
	}
	return Transform3D(b, Vector3(p_m.view_origin[0], p_m.view_origin[1], p_m.view_origin[2]));
}

void build_tdm_ui(GameState &r_game, TdmState &r_m, bool p_headless) {
	r_m.built = true;
	r_m.windowed = !p_headless;

	free_label(r_game.score_label);
	free_label(r_game.kit_label);
	if (r_m.options.deploy_screen) {
		free_label(r_game.death_label);
	}
	if (r_m.windowed && r_game.layer != nullptr) {
		r_m.tdm_hud = std::unique_ptr<Bf3TdmHud, void (*)(Bf3TdmHud *)>(new Bf3TdmHud(), &delete_part<Bf3TdmHud>);
		std::string error;
		Bf3Localisation *loc = r_game.localisation != nullptr ? r_game.localisation : Bf3Localisation::current();
		Bf3UiAssets *assets = r_game.ui_assets != nullptr ? r_game.ui_assets : Bf3UiAssets::current();
		if (!r_m.tdm_hud->build(r_game.layer, loc, assets, &error)) {
			UtilityFunctions::print(to_godot("[tdm] ERROR TDM HUD build: " + error));
			r_m.tdm_hud.reset();
			++r_m.ui_build_failures;
		} else {
			r_m.screen_records.add(r_m.tdm_hud.get());
		}
	}
	if (r_m.windowed && r_m.native_screens && r_game.layer != nullptr && r_game.native_hud != nullptr) {
		Bf3UiScreens *screens = memnew(Bf3UiScreens);
		screens->set_name("NativeScreens");
		screens->set_manual(true);
		r_game.layer->add_child(screens);
		screens->adopt(r_game.native_hud);
		r_m.screen_records.add(screens);
		r_m.ui_screens_id = screens->get_instance_id();
		r_m.native_flow =
				std::unique_ptr<NativeFlow, void (*)(NativeFlow *)>(new NativeFlow(), &delete_part<NativeFlow>);
		r_m.native_flow->set_keep_log(r_m.native_screens_check);
	}
	r_m.death_camera = std::unique_ptr<Bf3DeathCamera, void (*)(Bf3DeathCamera *)>(new Bf3DeathCamera(), &delete_part<Bf3DeathCamera>);
	if (r_game.crowd != nullptr && !r_game.crowd->built()) {
		r_game.crowd->set_corpse_seconds(r_m.corpse_seconds > 0.0f ? r_m.corpse_seconds : bf3::anim::kCrowdCorpseSeconds);
	}
}

void step_native_screens(GameState &r_game, TdmState &r_m, const host::PresentationFrame &p_f,
		const std::string &p_level, const std::string &p_mode, double p_delta) {
	Bf3UiScreens *screens = tdm_ui_screens(r_m);
	if (screens == nullptr || r_m.native_flow == nullptr) {
		return;
	}
	NativeFlow &flow = *r_m.native_flow;
	if (!flow.started()) {
		Bf3Localisation *loc = r_game.localisation != nullptr ? r_game.localisation : Bf3Localisation::current();
		if (loc != nullptr && loc->install() != nullptr) {
			flow.start(loc->install(), p_level, p_mode);
		}
	}
	if (!r_m.native_client_set && r_game.native_hud != nullptr && r_game.native_hud->is_ready() &&
			!r_game.native_hud->has_failed()) {
		screens->set_client(r_game.native_hud->client());
		r_m.native_client_set = true;
	}
	if (!r_m.native_following && r_m.native_client_set && flow.poll()) {
		screens->set_resolver(flow.resolver());
		screens->follow(flow.session());
		r_m.native_following = true;
		UtilityFunctions::print(to_godot("[ui] native screens follow the client UI flow of " + p_level + " " + p_mode +
				" (loaded in " + std::to_string(flow.load_seconds()) + " s)"));
	}
	if (flow.failed() && !r_m.native_failure_printed) {
		r_m.native_failure_printed = true;
		UtilityFunctions::print(to_godot("[ui] ERROR native screens: " + flow.error()));
	}
	screens->set_model(r_game.hud_model);
	host::UiGameState st;
	st.joined = p_f.match_valid && p_f.stats.connected && !p_f.stats.refused;
	st.phase = p_f.match_valid ? p_f.match.phase : net::MatchPhase::Loading;
	st.counting_down = st.phase == net::MatchPhase::PreRound && p_f.match.countdown_end_tick > p_f.newest_tick;
	st.own_deaths = r_m.own_deaths;
	st.own_spawns = r_m.own_spawns;
	st.winner = p_f.match.winner;
	st.unspawned = p_f.own_unspawned;
	if (r_m.native_following) {
		if (const bf3::core::Status s = flow.tick(st, p_delta); !s && !r_m.native_failure_printed) {
			r_m.native_failure_printed = true;
			UtilityFunctions::print(to_godot("[ui] ERROR native screens: " + s.error().message));
		}
		for (const bf3::core::ui::UiFlowAction &a : flow.take_actions()) {
			if (a.name == kUiFlowSpawnAction) {
				r_m.deploy_click = true;
			}
		}
	}
	screens->step(p_delta);
	if (!r_m.native_screens_check) {
		screens->clear_log();
	}
}

}

void Bf3LevelSession::tdm_ui_input(const Ref<InputEvent> &p_event) {
	if (!game_ || !game_->tdm) {
		return;
	}
	TdmState &m = *game_->tdm;
	Bf3UiScreens *screens = tdm_ui_screens(m);
	if (screens == nullptr || m.native_flow == nullptr || !m.native_following) {
		return;
	}
	const bf3::core::ui::UiInputRouter &router = m.native_flow->router();
	if (const InputEventKey *k = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (k->is_echo()) {
			return;
		}
		const Key code = k->get_physical_keycode() != KEY_NONE ? k->get_physical_keycode() : k->get_keycode();
		const std::string idk = ui_idk_of_key(code);
		if (idk.empty()) {
			return;
		}
		screens->key(bf3::core::ui::ui_flash_key_code(idk), k->is_pressed());
		for (const bf3::core::ui::UiInputConcept c : router.concepts_of_key(idk)) {
			screens->input_concept(int32_t(c), k->is_pressed());
		}
	} else if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		const MouseButton b = mb->get_button_index();
		if (b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_DOWN) {
			if (mb->is_pressed()) {
				screens->mouse_wheel(mb->get_position(),
						b == MOUSE_BUTTON_WHEEL_UP ? kUiWheelLinesPerNotch : -kUiWheelLinesPerNotch);
			}
			return;
		}
		screens->mouse_button(mb->get_position(), int32_t(b), mb->is_pressed());
		if (const std::string_view idb = ui_idb_of_button(b); !idb.empty()) {
			for (const bf3::core::ui::UiInputConcept c : router.concepts_of_mouse(idb)) {
				screens->input_concept(int32_t(c), mb->is_pressed());
			}
		}
	} else if (const InputEventMouseMotion *mm = Object::cast_to<InputEventMouseMotion>(p_event.ptr())) {
		screens->mouse_move(mm->get_position());
	}
}

void Bf3LevelSession::tdm_parse_args(std::string &r_error) {
	GameState &g = *game_;
	const bool scene_tdm = has_meta("openbf3_tdm") && bool(get_meta("openbf3_tdm"));
	const bool check = has_check_arg("m2b-check");
	const bool preview = !scene_tdm && !check && has_user_arg("tdm");
	g.tdm_preview = TdmPreview();
	if (!scene_tdm && !check && !preview) {
		return;
	}
	TdmArgText a = tdm_arg_text();
	std::string error;
	if (preview) {
		a.has_bots = false;
		a.has_deploy_screen = false;
		TdmPreview &p = g.tdm_preview;
		p.on = true;
		p.options.deploy_screen = false;
		if (const bf3::core::Status s = parse_tdm_options(a, p.options); !s.ok()) {
			error = s.error().message;
		}
		p.options.bot_tuning_cfg = config_path("res://config/bot_tuning.cfg");
		UtilityFunctions::print(to_godot("[tdm] --tdm preview: TDM rules on (no deploy screen, KitSelect kit), TDM bots, " + limits_text(p.options) + ", tuning " +
				(p.options.bot_tuning_cfg.empty() ? std::string("built-in defaults") : p.options.bot_tuning_cfg)));
	} else {
		g.tdm = std::unique_ptr<TdmState, void (*)(TdmState *)>(new TdmState(), &delete_part<TdmState>);
		TdmState &m = *g.tdm;
		m.enabled = true;
		m.playable = !check;
		if (check) {
			bf3::core::Result<std::vector<M2bCase>> cases = parse_m2b_cases(user_arg_value("m2b-check"));
			if (!cases) {
				error = cases.error().message;
			} else {
				m.cases = std::move(*cases);
			}
		}
		if (const bf3::core::Status s = parse_tdm_options(a, m.options); !s.ok() && error.empty()) {
			error = s.error().message;
		}
		bots_ = m.options.bots;
		m.native_screens_check = has_user_arg("native-screens-check");
		m.native_screens = m.native_screens_check || user_arg_value("native-screens") != "0";
		if (m.native_screens_check && quit_after_ <= 0.0) {
			quit_after_ = 1.0e9;
		}
		m.options.tdm_tuning_cfg = config_path("res://config/tdm_tuning.cfg");
		m.options.bot_tuning_cfg = config_path("res://config/bot_tuning.cfg");
		m.check_options.shots_dir = utf8_path(user_arg_value("m2b-shots"));
		if (has_user_arg("m2b-play-seconds")) {
			const double s = std::atof(user_arg_value("m2b-play-seconds").c_str());
			if (!(s >= 5.0 && s <= 3600.0)) {
				error = error.empty() ? "--m2b-play-seconds=" + user_arg_value("m2b-play-seconds") + ": not 5..3600" : error;
			}
			m.check_options.play_seconds = std::clamp(std::isfinite(s) ? s : 90.0, 5.0, 3600.0);
		}
		m.check_options.case_timeout_s = std::max(m.check_options.case_timeout_s, m.check_options.play_seconds + 90.0);
		if (m.checks() && quit_after_ <= 0.0) {
			quit_after_ = 1.0e9;
		}
		mapper_.enable_tdm(true);
		UtilityFunctions::print(to_godot(std::string("[tdm] ") + (m.playable ? "playable scene" : "check run") + ": TDM rules on, " +
				(m.options.deploy_screen ? "spawn screen (Enter deploys, 1-4 kit, 3 defibrillator, Tab scoreboard)" : "no spawn screen (automatic respawn, K kit)") +
				", " + std::to_string(m.options.bots) + " TDM bots, " + limits_text(m.options) + ", tuning " +
				(m.options.tdm_tuning_cfg.empty() ? "built-in" : "config/tdm_tuning.cfg") + " / " +
				(m.options.bot_tuning_cfg.empty() ? "built-in" : "config/bot_tuning.cfg") + (m.checks() ? ", case play" : "")));
	}
	if (!error.empty()) {
		UtilityFunctions::print(to_godot("[tdm] bad arguments: " + error));
		if (r_error.empty()) {
			r_error = error;
		}
	}
}

void Bf3LevelSession::tdm_load_options(host::SharedLoadOptions &r_options) {
	const GameState &g = *game_;
	if (g.tdm_preview.on) {
		r_options.tdm = true;
	}
	if (g.tdm) {
		r_options.tdm = true;
		r_options.tdm_tuning_cfg = g.tdm->options.tdm_tuning_cfg;
	}
}

void Bf3LevelSession::tdm_server_config(host::GameServerConfig &r_config) {
	GameState &g = *game_;
	const TdmOptions *options = g.tdm ? &g.tdm->options : (g.tdm_preview.on ? &g.tdm_preview.options : nullptr);
	if (options == nullptr) {
		return;
	}
	r_config.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	r_config.rules.deploy_screen = options->deploy_screen;
	r_config.rules.revive = true;
	r_config.rules.kill_limit_override = options->kill_limit;
	r_config.rules.preround_override_s = options->preround_s;
	r_config.rules.min_players_override = options->min_players;
	r_config.rules.max_players = 32;
	r_config.firefight = nullptr;
	r_config.tdm_bots = nullptr;
	if (bots_ > 0) {
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = std::min<uint32_t>(bots_, kTdmMaxBots);
		bots->tuning_cfg = options->bot_tuning_cfg;
		r_config.tdm_bots = bots;
	}
	if (!g.tdm) {
		g.part_notes.push_back("[tdm] --tdm preview: " + std::to_string(r_config.tdm_bots != nullptr ? r_config.tdm_bots->count : 0u) +
				" TDM bots, teams by AutoTeam, the round flow from the Metro TDM logic graph");
		return;
	}
	TdmState &m = *g.tdm;
	g.firefight_stats = nullptr;
	g.firefight_bots = 0;
	if (g.world == nullptr && r_config.level != nullptr && r_config.level->static_world != nullptr) {
		bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(r_config.level->static_world);
		if (world) {
			g.world = std::move(*world);
		}
	}
	if (r_config.level != nullptr && r_config.level->gunplay != nullptr && r_config.level->gunplay->soldier.time_for_corpse > 0.0f) {
		m.corpse_seconds = r_config.level->gunplay->soldier.time_for_corpse;
	}
	if (g.part_notes.size() < 64) {
		g.part_notes.push_back("[tdm] " + std::to_string(r_config.tdm_bots != nullptr ? r_config.tdm_bots->count : 0u) +
				" TDM bots, teams by AutoTeam, the round from the Metro TDM logic graph, " +
				(m.options.deploy_screen ? "the spawn screen" : "automatic respawn") + ", max slots " + std::to_string(r_config.max_slots));
	}
}

void Bf3LevelSession::tdm_presented(uint64_t p_now_ns) {
	GameState &r_game = *game_;
	if (!r_game.tdm) {
		return;
	}
	TdmState &m = *r_game.tdm;
	const host::PresentationFrame &f = *frame_;
	InputMapper &r_mapper = mapper_;
	const double now_s = double(p_now_ns) * 1e-9;

	m.own_slot = f.stats.connected || f.match_valid ? f.stats.slot : uint8_t(0xFF);
	const size_t rows = std::min<size_t>(f.player_count, f.players.size());
	for (size_t i = 0; i < rows; ++i) {
		const net::PlayerRow &row = f.players[i];
		if (row.team == 1 || row.team == 2) {
			r_game.slot_team[row.slot] = row.team;
		}
	}
	const net::PlayerRow *own_row = m.own_slot != 0xFF ? player_row_of(f, m.own_slot) : nullptr;
	m.own_team = f.own_team == 1 || f.own_team == 2 ? f.own_team : (own_row != nullptr && (own_row->team == 1 || own_row->team == 2) ? own_row->team : uint8_t(0));
	if (!r_game.spawned && m.own_team != 0) {
		r_game.own_team = m.own_team;
	}

	bool own_revive = false;
	for (const host::PresentEvent &e : r_game.events) {
		if (e.kind == host::PresentKind::Death) {
			++m.deaths_received;
			if ((e.flags & host::present_flags::kOwnVictim) != 0) {
				++m.own_deaths;
				m.own_death_s = now_s;
				m.own_killer = e.slot == e.aux ? uint8_t(0xFF) : e.slot;
				m.kill_card_source = host::HudKillCardSource();
				sim::Vec3 killer{};
				if (m.own_killer != 0xFF && presented_position(f, p_now_ns, m.own_killer, killer)) {
					const sim::Vec3 &me = f.own_curr.position;
					m.kill_card_source.killer_seen = true;
					m.kill_card_source.distance_m = std::sqrt((killer[0] - me[0]) * (killer[0] - me[0]) + (killer[1] - me[1]) * (killer[1] - me[1]) +
							(killer[2] - me[2]) * (killer[2] - me[2]));
				}
				m.death_view_pending = true;
			}
		} else if (e.kind == host::PresentKind::Revive && e.aux == m.own_slot && m.own_slot != 0xFF) {
			own_revive = true;
		}
	}
	if (own_revive) {
		m.revive_seen_s = now_s;
	}

	if (r_game.alive && (f.own_curr.life != m.own_life_seen || !m.was_alive)) {
		m.own_life_seen = f.own_curr.life;
		m.spawned_this_round = true;
		++m.own_spawns;
		m.life_revived = m.revive_seen_s >= 0.0 && now_s - m.revive_seen_s < 2.0;
		if (m.deploy.armed && !m.life_revived) {
			++m.deploys;
			m.kit_check_kit = m.deploy.kit;
			m.kit_check_until_s = now_s + 1.0;
		}
		tdm_deploy_spawned(m.deploy);
		m.own_death_s = -1.0;
		m.view_valid = false;
		m.death_view_pending = false;
		m.death_view_end = true;
	}
	m.was_alive = r_game.alive;
	if (m.kit_check_kit != 0xFF && r_game.alive) {
		if (f.own_kit == m.kit_check_kit) {
			m.kit_check_kit = 0xFF;
		} else if (now_s >= m.kit_check_until_s) {
			++m.deploy_kit_mismatch;
			UtilityFunctions::print(to_godot(std::string("[tdm] deployed kit ") + host::kit_role_name(host::KitRole(std::min<uint8_t>(f.own_kit, 3))) + ", requested " +
					host::kit_role_name(host::KitRole(std::min<uint8_t>(m.kit_check_kit, 3)))));
			m.kit_check_kit = 0xFF;
		}
	}

	if (f.match_valid) {
		const bool over = f.match.phase == net::MatchPhase::RoundOver || f.match.phase == net::MatchPhase::Restarting;
		if ((m.last_phase == net::MatchPhase::RoundOver || m.last_phase == net::MatchPhase::Restarting) && !over) {
			m.spawned_this_round = false;
			m.own_death_s = -1.0;
		}
		if (f.match.phase != m.last_phase) {
			print_match(f.match);
		}
		m.last_phase = f.match.phase;
		m.round_end = over;
	}
	if (!m.options.deploy_screen) {
		m.spawn_screen = false;
		m.can_deploy = false;
		r_mapper.set_spawn_screen(false);
		return;
	}

	TdmScreenInput si;
	si.match_valid = f.match_valid;
	si.phase = f.match.phase;
	si.own_alive = r_game.alive;
	si.own_unspawned = f.own_unspawned;
	si.kill_card_up = m.own_death_s >= 0.0 && now_s - m.own_death_s < host::kDeathScreenSeconds;
	m.spawn_screen = tdm_spawn_screen_up(si);
	r_mapper.set_spawn_screen(m.spawn_screen);
	m.can_deploy = tdm_can_deploy(f.match_valid, f.match, f.newest_tick, f.own_respawn_tick);
	if (const int k = r_mapper.take_kit(); k >= 0 && k < int(host::kKitCount)) {
		m.spawn_kit = uint8_t(k);
	}
	if (const uint32_t k = r_mapper.take_kit_presses(); k > 0) {
		m.spawn_kit = uint8_t((m.spawn_kit + k) % host::kKitCount);
	}
	if (r_mapper.take_team_presses() > 0) {
		UtilityFunctions::print("[tdm] T: the teams are AutoTeam's in TDM");
	}
	if (r_mapper.take_deploy_presses() > 0 || m.deploy_click) {
		if (m.spawn_screen && m.can_deploy) {
			m.deploy.armed = true;
		}
		m.deploy_click = false;
	}
	if (m.deploy.armed) {
		m.deploy.kit = m.spawn_kit;
	}
	if (tdm_deploy_step(m.deploy, m.spawn_screen, m.can_deploy, f.newest_tick)) {
		client_->send_spawn_request(m.deploy.kit);
		++m.spawn_requests_sent;
		m.last_request_tick = f.newest_tick;
		if (m.deploy.resends == 0) {
			UtilityFunctions::print(to_godot(std::string("[tdm] deploy: ") + host::kit_role_name(host::KitRole(m.deploy.kit)) + " (server tick " +
					std::to_string(f.newest_tick) + ")"));
		}
	}
}

bool Bf3LevelSession::tdm_frame(uint64_t p_now_ns, [[maybe_unused]] double p_delta, Camera3D *p_camera, bool p_free_camera) {
	GameState &r_game = *game_;
	const host::PresentationFrame &f = *frame_;
	if (TdmPreview &p = r_game.tdm_preview; p.on && f.match_valid) {
		const net::MatchState &l = p.last;
		if (!p.have_match || f.match.phase != l.phase || f.match.kills != l.kills || f.match.kill_limit != l.kill_limit || f.match.winner != l.winner) {
			p.have_match = true;
			p.last = f.match;
			print_match(f.match);
		}
	}
	if (!r_game.tdm) {
		return false;
	}
	TdmState &m = *r_game.tdm;
	const double now_s = double(p_now_ns) * 1e-9;
	using bf3::core::ms_since;
	if (!m.built) {
		build_tdm_ui(r_game, m, headless_);
	}
	InputMapper &mapper = mapper_;
	m.scoreboard_held = tdm_scoreboard_shown(mapper.scoreboard_held(), f.match_valid, f.match.phase);

	auto t0 = std::chrono::steady_clock::now();
	{
		host::HudFeedInput in;
		in.weapon = r_game.weapon;
		in.alive = r_game.alive;
		in.health = f.own_curr.health;
		in.pose = f.own_curr.pose;
		in.now_s = now_s;
		in.tdm = true;
		in.match_valid = f.match_valid;
		in.match = f.match;
		in.players = std::span<const net::PlayerRow>(f.players.data(), std::min<size_t>(f.player_count, f.players.size()));
		in.own_slot = m.own_slot;
		in.own_team = m.own_team;
		in.server_tick = f.newest_tick;
		in.own_respawn_tick = f.own_respawn_tick;
		in.own_revive_until = f.own_revive_until;
		in.own_unspawned = f.own_unspawned;
		in.spawned_this_round = m.spawned_this_round;
		in.spawn_kit = m.spawn_kit;
		in.scoreboard_held = m.scoreboard_held;
		in.kits = r_game.gunplay != nullptr ? &r_game.gunplay->kits : nullptr;
		in.kill_card_source = m.kill_card_source;
		const std::shared_ptr<const host::SharedLevel> level = shared_level();
		if (level != nullptr && level->tdm_tuning != nullptr) {
			in.score_kill = level->tdm_tuning->score_kill;
			in.score_headshot = level->tdm_tuning->score_headshot;
		}
		in.eor = level != nullptr ? level->eor.get() : nullptr;
		in.loadouts = level != nullptr && level->gunplay != nullptr ? &level->gunplay->loadouts : nullptr;
		in.loadout_choices = m.loadout_choices;
		in.game_mode = level != nullptr ? std::string_view(level->mode) : std::string_view();
		in.level_name_sid_hash = level != nullptr && !level->level_name_sid.empty()
				? bf3::core::ui::text_hash(level->level_name_sid)
				: 0u;
		host::fill_hud_tdm(in, r_game.events, r_game.hud_static, m.tdm_state, r_game.hud_model);
		if (!m.options.deploy_screen) {
			r_game.hud_model.spawn_screen.visible = false;
		}
		if (r_game.native_hud != nullptr) {
			r_game.native_hud->set_model(r_game.hud_model);
		}
		if (m.tdm_hud != nullptr && m.tdm_hud->built()) {
			m.tdm_hud->set_model(r_game.hud_model);
			if (const int k = m.tdm_hud->take_kit_click(); k >= 0 && k < int(host::kKitCount)) {
				m.spawn_kit = uint8_t(k);
			}
			if (m.tdm_hud->take_deploy_click()) {
				m.deploy_click = true;
			}
		}
	}
	m.kill_card_source = host::HudKillCardSource();
	step_native_screens(r_game, m, f, level_id(), mode_id(), p_delta);
	m.ms_tdm_hud = ms_since(t0);
#if OPENBF3_BUILD_CHECKS
	Bf3UiScreens *screens = tdm_ui_screens(m);
	if (m.native_screens_check && screens != nullptr && m.native_flow != nullptr && !m.finished) {
		if (m.native_check == nullptr) {
			m.native_check = std::unique_ptr<NativeScreensCheck, void (*)(NativeScreensCheck *)>(
					new NativeScreensCheck(), &delete_part<NativeScreensCheck>);
		}
		const auto press = [this](Key p_key, bool p_pressed) {
			Ref<InputEventKey> k;
			k.instantiate();
			k->set_keycode(p_key);
			k->set_physical_keycode(p_key);
			k->set_pressed(p_pressed);
			tdm_ui_input(k);
		};
		if (m.native_check->frame(*screens, *m.native_flow, now_s, press)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.native_check->report()));
			finish(m.native_check->exit_code());
			return true;
		}
	}
#endif

	t0 = std::chrono::steady_clock::now();
	const bool dead = !r_game.alive && m.spawned_this_round;
	if (m.death_camera != nullptr) {
		if (m.death_view_end) {
			m.death_camera->end();
		}
		if (m.death_view_pending) {
			DeathCameraStart st;

			const sim::Vec3 &p = f.own_curr.position;
			st.corpse = fx::Vec3{ p[0] + f.visual_offset[0], p[1] + f.visual_offset[1], p[2] + f.visual_offset[2] };
			st.yaw_rad = sim::yaw_radians(f.own_curr.yaw);
			const Vector3 eye = cameras_.last_eye();
			st.eye = fx::Vec3{ eye.x, eye.y, eye.z };
			st.killer_slot = m.own_killer == m.own_slot ? uint8_t(0xFF) : m.own_killer;
			st.killer_camera = !f.match_valid || f.match.killer_camera;
			m.death_camera->begin(st, now_s);
		}
		if (m.death_camera->active()) {
			sim::Vec3 k{};
			fx::Vec3 killer{};
			const bool seen = presented_position(f, p_now_ns, m.own_killer, k);
			if (seen) {
				killer = k;
			}
			const DeathCameraView v = m.death_camera->step(now_s, seen ? &killer : nullptr, r_game.world.get());
			if (v.active && p_camera != nullptr) {
				const Transform3D t = camera_transform(v.camera);
				keep_view(m, t);
			}
		}
	}
	m.death_view_pending = false;
	m.death_view_end = false;
	if (p_camera != nullptr && !p_free_camera) {
		if (dead && m.view_valid) {
			p_camera->set_global_transform(kept_view(m));
		} else if (!m.spawned_this_round && !f.valid && !m.join_view_placed) {

			const std::shared_ptr<const host::SharedLevel> shared = shared_level();
			if (shared != nullptr && !shared->spawns.empty()) {
				const sim::SpawnPoint &sp = shared->spawns.front();
				const float yaw = sim::yaw_radians(sp.yaw);
				const Basis b = Basis(Vector3(0, 1, 0), yaw + float(sim::kPi)) * Basis(Vector3(1, 0, 0), -0.25f);
				p_camera->set_global_transform(Transform3D(b, Vector3(sp.position[0], sp.position[1] + 3.0f, sp.position[2])));
				m.join_view_placed = true;
			}
		}
	}
	if (r_game.alive) {
		m.join_view_placed = false;
	}
	m.ms_death_camera = ms_since(t0);

	bool wants_mouse = m.spawn_screen;
	if (m.tdm_hud != nullptr && m.tdm_hud->built()) {
		wants_mouse = wants_mouse || m.tdm_hud->wants_mouse();
	}
	if (!headless_ && !mapper.scripted()) {
		if (wants_mouse && !m.mouse_released) {
			mapper.set_capture(false);
			m.mouse_released = true;
		} else if (!wants_mouse && m.mouse_released) {
			m.mouse_released = false;
			if (r_game.alive) {
				mapper.set_capture(true);
			}
		}
	}
	m.frame.game = nullptr;
	m.frame.match_valid = f.match_valid;
	m.frame.match = f.match_valid ? &f.match : nullptr;
	m.frame.players = std::span<const net::PlayerRow>(f.players.data(), std::min<size_t>(f.player_count, f.players.size()));
	m.frame.server_tick = f.newest_tick;
	m.frame.now_s = now_s;
	m.frame.own_alive = r_game.alive;
	m.frame.own_unspawned = f.own_unspawned;
	m.frame.spawn_screen = m.spawn_screen;
	m.frame.death_view = m.view_valid && dead;

#if OPENBF3_BUILD_CHECKS
	if (m.checks() && m.check == nullptr && !m.finished && r_game.hud_ready()) {
		m.check = std::unique_ptr<M2bCheck, void (*)(M2bCheck *)>(new M2bCheck(*this, r_game, m, m.cases, m.check_options), &delete_part<M2bCheck>);
	}
	if (m.check != nullptr && !m.finished) {
		if (m.check->frame(p_now_ns, p_delta)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.check->report()));
			finish(m.check->exit_code());
			return true;
		}
	}
#endif
	return false;
}

void Bf3LevelSession::tdm_shutdown() {
	if (!game_->tdm) {
		return;
	}
	TdmState &m = *game_->tdm;
	m.check.reset();
	if (Bf3UiScreens *screens = tdm_ui_screens(m)) {
		m.screen_records.remove(screens);
		screens->follow(nullptr);
		screens->set_resolver(nullptr);
		screens->adopt(nullptr);
	}
	m.ui_screens_id = 0;
	m.native_check.reset();
	m.native_flow.reset();
	if (m.tdm_hud != nullptr) {
		m.screen_records.remove(m.tdm_hud.get());
		m.tdm_hud->shutdown();
	}
	m.death_camera.reset();
}

const Bf3UiScreens *Bf3LevelSession::screen_stack() const {
	return game_ && game_->tdm ? tdm_ui_screens(*game_->tdm) : nullptr;
}

const UiScreenRecords *Bf3LevelSession::screen_records() const {
	return game_ && game_->tdm ? &game_->tdm->screen_records : nullptr;
}

bool Bf3LevelSession::tdm_join_ready() const {
	if (!game_ || !game_->tdm || !game_->tdm->enabled || !game_->tdm->options.deploy_screen || frame_ == nullptr) {
		return false;
	}

	const host::PresentationFrame &f = *frame_;
	return f.match_valid && f.stats.connected && !f.stats.refused;
}

}
