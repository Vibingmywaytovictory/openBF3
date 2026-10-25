#include "native_hud_moments.h"

#include "level_session.h"
#include "native_hud.h"
#include "swf_view.h"
#include "ui_assets.h"

#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/math.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/host/client_session.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
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
constexpr double kSettleStep = 1.0 / 30.0;
constexpr int32_t kRenderFrames = 3;
constexpr double kAliveTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
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
constexpr std::string_view kNameGlyphs = "WMHNEI";
constexpr const char *kKillFeedPath = "instance1.HudMessage_02";
constexpr const char *kKillLogPath = "instance1.HudMessage_00";
constexpr const char *kScorePath = "instance1.HudMessage_01";
constexpr const char *kAggregatorPath = "instance1.ScoreAggregator_01.aggHolder";
constexpr std::array<std::string_view, 2> kPreScoreMinorSids{ "ID_SCORE_AVENGER", "ID_SCORE_SAVIOUR" };
constexpr std::string_view kFootageAggregate = "240";
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr float kScanAboveSpawns = 3.0f;
constexpr float kScanBelowSpawns = 5.0f;
constexpr float kScanStep = 0.05f;
constexpr uint32_t kScanMaxFloors = 64;
constexpr float kFloorNormalY = 0.7f;
constexpr float kNearStep = 0.5f;
constexpr float kNearRadius = 8.0f;

struct MomentFloor {
	sim::Vec3 at{};
	bool clear = false;
};

std::string xyz(const sim::Vec3 &p_v) {
	return "(" + std::to_string(p_v[0]) + ", " + std::to_string(p_v[1]) + ", " + std::to_string(p_v[2]) + ")";
}

bool at_feet(const sim::Vec3 &p_at, const sim::Vec3 &p_feet) {
	const float dx = p_at[0] - p_feet[0];
	const float dz = p_at[2] - p_feet[2];
	return std::sqrt(dx * dx + dz * dz) <= kArriveXz && std::fabs(p_at[1] - p_feet[1]) <= kArriveY;
}

const char *text_of(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n->text.c_str();
		}
	}
	return nullptr;
}

const ui::SwfSceneNode *node_of(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n;
		}
	}
	return nullptr;
}

const ui::SwfScene *scene_of(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		if (v != nullptr && v->scene() != nullptr && v->scene()->find_node(p_path) != nullptr) {
			return v->scene();
		}
	}
	return nullptr;
}

float layer_scale(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		if (v != nullptr && v->scene() != nullptr && v->scene()->find_node(p_path) != nullptr) {
			return float(v->stage_fit().scale_x);
		}
	}
	return 1.0f;
}

struct MessageRow {
	float y = 0.0f;
	uint32_t frame = 0;
	float alpha = 0.0f;
	std::string text;
};

std::vector<MessageRow> message_rows(const Bf3NativeHud &p_hud, const char *p_widget, const char *p_text) {
	std::vector<MessageRow> out;
	const std::string holder = std::string(p_widget) + ".messageHolder";
	const ui::SwfScene *sc = scene_of(p_hud, holder.c_str());
	const ui::SwfSceneNode *h = sc != nullptr ? sc->find_node(holder) : nullptr;
	if (h == nullptr) {
		return out;
	}
	for (const ui::SwfNodeId id : h->children) {
		const ui::SwfSceneNode *row = sc->node(id);
		if (row == nullptr || !row->world_visible) {
			continue;
		}
		const ui::SwfSceneNode *t = sc->find_node(row->path + p_text);
		const ui::SwfSceneNode *body = sc->find_node(row->path + ".container");
		const float alpha = body != nullptr ? body->world_color.mul[3] : row->world_color.mul[3];
		out.push_back(MessageRow{ row->world.ty, row->frame, alpha, t != nullptr ? t->text : std::string() });
	}
	std::sort(out.begin(), out.end(), [](const MessageRow &p_a, const MessageRow &p_b) { return p_a.y < p_b.y; });
	return out;
}

