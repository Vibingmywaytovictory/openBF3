#include <doctest/doctest.h>

#include "bf3/anim/pose.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/loadout.h"
#include "bf3/render/viewmodel_set.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace bf3;

namespace {

constexpr const char *kLevel = "Win32/Levels/MP_Subway/MP_Subway";
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr int kSettleFrames = 60;
constexpr double kHeldMetres = 0.001;

struct Gait {
	const char *name;
	float speed;
	bool sprinting;
};
constexpr Gait kGaits[] = { { "walking", 1.0f, false }, { "running", 4.0f, false }, { "sprinting", 6.5f, true } };

struct Action {
	const char *name;
	bool reload;
	bool empty;
};
constexpr Action kActions[] = { { "idle", false, false }, { "reload", true, false }, { "empty reload", true, true } };

constexpr const char *kHands[] = { "LeftHand", "RightHand" };

struct Weapon {
	uint8_t team = 1;
	std::string unlock;
	std::string error;
	std::vector<std::string> lines;
	double worst = 0.0;
	std::string worst_case;
};

struct Drift {
	std::array<double, 2> worst{};
	std::array<int, 2> out_of_reach{};
};

Drift hand_drift(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item, const Gait &p_gait,
		const Action &p_action) {
	const anim::ViewmodelTuning tuning;
	const std::array<anim::VmClipRef, anim::kVmClips> refs = render::viewmodel_clip_refs(p_item);
	anim::ViewmodelAnimator moving(p_set.rig, refs, tuning);
	anim::ViewmodelAnimator plain(p_set.rig, refs, tuning);
	moving.set_locomotion(render::viewmodel_locomotion(p_item));
	const int32_t root = p_set.rig.find("Wep_Root");
	const std::array<int32_t, 2> hands{ p_set.rig.find(kHands[0]), p_set.rig.find(kHands[1]) };
	const std::array<int32_t, 2> shoulders{ p_set.rig.find("LeftArm"), p_set.rig.find("RightArm") };
	const std::array<int32_t, 2> elbows{ p_set.rig.find("LeftForeArm"), p_set.rig.find("RightForeArm") };
	const std::array<int32_t, 2> effectors{ p_set.rig.find("IK_Joint_LeftHand"), p_set.rig.find("IK_Joint_RightHand") };
	const anim::VmClipRef &reload = refs[size_t(p_action.empty ? anim::VmClip::ReloadEmpty : anim::VmClip::Reload)];
	const float reload_seconds =
			reload.seconds > 0.0f ? reload.seconds : (reload.tracks ? reload.tracks->duration_seconds() : 0.0f);
	const int frames = kSettleFrames + int((p_action.reload ? reload_seconds : 1.0f) / float(kFrameSeconds)) + 12;
	anim::ViewmodelFrameInput in;
	in.speed = p_gait.speed;
	in.sprinting = p_gait.sprinting;
	anim::LocalPose moving_pose, plain_pose;
	anim::ViewmodelOutput moving_out, plain_out;
	std::vector<anim::Xform> moving_model, plain_model;
	Drift drift;
	for (int f = 0; f < frames; ++f) {
		in.time_s += kFrameSeconds;
		in.dt = float(kFrameSeconds);
		std::vector<anim::VmCue> cues;
		if (p_action.reload && f == kSettleFrames) {
			cues.push_back(anim::VmCue{ anim::VmCue::Kind::ReloadStarted, in.time_s, p_action.empty });
		}
		moving.step(in, cues, moving_pose, moving_out);
		plain.step(in, cues, plain_pose, plain_out);
		if (f < kSettleFrames) {
			continue;
		}
		anim::model_pose(p_set.rig, moving_pose, moving_model);
		anim::model_pose(p_set.rig, plain_pose, plain_model);
		for (size_t h = 0; h < hands.size(); ++h) {
			const auto out_of_reach = [&](const std::vector<anim::Xform> &p_model) {
				const auto at = [&](int32_t p_joint) { return p_model[size_t(p_joint)].t; };
				const double arm_length =
						anim::vec_dist(at(shoulders[h]), at(elbows[h])) + anim::vec_dist(at(elbows[h]), at(hands[h]));
				return anim::vec_dist(at(shoulders[h]), at(effectors[h])) > arm_length - kHeldMetres;
			};
			if (out_of_reach(plain_model) || out_of_reach(moving_model)) {
				++drift.out_of_reach[h];
				continue;
			}
			const anim::Vec3 a =
					anim::xform_mul(anim::xform_inverse(moving_model[size_t(root)]), moving_model[size_t(hands[h])]).t;
			const anim::Vec3 b =
					anim::xform_mul(anim::xform_inverse(plain_model[size_t(root)]), plain_model[size_t(hands[h])]).t;
			drift.worst[h] = std::max(drift.worst[h], anim::vec_dist(a, b));
		}
	}
	return drift;
}

}

