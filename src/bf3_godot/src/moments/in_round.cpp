#include "in_round_moments.h"

#include "level_session.h"
#include "native_hud.h"
#include "native_hud_moments.h"

#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/math.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/host/client_session.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr std::array<NativeHudMoment, 16> kMoments{ {
		{ "m01", 20, 100, 1, 100, 0, 4, 295, 44.125f, true, 678.0f, 926.0f, 66.0f, 679.0f, 926.0f, 66.0f, 296 },
		{ "m02", 25, 217, 1, 100, 9, 12, 212, 95.250f, true, 722.0f, 857.0f, 152.0f, 722.0f, 857.0f, 152.5f, 210 },
		{ "m03", 31, 124, 1, 100, 50, 61, 270, 95.250f, true, 765.0f, 851.0f, 91.5f, 764.0f, 851.0f, 90.75f, 270 },
		{ "m04", 31, 217, 1, 100, 73, 83, 49, 99.250f, true, 726.0f, 910.0f, 312.5f, 727.0f, 909.0f, 313.5f, 49 },
		{ "m05", 30, 217, 1, 100, 74, 83, 82, 97.250f, true, 728.0f, 909.0f, 279.5f, 728.0f, 908.0f, 279.75f, 82 },
		{ "m06", 6, 217, 1, 94, 79, 85, 68, 115.375f, true, 760.0f, 920.0f, 294.0f, 760.0f, 919.0f, 294.75f, 59 },
		{ "m07", 31, 192, 1, 94, 79, 85, 289, 96.125f, true, 792.0f, 926.0f, 69.0f, 758.0f, 912.0f, 72.25f, 289, true },
		{ "m08", 21, 192, 1, 94, 79, 87, 286, 72.250f, false, 752.0f, 912.0f, 76.5f, 752.0f, 912.0f, 76.0f, 286 },
		{ "m09", 31, 168, 1, 100, 81, 88, 285, 115.250f, true, 734.0f, 914.0f, 77.0f, 736.0f, 914.0f, 78.0f, 285 },
		{ "m10", 31, 168, 1, 74, 85, 89, 87, 99.250f, true, 717.0f, 897.0f, 275.0f, 718.0f, 898.0f, 274.75f, 87 },
		{ "m11", 0, 0, 0, 87, 85, 89, 69, 4.250f, false, 717.0f, 898.0f, 293.0f, 719.0f, 897.0f, 293.25f, 69 },
		{ "m12", 17, 217, 0, 100, 87, 89, 302, 115.250f, true, 723.0f, 896.0f, 59.5f, 723.0f, 895.0f, 59.5f, 302 },
		{ "m13", 31, 217, 1, 100, 90, 93, 3, 92.000f, true, 684.0f, 868.0f, 359.5f, 684.0f, 868.0f, 0.0f, 2 },
		{ "m14", 31, 217, 1, 100, 92, 98, 192, 115.250f, true, 685.0f, 882.0f, 168.5f, 685.0f, 882.0f, 169.0f, 192 },
		{ "m15", 31, 217, 1, 100, 92, 99, 135, 115.250f, true, 685.0f, 889.0f, 229.5f, 686.0f, 890.0f, 229.75f, 129 },
		{ "m16", 17, 199, 0, 100, 87, 89, 80, 97.250f, true, 724.0f, 898.0f, 286.0f, 724.0f, 899.0f, 286.0f, 75 },
} };

constexpr double kSettleSeconds = 3.0;
constexpr float kMapRegistrationTexels = 1024.0f;
constexpr float kRegistrationRotationStep = 0.5f;

enum class MomentFade : uint8_t { None, In, Out };

struct MomentKill {
	const char *moment = "";
	uint8_t killer = 0;
	uint8_t victim = 0;
	uint32_t weapon = 0;
	const char *weapon_text = "";
	float killer_px = 0.0f;
	float victim_px = 0.0f;
	float alpha = 1.0f;
	MomentFade fade = MomentFade::None;
};

struct MomentPopup {
	const char *moment = "";
	uint8_t killer = 0;
	uint8_t victim = 0;
	uint32_t weapon = 0;
	const char *weapon_text = "";
	float killer_px = 0.0f;
	float victim_px = 0.0f;
	int32_t score_frame = -1;
	int32_t aggregator_frame = -1;
};

