// The M2b session (docs/design/m2b.md §4, §11 gx row; as built: docs/design/m2b-gx.md; owner gx). See
// level_session_m2b.h: the M2b session is the M2a session (level_session_m2a.cpp) with the TDM rules on. Its hooks are
// called from one tagged line `// M2b gx [<tag>]` each in the M2a hooks, and every one returns at once outside an M2b
// session (M2aGame::m2b null), so the M2a scene, M1, S1 and S3 run exactly as before.
//
// ui's and cam's parts (Bf3TdmHud, fill_hud_tdm, Bf3DeathCamera, the crowd's corpse time) are built only when their
// body is in the tree (OPENBF3_M2B_HAVE_UI / _CAM, the "# M2b W2.0" block of bf3_godot's CMake): this branch links
// before they land and picks them up after the merge without an edit. Without ui a plain status label stands in for
// the spawn screen, so the scene stays playable (Enter deploys, 1-4 pick the kit).

#include "level_session_m2b.h"

#include "level_session_m2a.h"

#include "bf3_localisation.h"
#include "fp_camera.h"
#include "game_crowd.h"
#include "godot_util.h"
#include "hud_controls.h"
#include "input_mapper.h"
#include "ui_assets.h"

#include "bf3/host/game_server.h"
#include "bf3/host/kits.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#ifndef OPENBF3_M2B_HAVE_UI
#define OPENBF3_M2B_HAVE_UI 0
#endif
#ifndef OPENBF3_M2B_HAVE_CAM
#define OPENBF3_M2B_HAVE_CAM 0
#endif
#ifndef OPENBF3_M2A_HAVE_CROWD
#define OPENBF3_M2A_HAVE_CROWD 0
#endif

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace net = bf3::net;
namespace sim = bf3::sim;