class NameFit {
public:
	std::string open(const char *p_movie, std::initializer_list<const char *> p_linkages) {
		Bf3UiAssets *assets = Bf3UiAssets::current();
		if (assets == nullptr || assets->movies() == nullptr || assets->swf_fonts() == nullptr) {
			return "no Bf3UiAssets movies or fonts";
		}
		fonts_ = assets->swf_fonts();
		bf3::core::Result<ui::SwfScene> s = ui::SwfScene::open(*assets->movies(), p_movie);
		if (!s) {
			return std::string(p_movie) + ": " + s.error().message;
		}
		scene_ = std::make_shared<ui::SwfScene>(std::move(*s));
		const std::string root = scene_->node(scene_->root())->path;
		int32_t depth = 1;
		for (const char *linkage : p_linkages) {
			bf3::core::Result<ui::SwfNodeId> a = scene_->attach_movie(root, linkage, linkage, depth++);
			if (!a) {
				return std::string(linkage) + ": " + a.error().message;
			}
		}
		prefix_ = root.empty() ? std::string() : root + ".";
		return {};
	}

	float chain_scale(std::string_view p_field) const {
		float s = 1.0f;
		std::string path = prefix_;
		size_t at = 0;
		while (at <= p_field.size()) {
			const size_t dot = std::min(p_field.find('.', at), p_field.size());
			path += std::string(p_field.substr(at, dot - at));
			if (const ui::SwfSceneNode *n = scene_->find_node(path)) {
				s *= n->matrix.a;
			}
			path += ".";
			at = dot + 1;
		}
		return s;
	}

	float width(std::string_view p_field, std::string_view p_text) {
		const std::string path = prefix_ + std::string(p_field);
		if (!scene_->set_text(path, p_text)) {
			return 0.0f;
		}
		const ui::SwfSceneNode *n = scene_->find_node(path);
		if (n == nullptr) {
			return 0.0f;
		}
		bf3::core::level::WorkBudget budget(ui::kSwfTextBudgetUnits);
		bf3::core::Result<ui::SwfTextLayout> l = ui::swf_layout_node_text(*fonts_, *n, budget);
		return l ? l->text_width : 0.0f;
	}

	std::optional<uint32_t> label(std::string_view p_linkage, std::string_view p_label) const {
		const ui::SwfSceneNode *n = scene_->find_node(prefix_ + std::string(p_linkage));
		return n != nullptr && n->timeline != nullptr ? n->timeline->label_frame(p_label) : std::nullopt;
	}

	uint32_t frames(std::string_view p_linkage) const {
		const ui::SwfSceneNode *n = scene_->find_node(prefix_ + std::string(p_linkage));
		return n != nullptr ? n->total_frames() : 0;
	}

	std::optional<float> alpha_at(std::string_view p_linkage, std::string_view p_child, uint32_t p_frame) {
		const std::string path = prefix_ + std::string(p_linkage);
		if (!scene_->goto_frame(path, p_frame, false)) {
			return std::nullopt;
		}
		const ui::SwfSceneNode *n = scene_->find_node(path);
		const ui::SwfSceneNode *c = scene_->find_node(path + "." + std::string(p_child));
		if (n == nullptr || c == nullptr || n->frame != p_frame) {
			return std::nullopt;
		}
		return c->color.mul[3];
	}

	std::string fit(std::string_view p_field, float p_units) {
		std::vector<std::pair<float, char>> glyphs;
		for (const char c : kNameGlyphs) {
			glyphs.emplace_back(width(p_field, std::string_view(&c, 1)), c);
		}
		std::sort(glyphs.begin(), glyphs.end(), [](const auto &p_a, const auto &p_b) { return p_a.first > p_b.first; });
		std::string out;
		for (const auto &[w, c] : glyphs) {
			while (out.size() + 1 < ui::kHudNameBytes && width(p_field, out + c) <= p_units) {
				out += c;
			}
		}
		return out;
	}

private:
	std::shared_ptr<ui::SwfScene> scene_;
	const ui::SwfFontSet *fonts_ = nullptr;
	std::string prefix_;
};

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

NativeHudMinimapPose native_hud_moment_pose(const sim::Vec3 &p_own, float p_view_yaw) {
	NativeHudMinimapPose p;
	p.x = p_own[0];
	p.z = p_own[2];
	p.yaw = p_view_yaw;
	p.valid = true;
	return p;
}