constexpr std::array<MomentKill, 63> kMomentKills{ {
		{ "m01", 0, 1, 0x0fd9c998u, "M40A5", 376.0f, 168.0f, 1.00f },
		{ "m01", 0, 1, 0x7672e09eu, "XBOW", 405.0f, 149.0f, 1.00f },
		{ "m02", 0, 1, 0x2745930bu, "MK11 MOD 0", 356.0f, 155.0f, 1.00f },
		{ "m02", 0, 1, 0x05bca424u, "SPAS-12", 387.0f, 149.0f, 1.00f },
		{ "m02", 1, 2, 0x0fe93389u, "MP443", 396.0f, 150.0f, 1.00f },
		{ "m03", 2, 1, 0x27415b44u, "G36C", 402.0f, 153.0f, 0.41f, MomentFade::Out },
		{ "m03", 0, 1, 0x2744a919u, "M416", 420.0f, 139.0f, 1.00f },
		{ "m03", 1, 0, 0x273e842du, "AN-94", 414.0f, 138.0f, 1.00f },
		{ "m03", 1, 0, 0xa8652d39u, "SAIGA 12K", 382.0f, 139.0f, 1.00f },
		{ "m04", 0, 1, 0x2745930bu, "MK11 MOD 0", 363.0f, 144.0f, 1.00f },
		{ "m04", 0, 1, 0x2745930bu, "MK11 MOD 0", 368.0f, 143.0f, 1.00f },
		{ "m04", 1, 0, 0x273e842du, "AN-94", 412.0f, 140.0f, 1.00f },
		{ "m04", 2, 1, 0x0fc9f7ddu, "L85A2", 382.0f, 167.0f, 1.00f },
		{ "m05", 0, 1, 0x2745930bu, "MK11 MOD 0", 364.0f, 148.0f, 1.00f },
		{ "m05", 0, 1, 0x2745930bu, "MK11 MOD 0", 368.0f, 143.0f, 1.00f },
		{ "m05", 1, 0, 0x273e842du, "AN-94", 412.0f, 140.0f, 1.00f },
		{ "m05", 2, 1, 0x0fc9f7ddu, "L85A2", 382.0f, 167.0f, 1.00f },
		{ "m06", 1, 0, 0xa8652d39u, "SAIGA 12K", 385.0f, 137.0f, 1.00f },
		{ "m06", 1, 2, 0x8511b67eu, "SVD", 415.0f, 150.0f, 1.00f },
		{ "m06", 0, 1, 0x2744a919u, "M416", 402.0f, 156.0f, 1.00f },
		{ "m06", 1, 0, 0x8511b522u, "SKS", 423.0f, 143.0f, 1.00f },
		{ "m07", 1, 2, 0x8511b67eu, "SVD", 415.0f, 150.0f, 1.00f },
		{ "m07", 0, 1, 0x2744a919u, "M416", 402.0f, 156.0f, 1.00f },
		{ "m07", 1, 0, 0x8511b522u, "SKS", 423.0f, 143.0f, 1.00f },
		{ "m07", 0, 1, 0x0fd83dfau, "M16A3", 410.0f, 139.0f, 1.00f },
		{ "m07", 1, 2, 0x8511b67eu, "SVD", 425.0f, 140.0f, 1.00f },
		{ "m08", 1, 0, 0x8511b522u, "SKS", 423.0f, 143.0f, 1.00f },
		{ "m08", 0, 1, 0x0fd83dfau, "M16A3", 410.0f, 139.0f, 1.00f },
		{ "m08", 1, 2, 0x8511b67eu, "SVD", 425.0f, 140.0f, 1.00f },
		{ "m09", 1, 2, 0x8511b67eu, "SVD", 425.0f, 140.0f, 1.00f },
		{ "m09", 0, 1, 0x1078c98cu, "UMP-45", 354.0f, 186.0f, 1.00f },
		{ "m09", 0, 1, 0x0fd83dfau, "M16A3", 419.0f, 131.0f, 1.00f },
		{ "m09", 1, 0, 0xa8652d39u, "SAIGA 12K", 388.0f, 133.0f, 0.28f, MomentFade::In },
		{ "m09", 0, 1, 0x0fd83dfau, "M16A3", 389.0f, 161.0f, 0.43f, MomentFade::In },
		{ "m10", 1, 0, 0xa8652d39u, "SAIGA 12K", 388.0f, 133.0f, 1.00f },
		{ "m10", 0, 1, 0x0fd83dfau, "M16A3", 389.0f, 161.0f, 1.00f },
		{ "m10", 0, 1, 0xdfcd9e7cu, "MK3A1", 395.0f, 158.0f, 1.00f },
		{ "m10", 1, 0, 0x273e842du, "AN-94", 398.0f, 155.0f, 1.00f },
		{ "m11", 1, 0, 0xa8652d39u, "SAIGA 12K", 388.0f, 133.0f, 0.06f, MomentFade::Out },
		{ "m11", 0, 1, 0x0fd83dfau, "M16A3", 389.0f, 161.0f, 1.00f },
		{ "m11", 0, 1, 0xdfcd9e7cu, "MK3A1", 395.0f, 158.0f, 1.00f },
		{ "m11", 1, 0, 0x273e842du, "AN-94", 398.0f, 155.0f, 1.00f },
		{ "m12", 0, 1, 0xdfcd9e7cu, "MK3A1", 395.0f, 158.0f, 1.00f },
		{ "m12", 1, 0, 0x273e842du, "AN-94", 398.0f, 155.0f, 1.00f },
		{ "m12", 0, 1, 0x2745930bu, "MK11 MOD 0", 356.0f, 154.0f, 1.00f },
		{ "m12", 1, 0, 0xf0576243u, "AEK-971", 393.0f, 146.0f, 1.00f },
		{ "m13", 0, 1, 0x0fd83dfau, "M16A3", 395.0f, 153.0f, 1.00f },
		{ "m13", 0, 1, 0x0fdcffc0u, "M9 TACT.", 388.0f, 139.0f, 1.00f },
		{ "m13", 0, 1, 0x8511986au, "M39 EMR", 384.0f, 144.0f, 1.00f },
		{ "m13", 1, 0, 0x140df493u, "PP-2000", 393.0f, 140.0f, 1.00f },
		{ "m14", 1, 0, 0xa8652d39u, "SAIGA 12K", 381.0f, 145.0f, 0.00f, MomentFade::Out },
		{ "m14", 1, 0, 0x8511b67eu, "SVD", 415.0f, 150.0f, 1.00f },
		{ "m14", 1, 0, 0x0f0d5c75u, "AK-74M", 385.0f, 158.0f, 1.00f },
		{ "m14", 1, 0, 0xf0c84fb0u, "AKS-74u", 397.0f, 138.0f, 1.00f },
		{ "m14", 0, 1, 0x7672e09eu, "XBOW", 392.0f, 160.0f, 1.00f },
		{ "m15", 1, 0, 0x8511b67eu, "SVD", 415.0f, 150.0f, 0.28f, MomentFade::Out },
		{ "m15", 1, 0, 0x0f0d5c75u, "AK-74M", 385.0f, 158.0f, 1.00f },
		{ "m15", 1, 0, 0xf0c84fb0u, "AKS-74u", 397.0f, 138.0f, 1.00f },
		{ "m15", 0, 1, 0x7672e09eu, "XBOW", 392.0f, 160.0f, 1.00f },
		{ "m16", 0, 1, 0xdfcd9e7cu, "MK3A1", 395.0f, 158.0f, 1.00f },
		{ "m16", 1, 0, 0x273e842du, "AN-94", 398.0f, 155.0f, 1.00f },
		{ "m16", 0, 1, 0x2745930bu, "MK11 MOD 0", 356.0f, 154.0f, 1.00f },
		{ "m16", 1, 0, 0xf0576243u, "AEK-971", 393.0f, 146.0f, 1.00f },
} };