namespace {

template <typename T>
void delete_m2b_part(T *p_part) {
	delete p_part;
}

std::filesystem::path m2b_utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

const char *m2b_team_name(uint8_t p_team) {
	return p_team == 1 ? "US" : (p_team == 2 ? "RU" : "?");
}

const char *m2b_phase_text(net::MatchPhase p_phase) {
	switch (p_phase) {
		case net::MatchPhase::Loading:
			return "loading";
		case net::MatchPhase::PreRound:
			return "pre-round";
		case net::MatchPhase::InRound:
			return "in round";
		case net::MatchPhase::RoundOver:
			return "round over";
	}
	return "?";
}

// A config file next to the project (res://config/<name>) as an OS path, or "" when it is not there (main thread).
std::string m2b_config_path(const char *p_res) {
	const std::string path = to_std(ProjectSettings::get_singleton()->globalize_path(p_res));
	std::error_code ec;
	return std::filesystem::exists(m2b_utf8_path(path), ec) ? path : std::string();
}

// The presented position of a slot this frame (false when it is not drawn).
bool m2b_presented_position(const host::PresentationFrame &p_frame, uint64_t p_now_ns, uint8_t p_slot, sim::Vec3 &r_position) {
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

const net::PlayerRow *m2b_row_of(const host::PresentationFrame &p_frame, uint8_t p_slot) {
	const size_t n = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	for (size_t i = 0; i < n; ++i) {
		if (p_frame.players[i].slot == p_slot) {
			return &p_frame.players[i];
		}
	}
	return nullptr;
}

Label *m2b_make_label(int p_size) {
	Label *l = memnew(Label);
	l->add_theme_color_override("font_color", Color(1, 1, 1));
	l->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	l->add_theme_constant_override("shadow_offset_x", 1);
	l->add_theme_constant_override("shadow_offset_y", 1);
	l->add_theme_font_size_override("font_size", p_size);
	l->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	l->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	l->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	return l;
}

void m2b_free_label(Label *&r_label) {
	if (r_label != nullptr) {
		r_label->queue_free();
		r_label = nullptr;
	}
}

// Godot's camera basis from a Frostbite-axes frame (right, up, forward = the view direction; a Camera3D looks down -Z).
Transform3D m2b_camera_transform(const fx::Transform &p_t) {
	Basis b;
	b.set_column(0, Vector3(p_t.right.x, p_t.right.y, p_t.right.z));
	b.set_column(1, Vector3(p_t.up.x, p_t.up.y, p_t.up.z));
	b.set_column(2, -Vector3(p_t.forward.x, p_t.forward.y, p_t.forward.z));
	return Transform3D(b, Vector3(p_t.trans.x, p_t.trans.y, p_t.trans.z));
}

void m2b_keep_view(M2bGame &r_m, const Transform3D &p_t) {
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

Transform3D m2b_kept_view(const M2bGame &p_m) {
	Basis b;
	for (int c = 0; c < 3; ++c) {
		b.set_column(c, Vector3(p_m.view_basis[c * 3 + 0], p_m.view_basis[c * 3 + 1], p_m.view_basis[c * 3 + 2]));
	}
	return Transform3D(b, Vector3(p_m.view_origin[0], p_m.view_origin[1], p_m.view_origin[2]));
}

// The Godot side, once, on the first frame after Done.
void m2b_build(M2aGame &r_game, M2bGame &r_m, bool p_headless) {
	r_m.built = true;
	r_m.windowed = !p_headless;
	// The M2a labels count kills from the client's own events and say "K kit, T team": the M2b scene shows the server's
	// MatchState instead (ui's screens, or the status label below). With the deploy screen off (M2a's automatic
	// respawn) the death label's respawn countdown stays.
	m2b_free_label(r_game.score_label);
	m2b_free_label(r_game.kit_label);
	if (r_m.options.deploy_screen) {
		m2b_free_label(r_game.death_label);
	}
#if OPENBF3_M2B_HAVE_UI
	if (r_m.windowed && r_game.layer != nullptr) {
		r_m.tdm_hud = std::unique_ptr<Bf3TdmHud, void (*)(Bf3TdmHud *)>(new Bf3TdmHud(), &delete_m2b_part<Bf3TdmHud>);
		std::string error;
		Bf3Localisation *loc = r_game.localisation != nullptr ? r_game.localisation : Bf3Localisation::current();
		Bf3UiAssets *assets = r_game.ui_assets != nullptr ? r_game.ui_assets : Bf3UiAssets::current();
		if (!r_m.tdm_hud->build(r_game.layer, loc, assets, &error)) {
			UtilityFunctions::print(to_godot("[m2b] ERROR TDM HUD build: " + error));
			r_m.tdm_hud.reset();
			++r_m.ui_build_failures;
		}
	}
#else
	UtilityFunctions::print("[m2b] TDM HUD skipped: waiting for ui (a status label stands in for the spawn screen)");
#endif
#if !OPENBF3_M2B_HAVE_UI
	if (r_game.layer != nullptr) {
		r_m.status_label = m2b_make_label(22);
		r_m.status_label->set_name("M2bStatus");
		r_game.layer->add_child(r_m.status_label);
	}
#endif
#if OPENBF3_M2B_HAVE_CAM
	r_m.death_camera = std::unique_ptr<Bf3DeathCamera, void (*)(Bf3DeathCamera *)>(new Bf3DeathCamera(), &delete_m2b_part<Bf3DeathCamera>);
#if OPENBF3_M2A_HAVE_CROWD
	// The 3p corpse stays for SoldierDef::time_for_corpse (15 s on Metro), set before the crowd's build (m2a_step_parts
	// builds it later in this frame).
	if (r_game.crowd != nullptr && !r_game.crowd->built()) {
		r_game.crowd->set_corpse_seconds(r_m.corpse_seconds > 0.0f ? r_m.corpse_seconds : bf3::anim::kCrowdCorpseSeconds);
	}
#endif
#else
	UtilityFunctions::print("[m2b] death view skipped: waiting for cam (the camera stays on the corpse)");
#endif
}

// The status label (ui not landed): the round, the counts, and the spawn screen's choice.
void m2b_status_text(const M2bGame &p_m, const host::PresentationFrame &p_frame, bool p_alive, Viewport *p_viewport) {
	if (p_m.status_label == nullptr) {
		return;
	}
	const Vector2 vp = p_viewport != nullptr ? p_viewport->get_visible_rect().size : Vector2(1280, 720);
	std::string text;
	char line[256];
	if (p_frame.match_valid) {
		const net::MatchState &ms = p_frame.match;
		std::snprintf(line, sizeof(line), "%s   US %d : RU %d of %d", m2b_phase_text(ms.phase), ms.kills[0], ms.kills[1], ms.kill_limit);
		text = line;
		if (ms.phase == net::MatchPhase::PreRound && ms.countdown_end_tick > p_frame.newest_tick) {
			std::snprintf(line, sizeof(line), "   round starts in %.0f s", double(ms.countdown_end_tick - p_frame.newest_tick) / double(sim::kSimHz));
			text += line;
		}
		if (ms.phase == net::MatchPhase::RoundOver) {
			text += ms.winner == 1 ? "\nUS WINS" : (ms.winner == 2 ? "\nRU WINS" : "\nDRAW");
		}
	} else {
		text = "waiting for the match";
	}
	if (p_m.spawn_screen) {
		std::snprintf(line, sizeof(line), "\n\nSPAWN   kit: %s (1-4)   team %s", host::kit_role_name(host::KitRole(std::min<uint8_t>(p_m.spawn_kit, host::kKitCount - 1))),
				m2b_team_name(p_m.own_team));
		text += line;
		if (p_m.can_deploy) {
			text += p_m.deploy.armed ? "\ndeploying..." : "\nENTER: deploy";
		} else if (p_frame.own_respawn_tick > p_frame.newest_tick) {
			std::snprintf(line, sizeof(line), "\ndeploy in %.1f s", double(p_frame.own_respawn_tick - p_frame.newest_tick) / double(sim::kSimHz));
			text += line;
		}
		if (p_frame.own_revive_until > p_frame.newest_tick && p_m.spawned_this_round) {
			text += "   revive possible";
		}
	} else if (!p_alive && p_m.own_death_s >= 0.0 && p_m.own_killer != 0xFF) {
		std::snprintf(line, sizeof(line), "\n\nKILLED BY slot %u (%s)", unsigned(p_m.own_killer), p_m.own_killer == p_m.own_slot ? "self" : "enemy");
		text += line;
	}
	p_m.status_label->set_text(to_godot(text));
	p_m.status_label->set_position(Vector2(0, vp.y * 0.12f));
	p_m.status_label->set_size(Vector2(vp.x, vp.y * 0.5f));
}

} // namespace

// ---- [args] ---------------------------------------------------------------------------------------------------------

void m2b_gx_args(M2aGame &r_game, bool p_scene_m2b, InputMapper &r_mapper, uint32_t &r_bots, double &r_quit_after, std::string &r_error) {
	const bool check = has_user_arg("m2b-check");
	if (!p_scene_m2b && !check) {
		return;
	}
	r_game.m2b = std::unique_ptr<M2bGame, void (*)(M2bGame *)>(new M2bGame(), &delete_m2b_part<M2bGame>);
	M2bGame &m = *r_game.m2b;
	m.enabled = true;
	m.playable = !check;
	std::string error;
	if (check) {
		bf3::core::Result<std::vector<M2bCase>> cases = parse_m2b_cases(user_arg_value("m2b-check"));
		if (!cases) {
			error = cases.error().message;
		} else {
			m.cases = std::move(*cases);
		}
	}
	M2bArgText a;
	a.has_kill_limit = has_user_arg("kill-limit");
	a.kill_limit = user_arg_value("kill-limit");
	a.has_preround = has_user_arg("preround");
	a.preround = user_arg_value("preround");
	a.has_bots = has_user_arg("bots");
	a.bots = user_arg_value("bots");
	a.has_deploy_screen = has_user_arg("deploy-screen");
	a.deploy_screen = user_arg_value("deploy-screen");
	if (const bf3::core::Status s = parse_m2b_options(a, m.options); !s.ok() && error.empty()) {
		error = s.error().message;
	}
	r_bots = m.options.bots; // M2a's default (24 Free bots) and its 32 cap do not apply: 31 TDM bots by default
	m.options.tdm_tuning_cfg = m2b_config_path("res://config/tdm_tuning.cfg");
	m.options.bot_tuning_cfg = m2b_config_path("res://config/bot_tuning.cfg");
	m.check_options.shots_dir = m2b_utf8_path(user_arg_value("m2b-shots"));
	if (has_user_arg("m2b-play-seconds")) {
		const double s = std::atof(user_arg_value("m2b-play-seconds").c_str());
		if (!(s >= 5.0 && s <= 3600.0)) {
			error = error.empty() ? "--m2b-play-seconds=" + user_arg_value("m2b-play-seconds") + ": not 5..3600" : error;
		}
		m.check_options.play_seconds = std::clamp(std::isfinite(s) ? s : 90.0, 5.0, 3600.0);
	}
	m.check_options.case_timeout_s = std::max(m.check_options.case_timeout_s, m.check_options.play_seconds + 90.0);
	if (m.checks() && r_quit_after <= 0.0) {
		r_quit_after = 1.0e9; // a load failure quits a check run with 1; the check itself ends the run
	}
	r_mapper.m2b_enable(true);
	char line[512];
	std::snprintf(line, sizeof(line), "[m2b] %s: TDM rules on, %s, %u TDM bots, kill limit %s, pre-round %s, tuning %s / %s%s", m.playable ? "playable scene" : "check run",
			m.options.deploy_screen ? "spawn screen (Enter deploys, 1-4 kit, 3 defibrillator, Tab scoreboard)" : "no spawn screen (automatic respawn, K kit)",
			m.options.bots, m.options.kill_limit > 0 ? std::to_string(m.options.kill_limit).c_str() : "data (100)",
			m.options.preround_s >= 0.0f ? (std::to_string(m.options.preround_s) + " s").c_str() : "data (10 s)",
			m.options.tdm_tuning_cfg.empty() ? "built-in" : "config/tdm_tuning.cfg", m.options.bot_tuning_cfg.empty() ? "built-in" : "config/bot_tuning.cfg",
			m.checks() ? ", case play" : "");
	UtilityFunctions::print(line);
	if (!error.empty()) {
		UtilityFunctions::print(to_godot("[m2b] bad arguments: " + error));
		if (r_error.empty()) {
			r_error = error;
		}
	}
}

// ---- [load] / [server] (loader thread: no Godot API) ------------------------------------------------------------------

void m2b_gx_load(const M2aGame &p_game, host::SharedLoadOptions &r_options) {
	if (!p_game.m2b) {
		return;
	}
	r_options.tdm = true; // the logic graph, the TDM kits (the assault defibrillator), the motor's immunity abort (ru)
	r_options.tdm_tuning_cfg = p_game.m2b->options.tdm_tuning_cfg;
}

void m2b_gx_server(M2aGame &r_game, host::GameServerConfig &r_config, uint32_t p_bots) {
	if (!r_game.m2b) {
		return;
	}
	M2bGame &m = *r_game.m2b;
	r_config.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	r_config.rules.deploy_screen = m.options.deploy_screen;
	r_config.rules.revive = true;
	r_config.rules.kill_limit_override = m.options.kill_limit;
	r_config.rules.preround_override_s = m.options.preround_s;
	r_config.rules.max_players = 32;
	r_config.firefight = nullptr; // the TDM bots replace bt's firefight bots
	r_game.firefight_stats = nullptr;
	r_game.firefight_bots = 0;
	r_config.tdm_bots = nullptr;
	if (p_bots > 0) {
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = std::min<uint32_t>(p_bots, kM2bMaxBots);
		bots->tuning_cfg = m.options.bot_tuning_cfg;
		r_config.tdm_bots = bots;
	}
	// The death view's pull-in (cam) needs the static world; the M2a checks make it only for themselves.
	if (r_game.world == nullptr && r_config.level != nullptr && r_config.level->static_world != nullptr) {
		bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(r_config.level->static_world);
		if (world) {
			r_game.world = std::move(*world);
		}
	}
	if (r_config.level != nullptr && r_config.level->gunplay != nullptr && r_config.level->gunplay->soldier.time_for_corpse > 0.0f) {
		m.corpse_seconds = r_config.level->gunplay->soldier.time_for_corpse;
	}
	if (r_game.part_notes.size() < 64) {
		r_game.part_notes.push_back("[m2b] " + std::to_string(r_config.tdm_bots != nullptr ? r_config.tdm_bots->count : 0u) +
				" TDM bots (bb), teams by AutoTeam, the round from the Metro TDM logic graph (ru), " +
				(m.options.deploy_screen ? "the spawn screen" : "automatic respawn") + ", max slots " + std::to_string(r_config.max_slots));
	}
}

// ---- [presented] (main thread, every frame) -------------------------------------------------------------------------

void m2b_gx_presented(M2aGame &r_game, host::ClientSession &r_client, const host::PresentationFrame &p_frame, InputMapper &r_mapper, uint64_t p_now_ns) {
	if (!r_game.m2b) {
		return;
	}
	M2bGame &m = *r_game.m2b;
	const host::PresentationFrame &f = p_frame;
	const double now_s = double(p_now_ns) * 1e-9;
	// 1. The own slot (MatchInfo's, spawned or not) and team; the slots' teams from the server's player rows.
	m.own_slot = f.stats.connected || f.match_valid ? f.stats.slot : uint8_t(0xFF);
	const size_t rows = std::min<size_t>(f.player_count, f.players.size());
	for (size_t i = 0; i < rows; ++i) {
		const net::PlayerRow &row = f.players[i];
		if (row.team == 1 || row.team == 2) {
			r_game.slot_team[row.slot] = row.team;
		}
	}
	const net::PlayerRow *own_row = m.own_slot != 0xFF ? m2b_row_of(f, m.own_slot) : nullptr;
	m.own_team = f.own_team == 1 || f.own_team == 2 ? f.own_team : (own_row != nullptr && (own_row->team == 1 || own_row->team == 2) ? own_row->team : uint8_t(0));
	if (!r_game.spawned && m.own_team != 0) {
		r_game.own_team = m.own_team; // before the first Spawned (the join screen) the team is AutoTeam's row
	}
	// 2. The drained events: the own death (the kill card's and the death view's start), revives of the own slot.
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
				if (m.own_killer != 0xFF && m2b_presented_position(f, p_now_ns, m.own_killer, killer)) {
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
	// 3. A new own life (a deploy, a revive, or M2a's automatic respawn without the deploy screen).
	if (r_game.alive && (f.own_curr.life != m.own_life_seen || !m.was_alive)) {
		m.own_life_seen = f.own_curr.life;
		m.spawned_this_round = true;
		m.life_revived = m.revive_seen_s >= 0.0 && now_s - m.revive_seen_s < 2.0;
		if (m.deploy.armed && !m.life_revived) {
			++m.deploys;
			m.kit_check_kit = m.deploy.kit;
			m.kit_check_until_s = now_s + 1.0; // Spawned's kit reaches the frame with the life; a frame of slack
		}
		m2b_deploy_spawned(m.deploy);
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
			UtilityFunctions::print(to_godot(std::string("[m2b] deployed kit ") + host::kit_role_name(host::KitRole(std::min<uint8_t>(f.own_kit, 3))) + ", requested " +
					host::kit_role_name(host::KitRole(std::min<uint8_t>(m.kit_check_kit, 3)))));
			m.kit_check_kit = 0xFF;
		}
	}
	// 4. The round: a restart in place (RoundOver -> PreRound / InRound) brings the join screen back.
	if (f.match_valid) {
		if (m.last_phase == net::MatchPhase::RoundOver && f.match.phase != net::MatchPhase::RoundOver) {
			m.spawned_this_round = false;
			m.own_death_s = -1.0;
		}
		if (f.match.phase != m.last_phase) {
			char line[160];
			std::snprintf(line, sizeof(line), "[m2b] match: %s, US %d - RU %d of %d%s", m2b_phase_text(f.match.phase), f.match.kills[0], f.match.kills[1], f.match.kill_limit,
					f.match.winner == 1 ? ", US wins" : (f.match.winner == 2 ? ", RU wins" : ""));
			UtilityFunctions::print(line);
		}
		m.last_phase = f.match.phase;
		m.round_end = f.match.phase == net::MatchPhase::RoundOver;
	}
	if (!m.options.deploy_screen) {
		m.spawn_screen = false;
		m.can_deploy = false;
		r_mapper.m2b_set_spawn_screen(false);
		return; // M2a's automatic respawn; K / T stay M2a's KitSelect
	}
	// 5. The spawn screen and Deploy.
	M2bScreenInput si;
	si.match_valid = f.match_valid;
	si.phase = f.match.phase;
	si.own_alive = r_game.alive;
	si.own_unspawned = f.own_unspawned;
	si.kill_card_up = m.own_death_s >= 0.0 && now_s - m.own_death_s < host::kKillCardSeconds;
	m.spawn_screen = m2b_spawn_screen_up(si);
	r_mapper.m2b_set_spawn_screen(m.spawn_screen);
	m.can_deploy = m2b_can_deploy(f.match_valid, f.match, f.newest_tick, f.own_respawn_tick);
	if (const int k = r_mapper.m2b_take_kit(); k >= 0 && k < int(host::kKitCount)) {
		m.spawn_kit = uint8_t(k);
	}
	if (const uint32_t k = r_mapper.m2a_take_kit_presses(); k > 0) {
		m.spawn_kit = uint8_t((m.spawn_kit + k) % host::kKitCount); // K cycles the spawn kit (KitSelect is M2a's)
	}
	if (r_mapper.m2a_take_team_presses() > 0) {
		UtilityFunctions::print("[m2b] T: the teams are AutoTeam's in TDM");
	}
	if (r_mapper.m2b_take_deploy_presses() > 0 || m.deploy_click) {
		if (m.spawn_screen && m.can_deploy) {
			m.deploy.armed = true;
		}
		m.deploy_click = false;
	}
	if (m.deploy.armed) {
		m.deploy.kit = m.spawn_kit;
	}
	if (m2b_deploy_step(m.deploy, m.spawn_screen, m.can_deploy, f.newest_tick)) {
		r_client.send_spawn_request(m.deploy.kit);
		++m.spawn_requests_sent;
		m.last_request_tick = f.newest_tick;
		if (m.deploy.resends == 0) {
			UtilityFunctions::print(to_godot(std::string("[m2b] deploy: ") + host::kit_role_name(host::KitRole(m.deploy.kit)) + " (server tick " +
					std::to_string(f.newest_tick) + ")"));
		}
	}
}

// ---- [frame] (main thread, after Done) ------------------------------------------------------------------------------

bool m2b_gx_frame(Bf3LevelSession &r_session, M2aGame &r_game, const host::PresentationFrame &p_frame, uint64_t p_now_ns, double p_delta, Camera3D *p_camera,
		bool p_free_camera, bool p_headless) {
	if (!r_game.m2b) {
		return false;
	}
	M2bGame &m = *r_game.m2b;
	const host::PresentationFrame &f = p_frame;
	const double now_s = double(p_now_ns) * 1e-9;
	const auto ms_since = [](std::chrono::steady_clock::time_point p_t0) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p_t0).count();
	};
	if (!m.built) {
		m2b_build(r_game, m, p_headless);
	}
	InputMapper &mapper = r_session.mapper();
	m.scoreboard_held = mapper.m2b_scoreboard_held();
	// 1. ui: the M2b screens from the same input and drained events as fill_hud (m2b_game.h's frame order).
	auto t0 = std::chrono::steady_clock::now();
#if OPENBF3_M2B_HAVE_UI
	{
		host::HudFeedInput in;
		in.weapon = r_game.weapon;
		in.alive = r_game.alive;
		in.health = f.own_curr.health;
		in.pose = f.own_curr.pose;
		in.now_s = now_s;
		in.m2b = true;
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
		host::fill_hud_tdm(in, r_game.events, r_game.hud_static, m.tdm_state, r_game.hud_model);
		if (!m.options.deploy_screen) {
			r_game.hud_model.spawn_screen.visible = false; // M2a's automatic respawn: nothing to deploy
		}
		if (m.tdm_hud != nullptr && m.tdm_hud->built()) {
			m.tdm_hud->set_model(r_game.hud_model);
			if (const int k = m.tdm_hud->take_kit_click(); k >= 0 && k < int(host::kKitCount)) {
				m.spawn_kit = uint8_t(k);
			}
			if (m.tdm_hud->take_deploy_click()) {
				m.deploy_click = true; // taken by the next frame's [presented] (the request goes out there)
			}
		}
	}
#endif
	m.kill_card_source = host::HudKillCardSource(); // only the frame that drained the own death
	m.ms_tdm_hud = ms_since(t0);
	// 2. cam: the death view while the own soldier is dead; its last camera is kept until the next life.
	t0 = std::chrono::steady_clock::now();
	const bool dead = !r_game.alive && m.spawned_this_round;
#if OPENBF3_M2B_HAVE_CAM
	if (m.death_camera != nullptr) {
		if (m.death_view_end) {
			m.death_camera->end();
		}
		if (m.death_view_pending) {
			DeathCameraStart st;
			const sim::Vec3 &p = f.own_curr.position;
			st.corpse = fx::Vec3{ p[0], p[1], p[2] };
			st.yaw_rad = mapper.view().yaw;
			const Vector3 eye = r_session.cameras().last_eye();
			st.eye = fx::Vec3{ eye.x, eye.y, eye.z };
			st.killer_slot = m.own_killer == m.own_slot ? uint8_t(0xFF) : m.own_killer;
			st.killer_camera = !f.match_valid || f.match.killer_camera;
			m.death_camera->begin(st, now_s);
		}
		if (m.death_camera->active()) {
			sim::Vec3 k{};
			fx::Vec3 killer{};
			const bool seen = m2b_presented_position(f, p_now_ns, m.own_killer, k);
			if (seen) {
				killer = fx::Vec3{ k[0], k[1], k[2] };
			}
			const DeathCameraView v = m.death_camera->step(now_s, seen ? &killer : nullptr, r_game.world.get());
			if (v.active && p_camera != nullptr) {
				const Transform3D t = m2b_camera_transform(v.camera);
				m2b_keep_view(m, t);
			}
		}
	}
#endif
	m.death_view_pending = false;
	m.death_view_end = false;
	if (p_camera != nullptr && !p_free_camera) {
		if (dead && m.view_valid) {
			p_camera->set_global_transform(m2b_kept_view(m)); // FpCamera put it on the corpse's eye: the death view's last frame
		} else if (!m.spawned_this_round && !f.valid && !m.join_view_placed) {
			// The join screen: no own soldier yet, so the camera looks over the first spawn point.
			const std::shared_ptr<const host::SharedLevel> shared = r_session.shared_level();
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
	// 3. The mouse: free while the spawn screen or the round end is up (their buttons), captured again after.
	bool wants_mouse = m.spawn_screen;
#if OPENBF3_M2B_HAVE_UI
	if (m.tdm_hud != nullptr && m.tdm_hud->built()) {
		wants_mouse = wants_mouse || m.tdm_hud->wants_mouse();
	}
#endif
	if (!p_headless && !mapper.scripted()) {
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
	m2b_status_text(m, f, r_game.alive, r_session.get_viewport());
	// 4. The per-frame view of the M2b parts (the check and ui's rows read it).
	m.frame.m2a = nullptr;
	m.frame.match_valid = f.match_valid;
	m.frame.match = f.match_valid ? &f.match : nullptr;
	m.frame.players = std::span<const net::PlayerRow>(f.players.data(), std::min<size_t>(f.player_count, f.players.size()));
	m.frame.server_tick = f.newest_tick;
	m.frame.now_s = now_s;
	m.frame.own_alive = r_game.alive;
	m.frame.own_unspawned = f.own_unspawned;
	m.frame.spawn_screen = m.spawn_screen;
	m.frame.death_view = m.view_valid && dead;
	// 5. The M2b check (once the HUD has loaded, or failed).
	if (m.checks() && m.check == nullptr && !m.finished && (r_game.hud == nullptr || r_game.hud->is_loaded())) {
		m.check = std::unique_ptr<M2bCheck, void (*)(M2bCheck *)>(new M2bCheck(r_session, r_game, m, m.cases, m.check_options), &delete_m2b_part<M2bCheck>);
	}
	if (m.check != nullptr && !m.finished) {
		if (m.check->frame(p_now_ns, p_delta)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.check->report()));
			r_session.finish(m.check->exit_code());
			return true;
		}
	}
	return false;
}

// ---- [shutdown] -----------------------------------------------------------------------------------------------------

void m2b_gx_shutdown(M2aGame &r_game) {
	if (!r_game.m2b) {
		return;
	}
	M2bGame &m = *r_game.m2b;
	m.check.reset();
#if OPENBF3_M2B_HAVE_UI
	if (m.tdm_hud != nullptr) {
		m.tdm_hud->shutdown();
	}
#endif
	m.death_camera.reset();
}

// ---- [done] (level_session.cpp's try_done) --------------------------------------------------------------------------

bool Bf3LevelSession::m2b_join_ready() const {
	if (!m2a_ || !m2a_->m2b || !m2a_->m2b->enabled || !m2a_->m2b->options.deploy_screen || frame_ == nullptr) {
		return false;
	}
	// The spawn screen at join (m2b.md §1.1 step 1): the load is done when the match is known, not at the first
	// spawn (the deploy screen's human is not spawned by its Ready, R10).
	const host::PresentationFrame &f = *frame_;
	return f.match_valid && f.stats.connected && !f.stats.refused;
}

} // namespace bf3::godot_side