namespace {

std::optional<MomentFloor> moment_floor(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns, float p_x, float p_z) {
	const sim::SpawnPoint *nearest = nullptr;
	float best = 0.0f;
	float low = std::numeric_limits<float>::infinity();
	float high = -std::numeric_limits<float>::infinity();
	for (const sim::SpawnPoint &sp : p_spawns) {
		const float dx = sp.position[0] - p_x;
		const float dz = sp.position[2] - p_z;
		const float d = dx * dx + dz * dz;
		if (nearest == nullptr || d < best) {
			nearest = &sp;
			best = d;
		}
		low = std::min(low, sp.position[1]);
		high = std::max(high, sp.position[1]);
	}
	if (nearest == nullptr) {
		return std::nullopt;
	}
	const float bottom = low - kScanBelowSpawns;
	const auto closer = [&](const sim::Vec3 &p_floor, const std::optional<sim::Vec3> &p_best) {
		return !p_best || std::fabs(p_floor[1] - nearest->position[1]) < std::fabs((*p_best)[1] - nearest->position[1]);
	};
	std::optional<sim::Vec3> clear_floor;
	std::optional<sim::Vec3> any_floor;
	float y = high + kScanAboveSpawns;
	for (uint32_t n = 0; n < kScanMaxFloors && y > bottom; ++n) {
		sim::StaticHit hit;
		if (!p_world.ground_below(sim::Vec3{ p_x, y, p_z }, y - bottom, sim::query_mask::kMovement, &hit)) {
			break;
		}
		y = hit.position[1] - kScanStep;
		if (hit.normal[1] < kFloorNormalY) {
			continue;
		}
		const sim::Vec3 capsule_base{ hit.position[0], hit.position[1] + p_motor.tuning.character_padding, hit.position[2] };
		const bool clear = !p_world.overlap_capsule(capsule_base, p_motor.radius, p_motor.poses[size_t(sim::Pose::Stand)].height, sim::query_mask::kMovement,
				p_motor.tuning.pose_change_max_penetration);
		if (clear && closer(hit.position, clear_floor)) {
			clear_floor = hit.position;
		}
		if (closer(hit.position, any_floor)) {
			any_floor = hit.position;
		}
	}
	if (clear_floor) {
		return MomentFloor{ *clear_floor, true };
	}
	if (any_floor) {
		return MomentFloor{ *any_floor, false };
	}
	return std::nullopt;
}

}

std::optional<sim::Vec3> native_hud_moment_feet(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns, float p_x,
		float p_z) {
	const std::optional<MomentFloor> f = moment_floor(p_world, p_motor, p_spawns, p_x, p_z);
	return f ? std::optional<sim::Vec3>(f->at) : std::nullopt;
}

std::optional<sim::Vec3> native_hud_moment_floor_near(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns,
		float p_x, float p_z, float p_radius) {
	std::optional<sim::Vec3> fallback;
	for (float r = 0.0f; r <= p_radius + 1e-3f; r += kNearStep) {
		const int32_t around = r > 0.0f ? std::max(8, int32_t(std::ceil(2.0f * float(std::numbers::pi) * r / kNearStep))) : 1;
		for (int32_t i = 0; i < around; ++i) {
			const float a = 2.0f * float(std::numbers::pi) * float(i) / float(around);
			const std::optional<MomentFloor> f = moment_floor(p_world, p_motor, p_spawns, p_x + r * std::cos(a), p_z + r * std::sin(a));
			if (f && f->clear) {
				return f->at;
			}
			if (f && !fallback) {
				fallback = f->at;
			}
		}
	}
	return fallback;
}

NativeHudMomentRun::NativeHudMomentRun(std::string p_out_dir, std::string_view p_ids) : out_dir_(std::move(p_out_dir)) {
	std::vector<std::string_view> ids;
	for (size_t at = 0; at < p_ids.size();) {
		const size_t end = std::min(p_ids.find(',', at), p_ids.size());
		if (end > at) {
			ids.push_back(p_ids.substr(at, end - at));
		}
		at = end + 1;
	}
	for (const NativeHudMoment &m : kMoments) {
		if (ids.empty()) {
			moments_.push_back(&m);
		}
	}
	for (const std::string_view id : ids) {
		for (const NativeHudMoment &m : kMoments) {
			if (id == m.id) {
				moments_.push_back(&m);
			}
		}
	}
	for (const char *sub : { "captures", "layers" }) {
		DirAccess::make_dir_recursive_absolute(String::utf8((out_dir_ + "/" + sub).c_str()));
	}
}

void NativeHudMomentRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(String::utf8(("NATIVE HUD ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void NativeHudMomentRun::hold(Bf3LevelSession &r_session, const NativeHudMoment &p_moment) {
	host::ClientSession *client = r_session.client();
	if (client == nullptr) {
		return;
	}
	const float yaw = native_hud_moment_view_yaw(p_moment);
	r_session.mapper().script_view(yaw, 0.0f);
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = feet_;
	q.yaw = sim::quantize_yaw(yaw);
	client->send_debug(q);
}

void NativeHudMomentRun::settle(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment, ui::HudModel p_model) {
	const std::string id = p_moment.id;
	const NativeHudMinimapPose pose = native_hud_moment_pose(feet_, native_hud_moment_view_yaw(p_moment));
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
	NameFit fit;
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
		row(id + ".messages", false, error);
		r_hud.set_fixture(p_model, pose);
		for (double t = 0.0; t < kSettleSeconds; t += kSettleStep) {
			r_hud.step(kSettleStep);
		}
		return;
	}
	const ui::HudMessageCompData &d = comp->data();
	const size_t queue = size_t(std::max(d.of(ui::HudMessageSource::Kill).queue_size, 0));
	const double flush = double(d.score_aggregate_time) + kFlushMarginSeconds;
	const double lead = pop != nullptr ? kPreScoreLeadSeconds : kKillLeadSeconds;
	const int32_t total = int32_t(std::lround((flush + lead) / kSettleStep));
	const auto step_before = [&](double p_seconds) { return total - int32_t(std::lround(p_seconds / kSettleStep)); };

	const ui::SwfSceneNode *feed = node_of(r_hud, kKillFeedPath);
	const ui::SwfSceneNode *log = node_of(r_hud, kKillLogPath);
	name_scale_ = layer_scale(r_hud, kKillLogPath);
	const float feed_scale = (feed != nullptr ? feed->world.a : 1.0f) * layer_scale(r_hud, kKillFeedPath);
	const float log_scale = (log != nullptr ? log->world.a : 1.0f) * name_scale_;
	float worst = 0.0f;
	const auto fitted_in = [&](NameFit &r_fit, std::string_view p_field, float p_scale, float p_px) {
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
		NameFit squad_fit;
		const std::string squad_error = squad_fit.open("ui/assets/squadlist", { "SquadList" });
		if (!squad_error.empty()) {
			row(id + ".squad_list", false, squad_error);
		} else {
			const ui::SwfSceneNode *sl = node_of(r_hud, kSquadListPath);
			const float sl_scale = (sl != nullptr ? sl->world.a : 1.0f) * layer_scale(r_hud, kSquadListPath);
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
		c += kSettleStep;
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
		row(id + ".kill_fade", false, fade_error);
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
		r_hud.step(kSettleStep);
	}
	const size_t fading = size_t(std::count_if(expect_kill_frames_.begin(), expect_kill_frames_.end(), [](int32_t p_f) { return p_f >= 0; }));
	names_detail_ = std::to_string(kills.size()) + " kill rows (" + std::to_string(fading) + " timed into the row timeline's fade), names within " +
			std::to_string(worst) + " px of the reference rects";
}

void NativeHudMomentRun::check_messages(Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	const std::string id = p_moment.id;
	const auto texts = [](const std::vector<MessageRow> &p_rows) {
		std::string s;
		for (const MessageRow &r : p_rows) {
			s += (s.empty() ? "" : ", ") + r.text;
		}
		return s;
	};
	const auto same = [](const std::vector<MessageRow> &p_rows, const std::vector<std::string> &p_want) {
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
	const std::vector<MessageRow> feed = message_rows(r_hud, kKillFeedPath, ".container.msg.txt");
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
	row(id + ".kill_feed", same(feed, expect_kills_) && fades_ok, "rows '" + texts(feed) + "', " + names_detail_ + fades);
	const std::vector<MessageRow> log = message_rows(r_hud, kKillLogPath, ".container.msg.txt");
	row(id + ".kill_log", same(log, expect_log_), "rows '" + texts(log) + "', want " + std::to_string(expect_log_.size()));
	const std::vector<MessageRow> score = message_rows(r_hud, kScorePath, ".scoringMessage.txt");
	const bool score_ok = expect_score_frame_ < 0 ? score.empty() : score.size() == 1 && int32_t(score[0].frame) == expect_score_frame_;
	row(id + ".score_popup", score_ok,
			"rows '" + texts(score) + "'" + (score.empty() ? std::string() : " at frame " + std::to_string(score.back().frame)) + ", want " +
					(expect_score_frame_ < 0 ? std::string("none") : "frame " + std::to_string(expect_score_frame_)));
	if (expect_aggregator_frame_ >= 0) {
		const ui::SwfSceneNode *agg = node_of(r_hud, kAggregatorPath);
		const char *t = text_of(r_hud, "instance1.ScoreAggregator_01.aggHolder.scoringMessage.txt");
		const std::string text = t != nullptr ? t : "";
		row(id + ".aggregator", text == kFootageAggregate,
				"text '" + text + "', the footage shows '" + std::string(kFootageAggregate) + "'; frame " + (agg != nullptr ? std::to_string(agg->frame) : std::string("missing")) +
						", the reference state frame " + std::to_string(expect_aggregator_frame_));
	}
}

void NativeHudMomentRun::check(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	const std::string id = p_moment.id;
	const host::PresentationFrame &f = r_session.frame();
	row(id + ".in_round", f.match_valid && f.match.phase == net::MatchPhase::InRound,
			std::string("match phase ") + (f.match_valid ? std::to_string(int(f.match.phase)) : std::string("not received")) + ", want " +
					std::to_string(int(net::MatchPhase::InRound)));
	const uint16_t want_yaw = sim::quantize_yaw(native_hud_moment_view_yaw(p_moment));
	row(id + ".pose", at_feet(f.own_curr.position, feet_) && f.own_curr.yaw == want_yaw,
			"feet " + xyz(f.own_curr.position) + ", yaw " + std::to_string(f.own_curr.yaw) + ", want " + xyz(feet_) + ", yaw " + std::to_string(want_yaw));
	const auto text_is = [&](const char *p_what, const char *p_path, const std::string &p_want) {
		const char *t = text_of(r_hud, p_path);
		row(id + "." + p_what, t != nullptr && p_want == t, std::string(p_path) + " '" + (t != nullptr ? t : "missing") + "', want '" + p_want + "'");
	};
	text_is("clip", "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay", std::to_string(p_moment.clip));
	text_is("reserve", "instance1.Ammo_01.Ammo.amholder.amtxt.txtDisplay", std::to_string(p_moment.reserve));
	text_is("grenades", "instance1.Ammo_01.Ammo.grtxt.txtDisplay", std::to_string(p_moment.grenades));
	text_is("health", "instance1.Health_01.Health.healthHolder.amtxt.txtDisplay", std::to_string(p_moment.health));
	const ui::SwfSceneNode *cps = node_of(r_hud, "instance1.Compass_01.multiplayerCompass.cps");
	row(id + ".compass", cps != nullptr && int32_t(cps->frame) == p_moment.compass_frame - 1,
			"cps frame " + (cps != nullptr ? std::to_string(cps->frame) : std::string("missing")) + ", want the reference's gotoAndStop " + std::to_string(p_moment.compass_frame) + " - 1");
	const NativeHudMinimapPose &mp = r_hud.minimap_pose();
	const float view_yaw = native_hud_moment_view_yaw(p_moment);
	const float own_offset = std::hypot(mp.x - feet_[0], mp.z - feet_[2]);
	const float yaw_offset = std::fabs(std::remainder(mp.yaw - view_yaw, float(2.0 * std::numbers::pi)));
	const int32_t state_frame = ui::hud_compass_frame(bf3::host::hud_compass_heading_deg(mp.yaw));
	row(id + ".state", mp.valid && own_offset == 0.0f && at_feet(f.own_curr.position, feet_) && yaw_offset < 1e-5f && f.own_curr.yaw == sim::quantize_yaw(mp.yaw) && cps != nullptr &&
					int32_t(cps->frame) == state_frame,
			"minimap pose " + std::to_string(own_offset) + " m from the state feet, own soldier at " + xyz(f.own_curr.position) + ", yaw offset " + std::to_string(yaw_offset) + " rad, own yaw " + std::to_string(f.own_curr.yaw) +
					" want " + std::to_string(sim::quantize_yaw(mp.yaw)) + ", cps frame " + (cps != nullptr ? std::to_string(cps->frame) : std::string("missing")) +
					" want " + std::to_string(state_frame) + " from the minimap yaw");
	const NativeHudPaintStats &ps = r_hud.paint_stats();
	row(id + ".engine_targets", ps.bg_panels > 0 && ps.ekg_traces > 0 && ps.minimap_maps > 0,
			"iBgDraw panels " + std::to_string(ps.bg_panels) + ", EKG traces " + std::to_string(ps.ekg_traces) + ", minimap maps " + std::to_string(ps.minimap_maps) +
					", minimap icons " + std::to_string(ps.minimap_icons));
	const std::optional<ui::MinimapLevel> map = r_hud.minimap_level();
	if (!map) {
		row(id + ".minimap", false, "no minimap level");
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
		row(id + ".minimap", mp.valid && r_hud.client().minimap_small == map->profile.init_small && zoom == want_zoom.zoom && want_zoom.player_is_center && zoom == 0.0f && area_texel > 0.0f && state_to_area <= area_texel && detail_ok && registered >= low && registered <= high && shown >= low && shown <= high,
				std::string(r_hud.client().minimap_small ? "small" : "large") + " map, MapZoom step " + std::to_string(want_zoom.step) + " = " + std::to_string(zoom) +
						(want_zoom.player_is_center ? ", player centred" : ", player not centred") + ", opacity " + std::to_string(map->profile.texture_opacity) +
						"; state texel " + std::to_string(state_to_area) + " and detail registration " + std::to_string(detail_to_area) + (p_moment.detail_ambiguous ? " (ambiguous)" : "") +
						" texels from the combat-area registration, bound one combat-area texel " + std::to_string(area_texel) + "; registered heading " +
						std::to_string(registered) + " and shown " + std::to_string(shown) + " deg from compass frame " + std::to_string(int32_t(k0)) + ", compass interval [" +
						std::to_string(low) + ", " + std::to_string(high) + "]");
	}
	check_messages(r_hud, p_moment);
	check_squad(r_hud, p_moment);
}

void NativeHudMomentRun::check_squad(Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	if (expect_squad_.empty()) {
		return;
	}
	const std::string id = p_moment.id;
	bool ok = true;
	std::string detail;
	for (size_t i = 0; i < expect_squad_.size(); ++i) {
		const NativeHudSquadRow &q = expect_squad_[i];
		const std::string rp = std::string(kSquadListPath) + ".squadMember" + std::to_string(i);
		const ui::SwfSceneNode *r = node_of(r_hud, rp.c_str());
		const ui::SwfSceneNode *kit = node_of(r_hud, (rp + ".kit").c_str());
		const ui::SwfSceneNode *booster = node_of(r_hud, (rp + ".booster").c_str());
		const ui::SwfSceneNode *leader = node_of(r_hud, (rp + ".leader").c_str());
		const char *txt = text_of(r_hud, (rp + ".label.txt").c_str());
		const bool row_ok = r != nullptr && r->world_visible && kit != nullptr && int32_t(kit->frame) + 1 == q.kit && booster != nullptr && booster->visible &&
				int32_t(booster->frame) + 1 == q.booster && leader != nullptr && leader->visible == q.leader && txt != nullptr && expect_squad_names_[i] == txt;
		ok = ok && row_ok;
		detail += (i == 0 ? "" : "; ") + std::string("row ") + std::to_string(i) + " kit " + (kit != nullptr ? std::to_string(kit->frame + 1) : std::string("missing")) +
				" booster " + (booster != nullptr ? std::to_string(booster->frame + 1) : std::string("missing")) + " leader " +
				(leader != nullptr && leader->visible ? "shown" : "hidden") + (row_ok ? "" : " (want kit " + std::to_string(q.kit) + " booster " + std::to_string(q.booster) + ")");
	}
	row(id + ".squad_list", ok, detail + "; names fill the reference's masked name rects");
}

bool NativeHudMomentRun::step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, double p_delta) {
	switch (phase_) {
		case Phase::WaitAlive: {
			waited_ += p_delta;
			if (!r_hud.is_ready()) {
				return false;
			}
			if (r_hud.has_failed() || moments_.empty()) {
				row("built", false, r_hud.has_failed() ? r_hud.error() : std::string("no moment selected"));
				phase_ = Phase::Done;
				return true;
			}
			const host::PresentationFrame &f = r_session.frame();
			const bool alive = r_hud.model().soldier.is_alive;
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			const bool minimap = r_hud.minimap_ready();
			if (!alive || !in_round || !minimap) {
				if (waited_ > kAliveTimeout) {
					if (!alive || !in_round) {
						row("alive_in_round", false,
								"after " + std::to_string(kAliveTimeout) + " s: own soldier " + (alive ? "alive" : "not spawned") + ", round " +
										(in_round ? "in round" : "not in round"));
					} else {
						row("minimap_ready", false, "after " + std::to_string(kAliveTimeout) + " s: the minimap source has no satellite texture to paint");
					}
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			r_hud.set_manual(true);
			InputMapper &mapper = r_session.mapper();
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			index_ = 0;
			phase_ = Phase::Place;
			return false;
		}
		case Phase::Place: {
			const NativeHudMoment &m = *moments_[index_];
			host::ClientSession *client = r_session.client();
			const sim::CollisionWorld *world = r_session.collision_world();
			const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
			const std::optional<ui::MinimapLevel> map = r_hud.minimap_level();
			if (!map) {
				row(std::string(m.id) + ".place", false, "no minimap level to place the registration texel");
				phase_ = Phase::Done;
				return true;
			}
			const std::array<float, 2> at_xz = native_hud_moment_world_xz(m, *map);
			const std::optional<sim::Vec3> feet =
					world != nullptr && level != nullptr ? native_hud_moment_floor_near(*world, level->motor, level->spawns, at_xz[0], at_xz[1], kNearRadius) : std::nullopt;
			if (m.detail_ambiguous) {
				UtilityFunctions::print(String::utf8((std::string("NATIVE HUD FLAG ") + m.id + " the detail registration disagrees with the combat-area registration; the state uses the combat-area one").c_str()));
			}
			if (client == nullptr) {
				row(std::string(m.id) + ".place", false, "no client");
				phase_ = Phase::Done;
				return true;
			}
			if (!feet) {
				row(std::string(m.id) + ".place", false, "no floor under (" + std::to_string(at_xz[0]) + ", " + std::to_string(at_xz[1]) + ")");
				feet_ = r_session.frame().own_curr.position;
				phase_ = Phase::Settle;
				return false;
			}
			feet_ = *feet;
			const float off = std::hypot(feet_[0] - at_xz[0], feet_[2] - at_xz[1]);
			if (off > kArriveXz) {
				UtilityFunctions::print(String::utf8((std::string("NATIVE HUD FLAG ") + m.id + " no clear standing floor at the reference xz; the nearest is " + std::to_string(off) + " m away at " + xyz(feet_)).c_str()));
			}
			hold(r_session, m);
			arriving_ = 0.0;
			phase_ = Phase::Arrive;
			return false;
		}
		case Phase::Arrive: {
			const NativeHudMoment &m = *moments_[index_];
			arriving_ += p_delta;
			hold(r_session, m);
			const sim::Vec3 &at = r_session.frame().own_curr.position;
			if (!at_feet(at, feet_)) {
				if (arriving_ > kArriveTimeout) {
					row(std::string(m.id) + ".place", false, "the own soldier is at " + xyz(at) + " " + std::to_string(kArriveTimeout) + " s after the teleport to " + xyz(feet_));
					phase_ = Phase::Settle;
					return false;
				}
				return false;
			}
			phase_ = Phase::Settle;
			return false;
		}
		case Phase::Settle: {
			const NativeHudMoment &m = *moments_[index_];
			settle(r_session, r_hud, m, native_hud_moment_model(m, r_hud.model(), r_hud.readout().crosshair_base_offset));
			r_hud.set_visible(true);
			frames_ = 0;
			phase_ = Phase::Render;
			return false;
		}
		case Phase::Render:
			hold(r_session, *moments_[index_]);
			r_hud.set_visible(true);
			if (++frames_ < kRenderFrames) {
				return false;
			}
			phase_ = Phase::Capture;
			return false;
		case Phase::Capture: {
			composite_ = r_hud.get_viewport() != nullptr ? r_hud.get_viewport()->get_texture()->get_image() : Ref<Image>();
			colour_ = r_hud.capture_views();
			r_hud.set_additive_coverage(true);
			hold(r_session, *moments_[index_]);
			frames_ = 0;
			phase_ = Phase::Coverage;
			return false;
		}
		case Phase::Coverage: {
			const NativeHudMoment &m = *moments_[index_];
			hold(r_session, m);
			if (++frames_ < kRenderFrames) {
				return false;
			}
			const std::array<Ref<Image>, kNativeHudViews> coverage = r_hud.capture_views();
			r_hud.set_additive_coverage(false);
			Ref<Image> composite = composite_;
			Ref<Image> layer = Bf3NativeHud::capture_layer(colour_, coverage);
			Ref<Image> trans = swf_view_transmittance(colour_);
			composite_ = Ref<Image>();
			colour_ = {};
			bool ok = composite.is_valid() && layer.is_valid() && trans.is_valid();
			std::string detail;
			if (ok) {
				composite->convert(Image::FORMAT_RGB8);
				const std::string cp = out_dir_ + "/captures/" + m.id + ".png";
				const std::string lp = out_dir_ + "/layers/" + m.id + ".png";
				const std::string tp = out_dir_ + "/layers/" + m.id + "_trans.png";
				ok = composite->save_png(String::utf8(cp.c_str())) == OK && layer->save_png(String::utf8(lp.c_str())) == OK && trans->save_png(String::utf8(tp.c_str())) == OK;
				detail = std::to_string(composite->get_width()) + "x" + std::to_string(composite->get_height()) + " composite, " +
						std::to_string(layer->get_width()) + "x" + std::to_string(layer->get_height()) + " HUD layer, " + std::to_string(trans->get_width()) + "x" +
						std::to_string(trans->get_height()) + " transmittance -> " + cp;
				ok = ok && composite->get_width() == 1280 && composite->get_height() == 720 && layer->get_width() == 1280 && layer->get_height() == 720 &&
						trans->get_width() == 1280 && trans->get_height() == 720;
			} else {
				detail = "no viewport image, HUD layer or transmittance";
			}
			row(std::string(m.id) + ".capture", ok, detail);
			captured_ += ok ? 1 : 0;
			if (ok) {
				const PackedByteArray bytes = layer->get_data() + trans->get_data();
				const auto [first, fresh] = first_capture_.try_emplace(m.id, bytes);
				if (!fresh) {
					const int64_t n = std::min(first->second.size(), bytes.size());
					int64_t differ = std::max(first->second.size(), bytes.size()) - n;
					for (int64_t i = 0; i < n; ++i) {
						differ += first->second[i] != bytes[i] ? 1 : 0;
					}
					row(std::string(m.id) + ".repeat", differ == 0,
							std::to_string(differ) + " of " + std::to_string(bytes.size()) + " HUD layer and transmittance bytes differ from the first capture of " + m.id);
				}
			}
			if (const ui::SwfScene *ls = name_scale_ > 0.0f && ok ? scene_of(r_hud, kKillLogPath) : nullptr) {
				const float stage_px = name_scale_ * ls->stage().width();
				row(std::string(m.id) + ".name_scale", std::fabs(stage_px - float(layer->get_width())) < 0.5f,
						"names fitted at " + std::to_string(name_scale_) + " layer px per stage unit: stage " + std::to_string(ls->stage().width()) + " -> " +
								std::to_string(stage_px) + " px, the HUD layer is " + std::to_string(layer->get_width()) + " px wide");
			}
			name_scale_ = 0.0f;
			check(r_session, r_hud, m);
			if (++index_ >= moments_.size()) {
				phase_ = Phase::Done;
				return true;
			}
			phase_ = Phase::Place;
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