TEST_SUITE("render-viewmodel-reload-moving") {
	TEST_CASE("1p reload while walking, running and sprinting: each kit's weapons stay in the hands that hold them "
			  "without the "
			  "locomotion deltas") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		if (!install.ok()) {
			return;
		}
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		if (!gunplay.ok()) {
			return;
		}
		const host::LoadoutCatalog &catalog = (*gunplay)->loadouts;
		std::vector<Weapon> weapons;
		std::set<std::string> seen;
		for (uint8_t team = 1; team <= host::kKitTeams; ++team) {
			for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
				const host::LoadoutKit *k = catalog.kit(team, kit);
				REQUIRE(k != nullptr);
				if (k == nullptr) {
					return;
				}
				for (size_t slot : { size_t(0), size_t(1) }) {
					const host::LoadoutItem *item = k->find(slot, k->defaults[slot]);
					REQUIRE(item != nullptr);
					if (item == nullptr) {
						return;
					}
					if (seen.insert(std::to_string(team) + item->unlock).second) {
						weapons.push_back(Weapon{ team, item->unlock, {}, {}, 0.0, {} });
					}
				}
			}
		}
		REQUIRE(weapons.size() >= 8);
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		if (!sources.ok()) {
			return;
		}
		core::parallel_for(weapons.size(), [&](size_t i) {
			Weapon &w = weapons[i];
			render::ViewmodelSetSpec spec =
					render::team_viewmodel_items(w.team, std::span<const std::string>(&w.unlock, 1));
			spec.superbundle = kLevel;
			core::Result<std::unique_ptr<render::ViewmodelSet>> set =
					render::load_viewmodel_set(**sources, spec, nullptr);
			if (!set) {
				w.error = set.error().message;
				return;
			}
			if ((*set)->items.size() != 1 || !(*set)->items[0].locomotion) {
				w.error = "no item with locomotion deltas";
				return;
			}
			for (const char *joint : { "Wep_Root", "LeftArm", "LeftForeArm", "LeftHand", "IK_Joint_LeftHand",
						 "RightArm", "RightForeArm", "RightHand", "IK_Joint_RightHand" }) {
				if ((*set)->rig.find(joint) < 0) {
					w.error = std::string("rig without ") + joint;
					return;
				}
			}
			const render::ViewmodelItem &item = (*set)->items[0];
			for (const Gait &g : kGaits) {
				for (const Action &a : kActions) {
					const Drift d = hand_drift(**set, item, g, a);
					char line[240];
					std::snprintf(line, sizeof(line),
							"%s %s: LeftHand %.1f mm, RightHand %.1f mm; frames with the effector out of the arm's "
							"reach: "
							"%d left, %d right",
							a.name, g.name, d.worst[0] * 1000.0, d.worst[1] * 1000.0, d.out_of_reach[0],
							d.out_of_reach[1]);
					w.lines.push_back(line);
					if (std::max(d.worst[0], d.worst[1]) > w.worst) {
						w.worst = std::max(d.worst[0], d.worst[1]);
						w.worst_case = line;
					}
				}
			}
		});
		std::vector<std::string> drifting;
		for (const Weapon &w : weapons) {
			REQUIRE_MESSAGE(w.error.empty(), w.unlock, ": ", w.error);
			for (const std::string &l : w.lines) {
				std::printf("render.viewmodel_reload_moving team %u %s %s\n", unsigned(w.team), w.unlock.c_str(),
						l.c_str());
			}
			if (w.worst > kHeldMetres) {
				drifting.push_back(w.unlock + " " + w.worst_case);
			}
		}
		MESSAGE(weapons.size(), " weapons, ", std::size(kGaits), " gaits, ", std::size(kActions), " actions; ",
				drifting.size(), " with a hand more than ", kHeldMetres * 1000.0, " mm off its place on the weapon");
		for (const std::string &d : drifting) {
			MESSAGE("drift: ", d);
		}
		CHECK(drifting.empty());
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}