constexpr std::array<MomentPopup, 4> kMomentPopups{ {
		{ "m06", 2, 1, 0x0fd83dfau, "M16A3", 172.0f, 267.0f, 15, 13 },
		{ "m07", 2, 1, 0x0fd83dfau, "M16A3", 172.0f, 267.0f, 48, 14 },
		{ "m12", 2, 1, 0x0fd83dfau, "M16A3", 168.0f, 272.0f, 17, -1 },
		{ "m16", 2, 1, 0x0fd83dfau, "M16A3", 168.0f, 272.0f, 14, -1 },
} };

struct MomentSquad {
	const char *moment = "";
	std::array<NativeHudSquadRow, ui::kHudSquadSize> rows{};
};

constexpr MomentSquad squad_of(const char *p_moment, int16_t p_leader_booster, int16_t p_kit1, int16_t p_kit2, int16_t p_booster2, int16_t p_booster3) {
	return { p_moment, { { { 70, p_leader_booster, true }, { p_kit1, 138, false }, { p_kit2, p_booster2, false }, { 70, p_booster3, false } } } };
}

constexpr std::array<MomentSquad, 16> kMomentSquads{ {
		squad_of("m01", 143, 70, 71, 139, 140),
		squad_of("m02", 143, 145, 145, 139, 140),
		squad_of("m03", 143, 70, 71, 144, 140),
		squad_of("m04", 139, 70, 71, 139, 140),
		squad_of("m05", 139, 70, 71, 139, 140),
		squad_of("m06", 139, 70, 71, 139, 143),
		squad_of("m07", 139, 70, 71, 139, 143),
		squad_of("m08", 139, 70, 71, 139, 143),
		squad_of("m09", 139, 70, 71, 139, 143),
		squad_of("m10", 139, 70, 71, 139, 143),
		squad_of("m11", 139, 70, 71, 139, 143),
		squad_of("m12", 139, 70, 71, 139, 143),
		squad_of("m13", 139, 70, 71, 139, 143),
		squad_of("m14", 139, 70, 71, 139, 143),
		squad_of("m15", 139, 70, 71, 139, 143),
		squad_of("m16", 139, 70, 71, 139, 143),
} };

constexpr std::array<float, 2> kSquadNamePx{ 452.0f - 297.0f, 452.0f - 276.0f };
constexpr uint8_t kSquadSlot0 = 24;
constexpr const char *kSquadListPath = "instance1.SquadList_00";

constexpr double kFlushMarginSeconds = 1.0;
constexpr double kKillLeadSeconds = 3.0;
constexpr double kPreScoreLeadSeconds = 7.9;
constexpr int32_t kLateRowFrames = 10;
constexpr uint8_t kLocalSlot = 0;
constexpr uint8_t kLogVictimSlot = 20;
constexpr int8_t kOwnTeam = 1;
constexpr int8_t kOtherTeam = 2;
constexpr int8_t kOwnSquad = 1;
constexpr int8_t kOtherSquad = 2;
constexpr const char *kKillFeedPath = "instance1.HudMessage_02";
constexpr const char *kKillLogPath = "instance1.HudMessage_00";
constexpr const char *kScorePath = "instance1.HudMessage_01";
constexpr const char *kAggregatorPath = "instance1.ScoreAggregator_01.aggHolder";
constexpr std::array<std::string_view, 2> kPreScoreMinorSids{ "ID_SCORE_AVENGER", "ID_SCORE_SAVIOUR" };
constexpr std::string_view kFootageAggregate = "240";

void set_name(ui::HudModel &r_m, uint8_t p_slot, int8_t p_team, int8_t p_squad, const std::string &p_name) {
	ui::HudPlayer &p = r_m.players[p_slot];
	p = ui::HudPlayer{};
	p.team = p_team;
	p.squad = p_squad;
	p.alive = true;
	p.local = p_slot == kLocalSlot;
	std::copy_n(p_name.data(), std::min(p_name.size(), p.name.size() - 1), p.name.data());
}

void relation_slot(ui::HudModel &r_m, uint8_t p_slot, uint8_t p_relation, const std::string &p_name) {
	const bool enemy = int32_t(p_relation) == ui::kHudRelationEnemy;
	const bool squad = int32_t(p_relation) == ui::kHudRelationSquad;
	set_name(r_m, p_slot, enemy ? kOtherTeam : kOwnTeam, squad ? kOwnSquad : (enemy ? int8_t(0) : kOtherSquad), p_name);
}

void push_kill(ui::HudKillFeed &r_f, uint8_t p_killer, uint8_t p_victim, uint32_t p_weapon) {
	++r_f.newest_serial;
	ui::HudKill &k = r_f.lines[r_f.newest_serial % r_f.lines.size()];
	k = ui::HudKill{};
	k.serial = r_f.newest_serial;
	k.killer = p_killer;
	k.victim = p_victim;
	k.weapon_name_sid_hash = p_weapon;
}

void push_score(ui::HudScoreFeed &r_f, std::string_view p_sid, int32_t p_points) {
	++r_f.newest_serial;
	ui::HudScoreEvent &e = r_f.events[r_f.newest_serial % r_f.events.size()];
	e.serial = r_f.newest_serial;
	e.sid_hash = ui::text_hash(p_sid);
	e.points = p_points;
}

class InRoundMoments final : public NativeHudMomentGroup {
public:
	NativeHudMomentWait ready(const Bf3LevelSession &p_session, Bf3NativeHud &r_hud) override;
	size_t moment_count() const override { return kMoments.size(); }
	std::string_view moment_id(size_t p_moment) const override { return kMoments[p_moment].id; }
	std::optional<NativeHudMomentPlace> place(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, size_t p_moment) override;
	void settle(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment) override;
	void check(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment, const Ref<Image> &p_layer) override;

private:
	void settle_moment(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment, ui::HudModel p_model);
	void check_moment(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
	void check_messages(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
	void check_squad(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);

	std::vector<std::string> expect_kills_;
	std::vector<int32_t> expect_kill_frames_;
	std::vector<float> reference_kill_alphas_;
	std::vector<std::string> expect_log_;
	int32_t expect_score_frame_ = -1;
	int32_t expect_aggregator_frame_ = -1;
	float name_scale_ = 0.0f;
	std::string names_detail_;
	std::vector<NativeHudSquadRow> expect_squad_;
	std::vector<std::string> expect_squad_names_;
};

}

std::span<const NativeHudMoment> native_hud_moments() {
	return kMoments;
}

ui::HudModel native_hud_moment_model(const NativeHudMoment &p_moment, const ui::HudModel &p_base, double p_crosshair_base_offset) {
	ui::HudModel m = p_base;
	m.soldier.is_alive = true;
	m.soldier.health = float(p_moment.health);
	m.soldier.compass_angle = bf3::host::hud_compass_heading_deg(native_hud_moment_view_yaw(p_moment));
	ui::HudWeapon &w = m.weapon;
	w.active_weapon_ammo = p_moment.clip;
	w.active_weapon_ammo_in_bag = p_moment.reserve;
	w.secondary_weapon_ammo = p_moment.grenades;
	w.active_weapon_show_ammo = true;
	w.active_weapon_critical_ammo = false;
	w.firing_mode = 2;
	w.firing_mode_types = (1u << 0) | (1u << 2);
	w.active_weapon_crosshair = 1;
	w.zoomed = !p_moment.crosshair;
	w.dispersion = ui::hud_crosshair_spread_for_arm(double(p_moment.dispersion_px), p_crosshair_base_offset);
	w.absolute_dispersion = w.dispersion;
	m.team.friendly_team = 1;
	m.team.enemy_team1 = 2;
	m.team.friendly_team_name = m.team.team1_name;
	m.team.enemy_team1_name = m.team.team2_name;
	m.kill_counter.player_team = 1;
	m.kill_counter.team1_kills = p_moment.tickets_us;
	m.kill_counter.team2_kills = p_moment.tickets_ru;
	m.kill_counter.team_kills_list.count = 2;
	m.kill_counter.team_kills_list.kills[0] = p_moment.tickets_us;
	m.kill_counter.team_kills_list.kills[1] = p_moment.tickets_ru;
	m.kill_counter.player_team_kills = p_moment.tickets_us;
	m.kill_counter.enemy_team_kills = p_moment.tickets_ru;
	++m.serial;
	return m;
}

float native_hud_moment_heading_deg(const NativeHudMoment &p_moment) {
	return float(p_moment.compass_frame - 1) - 0.5f;
}

float native_hud_moment_view_yaw(const NativeHudMoment &p_moment) {
	return float(-double(native_hud_moment_heading_deg(p_moment)) * bf3::core::kDegToRad);
}

std::array<float, 2> native_hud_moment_texel(const NativeHudMoment &p_moment) {
	return p_moment.detail_ambiguous ? std::array<float, 2>{ p_moment.area_texel_x, p_moment.area_texel_y } : std::array<float, 2>{ p_moment.map_texel_x, p_moment.map_texel_y };
}

std::array<float, 2> native_hud_moment_world_xz(const NativeHudMoment &p_moment, const ui::MinimapLevel &p_map) {
	const std::array<float, 2> texel = native_hud_moment_texel(p_moment);
	return ui::minimap_uv_to_world(ui::minimap_mapping(p_map.data), { texel[0] / kMapRegistrationTexels, texel[1] / kMapRegistrationTexels });
}

float native_hud_moment_registered_heading_deg(const NativeHudMoment &p_moment) {
	return 360.0f - (p_moment.detail_ambiguous ? p_moment.area_rotation_deg : p_moment.map_rotation_deg);
}

NativeHudMomentWait InRoundMoments::ready(const Bf3LevelSession &p_session, Bf3NativeHud &r_hud) {
	const host::PresentationFrame &f = p_session.frame();
	const bool alive = r_hud.model().soldier.is_alive;
	const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
	const bool minimap = r_hud.minimap_ready();
	if (!alive || !in_round) {
		return { false, "alive_in_round",
			std::string("own soldier ") + (alive ? "alive" : "not spawned") + ", round " +
					(in_round ? "in round" : "not in round") };
	}
	if (!minimap) {
		return { false, "minimap_ready", "the minimap source has no satellite texture to paint" };
	}
	return { true, {}, {} };
}

std::optional<NativeHudMomentPlace> InRoundMoments::place(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, size_t p_moment) {
	const NativeHudMoment &m = kMoments[p_moment];
	const std::optional<ui::MinimapLevel> map = r_hud.minimap_level();
	if (!map) {
		r_run.row(std::string(m.id) + ".place", false, "no minimap level to place the registration texel");
		return std::nullopt;
	}
	const std::array<float, 2> at_xz = native_hud_moment_world_xz(m, *map);
	if (m.detail_ambiguous) {
		UtilityFunctions::print(String::utf8((std::string("NATIVE HUD FLAG ") + m.id + " the detail registration disagrees with the combat-area registration; the state uses the combat-area one").c_str()));
	}
	return NativeHudMomentPlace{ at_xz[0], at_xz[1], native_hud_moment_view_yaw(m) };
}

void InRoundMoments::settle(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment) {
	const NativeHudMoment &m = kMoments[p_moment];
	settle_moment(r_run, r_session, r_hud, m, native_hud_moment_model(m, r_hud.model(), r_hud.readout().crosshair_base_offset));
}

void InRoundMoments::check(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment, const Ref<Image> &p_layer) {
	const NativeHudMoment &m = kMoments[p_moment];
	if (const ui::SwfScene *ls = name_scale_ > 0.0f && p_layer.is_valid() ? native_hud_scene_of(r_hud, kKillLogPath) : nullptr) {
		const float stage_px = name_scale_ * ls->stage().width();
		r_run.row(std::string(m.id) + ".name_scale", std::fabs(stage_px - float(p_layer->get_width())) < 0.5f,
				"names fitted at " + std::to_string(name_scale_) + " layer px per stage unit: stage " + std::to_string(ls->stage().width()) + " -> " +
						std::to_string(stage_px) + " px, the HUD layer is " + std::to_string(p_layer->get_width()) + " px wide");
	}
	name_scale_ = 0.0f;
	check_moment(r_run, r_session, r_hud, m);
}

void InRoundMoments::settle_moment(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment, ui::HudModel p_model) {
	const std::string id = p_moment.id;
	const NativeHudMinimapPose pose = native_hud_moment_pose(r_run.feet(), native_hud_moment_view_yaw(p_moment));
	expect_kills_.clear();
	expect_kill_frames_.clear();
	reference_kill_alphas_.clear();
	expect_log_.clear();
	expect_score_frame_ = -1;
	expect_aggregator_frame_ = -1;
	std::vector<const MomentKill *> kills;
	for (const MomentKill &k : kMomentKills) {
		if (id == k.moment) {
			kills.push_back(&k);
		}
	}
	const MomentPopup *pop = nullptr;
	for (const MomentPopup &p : kMomentPopups) {
		pop = id == p.moment ? &p : pop;
	}
	r_hud.set_ekg_clock(0.0);
	const ui::HudMessageComp *comp = r_hud.messages();
	const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
	NativeHudNameFit fit;
	std::string error = comp == nullptr ? std::string("no HudMessageComp") : fit.open("ui/assets/hudmessage", { "Kill", "LocalKill", "Score" });
	if (error.empty() && pop != nullptr && (level == nullptr || level->tdm_tuning == nullptr)) {
		error = "no TdmTuning scores";
	}
	std::vector<std::pair<std::string_view, int32_t>> pre_minor;
	for (const std::string_view sid : kPreScoreMinorSids) {
		const std::optional<double> v = error.empty() && pop != nullptr && level->scoring != nullptr ? level->scoring->score(sid) : std::nullopt;
		if (error.empty() && pop != nullptr && !v) {
			error = "no " + std::string(sid) + " row in the install scoring table";
		}
		pre_minor.emplace_back(sid, v ? int32_t(std::lround(*v)) : 0);
	}
	const std::optional<uint32_t> score_start = error.empty() ? fit.label("Score", "start") : std::nullopt;
	if (error.empty() && pop != nullptr && !score_start) {
		error = "no Score start label";
	}
	if (!error.empty()) {
		r_run.row(id + ".messages", false, error);
		r_hud.set_fixture(p_model, pose);
		for (double t = 0.0; t < kSettleSeconds; t += kNativeHudMomentStep) {
			r_hud.step(kNativeHudMomentStep);
		}
		return;
	}
	const ui::HudMessageCompData &d = comp->data();
	const size_t queue = size_t(std::max(d.of(ui::HudMessageSource::Kill).queue_size, 0));
	const double flush = double(d.score_aggregate_time) + kFlushMarginSeconds;
	const double lead = pop != nullptr ? kPreScoreLeadSeconds : kKillLeadSeconds;
	const int32_t total = int32_t(std::lround((flush + lead) / kNativeHudMomentStep));
	const auto step_before = [&](double p_seconds) { return total - int32_t(std::lround(p_seconds / kNativeHudMomentStep)); };

	const ui::SwfSceneNode *feed = native_hud_node_of(r_hud, kKillFeedPath);
	const ui::SwfSceneNode *log = native_hud_node_of(r_hud, kKillLogPath);
	name_scale_ = native_hud_layer_scale(r_hud, kKillLogPath);
	const float feed_scale = (feed != nullptr ? feed->world.a : 1.0f) * native_hud_layer_scale(r_hud, kKillFeedPath);
	const float log_scale = (log != nullptr ? log->world.a : 1.0f) * name_scale_;
	float worst = 0.0f;
	const auto fitted_in = [&](NativeHudNameFit &r_fit, std::string_view p_field, float p_scale, float p_px) {
		const float s = p_scale * r_fit.chain_scale(p_field);
		const std::string name = s > 0.0f ? r_fit.fit(p_field, p_px / s) : std::string();
		worst = std::max(worst, std::fabs(r_fit.width(p_field, name) * s - p_px));
		return name;
	};
	const auto fitted = [&](std::string_view p_field, float p_scale, float p_px) { return fitted_in(fit, p_field, p_scale, p_px); };
	ui::HudModel m = p_model;
	m.players.fill(ui::HudPlayer{});
	m.local_player = kLocalSlot;
	m.local_team = kOwnTeam;
	set_name(m, kLocalSlot, kOwnTeam, kOwnSquad, pop != nullptr ? fitted("LocalKill.container.killer.txt", log_scale, pop->killer_px) : std::string());
	for (size_t i = 0; i < kills.size(); ++i) {
		relation_slot(m, uint8_t(1 + 2 * i), kills[i]->killer, fitted("Kill.container.killer.txt", feed_scale, kills[i]->killer_px));
		relation_slot(m, uint8_t(2 + 2 * i), kills[i]->victim, fitted("Kill.container.victim.txt", feed_scale, kills[i]->victim_px));
		expect_kills_.push_back(std::string("[") + kills[i]->weapon_text + "]");
	}
	if (pop != nullptr) {
		relation_slot(m, kLogVictimSlot, pop->victim, fitted("LocalKill.container.victim.txt", log_scale, pop->victim_px));
		expect_log_.push_back(std::string("[") + pop->weapon_text + "]");
		expect_score_frame_ = pop->score_frame;
		expect_aggregator_frame_ = pop->aggregator_frame;
	}
	expect_squad_.clear();
	expect_squad_names_.clear();
	const MomentSquad *squad = nullptr;
	for (const MomentSquad &q : kMomentSquads) {
		squad = id == q.moment ? &q : squad;
	}
	if (squad != nullptr) {
		NativeHudNameFit squad_fit;
		const std::string squad_error = squad_fit.open("ui/assets/squadlist", { "SquadList" });
		if (!squad_error.empty()) {
			r_run.row(id + ".squad_list", false, squad_error);
		} else {
			const ui::SwfSceneNode *sl = native_hud_node_of(r_hud, kSquadListPath);
			const float sl_scale = (sl != nullptr ? sl->world.a : 1.0f) * native_hud_layer_scale(r_hud, kSquadListPath);
			ui::HudSquadMemberList &members = m.squad.local_squad_members;
			members = ui::HudSquadMemberList{};
			for (size_t i = 0; i < squad->rows.size(); ++i) {
				const NativeHudSquadRow &q = squad->rows[i];
				const uint8_t slot = uint8_t(kSquadSlot0 + i);
				const std::string field = "SquadList.squadMember" + std::to_string(i) + ".label.txt";
				const std::string name = fitted_in(squad_fit, field, sl_scale, kSquadNamePx[q.leader ? 0 : 1]);
				set_name(m, slot, kOwnTeam, kOwnSquad, name);
				ui::HudPlayer &p = m.players[slot];
				p.alive = q.kit != ui::kUiHudIconDead;
				p.kit_icon = p.alive ? q.kit : int16_t(0);
				p.booster_icon = q.booster;
				members.index[i] = slot;
				members.leader = q.leader ? slot : members.leader;
				expect_squad_.push_back(q);
				expect_squad_names_.push_back(name);
			}
			members.count = uint8_t(squad->rows.size());
		}
	}
	const int32_t pre_score_step = pop != nullptr && pop->aggregator_frame >= 0 ? step_before(kPreScoreLeadSeconds) : -1;
	const int32_t kill_step = step_before(kKillLeadSeconds);
	const int32_t late_step = total - kLateRowFrames;
	const int32_t popup_step = pop != nullptr ? total - (pop->score_frame - int32_t(*score_start)) : -1;
	std::vector<double> clock(size_t(std::max(total, 0)));
	double c = r_hud.message_clock();
	for (double &t : clock) {
		c += kNativeHudMomentStep;
		t = c;
	}
	const double kill_seconds = double(d.of(ui::HudMessageSource::Kill).normal_seconds);
	const auto removal_step = [&](int32_t p_push) {
		for (int32_t j = p_push + 1; j < total; ++j) {
			if (clock[size_t(p_push)] + kill_seconds <= clock[size_t(j)]) {
				return j;
			}
		}
		return total;
	};
	const std::optional<uint32_t> in_label = fit.label("Kill", "startAnim");
	const std::optional<uint32_t> out_label = fit.label("Kill", "outAnim");
	const uint32_t kill_frames = fit.frames("Kill");
	const auto nearest_frame = [&](uint32_t p_first, uint32_t p_last, float p_alpha) {
		std::optional<uint32_t> best;
		float best_d = std::numeric_limits<float>::infinity();
		for (uint32_t f = p_first; f <= p_last; ++f) {
			const std::optional<float> a = fit.alpha_at("Kill", "container", f);
			if (a && std::fabs(*a - p_alpha) < best_d) {
				best = f;
				best_d = std::fabs(*a - p_alpha);
			}
		}
		return best;
	};
	std::vector<int32_t> push_step(kills.size(), -1);
	std::string fade_error;
	std::optional<uint32_t> last_in;
	for (size_t i = 0; i < kills.size(); ++i) {
		expect_kill_frames_.push_back(-1);
		reference_kill_alphas_.push_back(kills[i]->alpha);
		if (kills[i]->fade == MomentFade::None) {
			push_step[i] = i < queue ? kill_step : late_step;
			continue;
		}
		if (!in_label || !out_label || kill_frames < *out_label + 2) {
			fade_error = "no Kill startAnim / outAnim frames in the probe movie";
			break;
		}
		if (kills[i]->fade == MomentFade::In) {
			std::optional<uint32_t> f = nearest_frame(*in_label + 1, *out_label - 1, kills[i]->alpha);
			if (f && last_in) {
				f = std::min(*f, *last_in);
			}
			if (!f) {
				fade_error = "no Kill startAnim alpha";
				break;
			}
			last_in = f;
			push_step[i] = total - int32_t(*f - *in_label);
			expect_kill_frames_.back() = int32_t(*f);
			continue;
		}
		const std::optional<uint32_t> f = nearest_frame(*out_label + 1, kill_frames - 2, kills[i]->alpha);
		if (!f) {
			fade_error = "no Kill outAnim alpha";
			break;
		}
		const int32_t trigger = total - int32_t(*f - *out_label);
		for (int32_t p = 0; p < total && push_step[i] < 0; ++p) {
			push_step[i] = removal_step(p) == trigger ? p : -1;
		}
		if (push_step[i] < 0) {
			fade_error = "no push step removes kill row " + std::to_string(i) + " at step " + std::to_string(trigger) + " of " + std::to_string(total);
			break;
		}
		expect_kill_frames_.back() = int32_t(*f);
	}
	for (size_t i = 1; i < push_step.size() && fade_error.empty(); ++i) {
		if (push_step[i] < push_step[i - 1]) {
			fade_error = "kill row " + std::to_string(i) + " would be pushed before row " + std::to_string(i - 1);
		}
	}
	if (!fade_error.empty()) {
		r_run.row(id + ".kill_fade", false, fade_error);
	}
	r_hud.set_fixture(m, pose);
	for (int32_t s = 0; s < total; ++s) {
		bool changed = false;
		if (s == pre_score_step) {
			push_score(m.messages.score_message, ui::kHudScoreKillSid, level->tdm_tuning->score_kill);
			for (const auto &[sid, points] : pre_minor) {
				push_score(m.messages.score_message, sid, points);
			}
			push_score(m.messages.score_message, ui::kHudScoreHeadshotSid, level->tdm_tuning->score_headshot);
			changed = true;
		}
		for (size_t i = 0; i < kills.size(); ++i) {
			if (push_step[i] == s) {
				push_kill(m.messages.kill_message, uint8_t(1 + 2 * i), uint8_t(2 + 2 * i), kills[i]->weapon);
				changed = true;
			}
		}
		if (s == kill_step && pop != nullptr) {
			push_kill(m.messages.local_kill_message, kLocalSlot, kLogVictimSlot, pop->weapon);
			changed = true;
		}
		if (s == popup_step) {
			push_score(m.messages.score_message, ui::kHudScoreKillSid, level->tdm_tuning->score_kill);
			changed = true;
		}
		if (changed) {
			++m.serial;
			r_hud.set_fixture(m, pose);
		}
		r_hud.step(kNativeHudMomentStep);
	}
	const size_t fading = size_t(std::count_if(expect_kill_frames_.begin(), expect_kill_frames_.end(), [](int32_t p_f) { return p_f >= 0; }));
	names_detail_ = std::to_string(kills.size()) + " kill rows (" + std::to_string(fading) + " timed into the row timeline's fade), names within " +
			std::to_string(worst) + " px of the reference rects";
}

void InRoundMoments::check_messages(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	const std::string id = p_moment.id;
	const auto texts = [](const std::vector<NativeHudMessageRow> &p_rows) {
		std::string s;
		for (const NativeHudMessageRow &r : p_rows) {
			s += (s.empty() ? "" : ", ") + r.text;
		}
		return s;
	};
	const auto same = [](const std::vector<NativeHudMessageRow> &p_rows, const std::vector<std::string> &p_want) {
		if (p_rows.size() != p_want.size()) {
			return false;
		}
		for (size_t i = 0; i < p_rows.size(); ++i) {
			if (p_rows[i].text != p_want[i]) {
				return false;
			}
		}
		return true;
	};
	const std::vector<NativeHudMessageRow> feed = native_hud_message_rows(r_hud, kKillFeedPath, ".container.msg.txt");
	bool fades_ok = true;
	std::string fades;
	char buf[96];
	for (size_t i = 0; i < expect_kill_frames_.size(); ++i) {
		if (expect_kill_frames_[i] < 0) {
			continue;
		}
		const bool ok = i < feed.size() && int32_t(feed[i].frame) == expect_kill_frames_[i];
		fades_ok = fades_ok && ok;
		std::snprintf(buf, sizeof(buf), "; row %zu frame %d alpha %.3f (want frame %d, reference alpha %.2f)", i, i < feed.size() ? int32_t(feed[i].frame) : -1,
				i < feed.size() ? double(feed[i].alpha) : -1.0, expect_kill_frames_[i], double(reference_kill_alphas_[i]));
		fades += buf;
	}
	r_run.row(id + ".kill_feed", same(feed, expect_kills_) && fades_ok, "rows '" + texts(feed) + "', " + names_detail_ + fades);
	const std::vector<NativeHudMessageRow> log = native_hud_message_rows(r_hud, kKillLogPath, ".container.msg.txt");
	r_run.row(id + ".kill_log", same(log, expect_log_), "rows '" + texts(log) + "', want " + std::to_string(expect_log_.size()));
	const std::vector<NativeHudMessageRow> score = native_hud_message_rows(r_hud, kScorePath, ".scoringMessage.txt");
	const bool score_ok = expect_score_frame_ < 0 ? score.empty() : score.size() == 1 && int32_t(score[0].frame) == expect_score_frame_;
	r_run.row(id + ".score_popup", score_ok,
			"rows '" + texts(score) + "'" + (score.empty() ? std::string() : " at frame " + std::to_string(score.back().frame)) + ", want " +
					(expect_score_frame_ < 0 ? std::string("none") : "frame " + std::to_string(expect_score_frame_)));
	if (expect_aggregator_frame_ >= 0) {
		const ui::SwfSceneNode *agg = native_hud_node_of(r_hud, kAggregatorPath);
		const char *t = native_hud_text_of(r_hud, "instance1.ScoreAggregator_01.aggHolder.scoringMessage.txt");
		const std::string text = t != nullptr ? t : "";
		r_run.row(id + ".aggregator", text == kFootageAggregate,
				"text '" + text + "', the footage shows '" + std::string(kFootageAggregate) + "'; frame " + (agg != nullptr ? std::to_string(agg->frame) : std::string("missing")) +
						", the reference state frame " + std::to_string(expect_aggregator_frame_));
	}
}

void InRoundMoments::check_moment(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	const std::string id = p_moment.id;
	const host::PresentationFrame &f = r_session.frame();
	r_run.row(id + ".in_round", f.match_valid && f.match.phase == net::MatchPhase::InRound,
			std::string("match phase ") + (f.match_valid ? std::to_string(int(f.match.phase)) : std::string("not received")) + ", want " +
					std::to_string(int(net::MatchPhase::InRound)));
	const uint16_t want_yaw = sim::quantize_yaw(native_hud_moment_view_yaw(p_moment));
	r_run.row(id + ".pose", native_hud_at_feet(f.own_curr.position, r_run.feet()) && f.own_curr.yaw == want_yaw,
			"feet " + native_hud_xyz(f.own_curr.position) + ", yaw " + std::to_string(f.own_curr.yaw) + ", want " + native_hud_xyz(r_run.feet()) + ", yaw " + std::to_string(want_yaw));
	const auto text_is = [&](const char *p_what, const char *p_path, const std::string &p_want) {
		const char *t = native_hud_text_of(r_hud, p_path);
		r_run.row(id + "." + p_what, t != nullptr && p_want == t, std::string(p_path) + " '" + (t != nullptr ? t : "missing") + "', want '" + p_want + "'");
	};
	text_is("clip", "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay", std::to_string(p_moment.clip));
	text_is("reserve", "instance1.Ammo_01.Ammo.amholder.amtxt.txtDisplay", std::to_string(p_moment.reserve));
	text_is("grenades", "instance1.Ammo_01.Ammo.grtxt.txtDisplay", std::to_string(p_moment.grenades));
	text_is("health", "instance1.Health_01.Health.healthHolder.amtxt.txtDisplay", std::to_string(p_moment.health));
	const ui::SwfSceneNode *cps = native_hud_node_of(r_hud, "instance1.Compass_01.multiplayerCompass.cps");
	r_run.row(id + ".compass", cps != nullptr && int32_t(cps->frame) == p_moment.compass_frame - 1,
			"cps frame " + (cps != nullptr ? std::to_string(cps->frame) : std::string("missing")) + ", want the reference's gotoAndStop " + std::to_string(p_moment.compass_frame) + " - 1");
	const NativeHudMinimapPose &mp = r_hud.minimap_pose();
	const float view_yaw = native_hud_moment_view_yaw(p_moment);
	const float own_offset = std::hypot(mp.x - r_run.feet()[0], mp.z - r_run.feet()[2]);
	const float yaw_offset = std::fabs(std::remainder(mp.yaw - view_yaw, float(2.0 * std::numbers::pi)));
	const int32_t state_frame = ui::hud_compass_frame(bf3::host::hud_compass_heading_deg(mp.yaw));
	r_run.row(id + ".state", mp.valid && own_offset == 0.0f && native_hud_at_feet(f.own_curr.position, r_run.feet()) && yaw_offset < 1e-5f && f.own_curr.yaw == sim::quantize_yaw(mp.yaw) && cps != nullptr &&
					int32_t(cps->frame) == state_frame,
			"minimap pose " + std::to_string(own_offset) + " m from the state feet, own soldier at " + native_hud_xyz(f.own_curr.position) + ", yaw offset " + std::to_string(yaw_offset) + " rad, own yaw " + std::to_string(f.own_curr.yaw) +
					" want " + std::to_string(sim::quantize_yaw(mp.yaw)) + ", cps frame " + (cps != nullptr ? std::to_string(cps->frame) : std::string("missing")) +
					" want " + std::to_string(state_frame) + " from the minimap yaw");
	const NativeHudPaintStats &ps = r_hud.paint_stats();
	r_run.row(id + ".engine_targets", ps.bg_panels > 0 && ps.ekg_traces > 0 && ps.minimap_maps > 0,
			"iBgDraw panels " + std::to_string(ps.bg_panels) + ", EKG traces " + std::to_string(ps.ekg_traces) + ", minimap maps " + std::to_string(ps.minimap_maps) +
					", minimap icons " + std::to_string(ps.minimap_icons));
	const std::optional<ui::MinimapLevel> map = r_hud.minimap_level();
	if (!map) {
		r_run.row(id + ".minimap", false, "no minimap level");
	} else {
		const ui::MinimapUv uv = ui::world_to_minimap_uv(ui::minimap_mapping(map->data), mp.x, mp.z);
		const uint32_t area_side = r_hud.minimap_combat_area_side();
		const float area_texel = area_side > 0 ? kMapRegistrationTexels / float(area_side) : 0.0f;
		const auto apart = [](float p_ax, float p_ay, float p_bx, float p_by) { return std::max(std::fabs(p_ax - p_bx), std::fabs(p_ay - p_by)); };
		const float state_to_area = apart(uv.u * kMapRegistrationTexels, uv.v * kMapRegistrationTexels, p_moment.area_texel_x, p_moment.area_texel_y);
		const float detail_to_area = apart(p_moment.map_texel_x, p_moment.map_texel_y, p_moment.area_texel_x, p_moment.area_texel_y);
		const float k0 = float(p_moment.compass_frame - 1);
		const float turn = std::remainder(float(p_moment.compass_next - p_moment.compass_frame), 360.0f);
		const float low = std::min(0.0f, turn) - 1.0f - kRegistrationRotationStep;
		const float high = std::max(0.0f, turn) + kRegistrationRotationStep;
		const float registered = std::remainder(native_hud_moment_registered_heading_deg(p_moment) - k0, 360.0f);
		const float shown = std::remainder(bf3::host::hud_compass_heading_deg(mp.yaw) - k0, 360.0f);
		const bool detail_ok = p_moment.detail_ambiguous ? detail_to_area > area_texel : detail_to_area <= area_texel;
		const ui::MinimapZoom want_zoom = ui::minimap_zoom(map->profile, map->profile.init_small);
		const float zoom = r_hud.client().minimap_zoom;
		r_run.row(id + ".minimap", mp.valid && r_hud.client().minimap_small == map->profile.init_small && zoom == want_zoom.zoom && want_zoom.player_is_center && zoom == 0.0f && area_texel > 0.0f && state_to_area <= area_texel && detail_ok && registered >= low && registered <= high && shown >= low && shown <= high,
				std::string(r_hud.client().minimap_small ? "small" : "large") + " map, MapZoom step " + std::to_string(want_zoom.step) + " = " + std::to_string(zoom) +
						(want_zoom.player_is_center ? ", player centred" : ", player not centred") + ", opacity " + std::to_string(map->profile.texture_opacity) +
						"; state texel " + std::to_string(state_to_area) + " and detail registration " + std::to_string(detail_to_area) + (p_moment.detail_ambiguous ? " (ambiguous)" : "") +
						" texels from the combat-area registration, bound one combat-area texel " + std::to_string(area_texel) + "; registered heading " +
						std::to_string(registered) + " and shown " + std::to_string(shown) + " deg from compass frame " + std::to_string(int32_t(k0)) + ", compass interval [" +
						std::to_string(low) + ", " + std::to_string(high) + "]");
	}
	check_messages(r_run, r_hud, p_moment);
	check_squad(r_run, r_hud, p_moment);
}

void InRoundMoments::check_squad(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	if (expect_squad_.empty()) {
		return;
	}
	const std::string id = p_moment.id;
	bool ok = true;
	std::string detail;
	for (size_t i = 0; i < expect_squad_.size(); ++i) {
		const NativeHudSquadRow &q = expect_squad_[i];
		const std::string rp = std::string(kSquadListPath) + ".squadMember" + std::to_string(i);
		const ui::SwfSceneNode *r = native_hud_node_of(r_hud, rp.c_str());
		const ui::SwfSceneNode *kit = native_hud_node_of(r_hud, (rp + ".kit").c_str());
		const ui::SwfSceneNode *booster = native_hud_node_of(r_hud, (rp + ".booster").c_str());
		const ui::SwfSceneNode *leader = native_hud_node_of(r_hud, (rp + ".leader").c_str());
		const char *txt = native_hud_text_of(r_hud, (rp + ".label.txt").c_str());
		const bool row_ok = r != nullptr && r->world_visible && kit != nullptr && int32_t(kit->frame) + 1 == q.kit && booster != nullptr && booster->visible &&
				int32_t(booster->frame) + 1 == q.booster && leader != nullptr && leader->visible == q.leader && txt != nullptr && expect_squad_names_[i] == txt;
		ok = ok && row_ok;
		detail += (i == 0 ? "" : "; ") + std::string("row ") + std::to_string(i) + " kit " + (kit != nullptr ? std::to_string(kit->frame + 1) : std::string("missing")) +
				" booster " + (booster != nullptr ? std::to_string(booster->frame + 1) : std::string("missing")) + " leader " +
				(leader != nullptr && leader->visible ? "shown" : "hidden") + (row_ok ? "" : " (want kit " + std::to_string(q.kit) + " booster " + std::to_string(q.booster) + ")");
	}
	r_run.row(id + ".squad_list", ok, detail + "; names fill the reference's masked name rects");
}

void register_native_hud_moments_in_round(NativeHudMomentGroups &r_groups) {
	r_groups.push_back(std::make_unique<InRoundMoments>());
}

}
