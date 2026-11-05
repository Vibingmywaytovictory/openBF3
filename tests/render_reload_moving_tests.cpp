#include <doctest/doctest.h>

#include "bf3/anim/hand_reach.h"
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
constexpr double kBoltRootMetres = 0.1;

struct Gait {
	const char *name;
	float speed;
	bool sprinting;
};
constexpr Gait kGaits[] = { { "standing", 0.0f, false }, { "walking", 1.0f, false }, { "running", 4.0f, false },
	{ "sprinting", 6.5f, true } };

struct Action {
	const char *name;
	bool reload;
	bool empty;
	bool fire;
};
constexpr Action kActions[] = { { "idle", false, false, false }, { "reload", true, false, false },
	{ "empty reload", true, true, false }, { "fire", false, false, true } };
constexpr double kFireSeconds = 1.5;

constexpr const char *kHands[] = { "LeftHand", "RightHand" };

struct Weapon {
	uint8_t team = 1;
	std::string unlock;
	std::string error;
	std::vector<std::string> lines;
	double worst = 0.0;
	std::string worst_case;
	std::string bolt;
	double bolt_root = 0.0;
};

double bolt_root_travel(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item) {
	const render::ViewmodelClip &idle = p_item.clips[size_t(render::ViewmodelClipRole::Idle)];
	const render::ViewmodelClip &bolt = p_item.clips[size_t(render::ViewmodelClipRole::Bolt)];
	const int32_t root = p_set.rig.find("Wep_Root");
	core::ant::ClipPose sample;
	anim::LocalPose base = anim::rest_pose(p_set.rig);
	if (idle.source == render::ViewmodelClipSource::Clip && core::ant::sample_clip(idle.clip.clip, 0.0f, sample)) {
		anim::apply_clip_pose(idle.tracks, sample, base);
	}
	std::vector<anim::Xform> model;
	anim::model_pose(p_set.rig, base, model);
	const anim::Vec3 at_idle = model[size_t(root)].t;
	double travel = 0.0;
	for (uint32_t f = 0; f <= bolt.tracks.end_frame; ++f) {
		anim::LocalPose pose = base;
		if (core::ant::sample_clip(bolt.clip.clip, float(f), sample)) {
			anim::apply_clip_pose(bolt.tracks, sample, pose);
		}
		anim::model_pose(p_set.rig, pose, model);
		travel = std::max(travel, anim::vec_dist(model[size_t(root)].t, at_idle));
	}
	return travel;
}

struct Drift {
	std::array<double, 2> worst{};
	std::array<int, 2> straight{};
};

struct BagThrow {
	size_t reach_tags = 0;
	int window_frames = 0;
	double window_off_free = 0.0, held_off = 0.0;
};

BagThrow bag_throw_while_running(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item) {
	BagThrow out;
	const std::array<anim::VmClipRef, anim::kVmClips> refs = render::viewmodel_clip_refs(p_item);
	const anim::VmClipRef &fire = refs[size_t(anim::VmClip::Fire)];
	out.reach_tags = fire.reach.size();
	if (fire.reach.empty() || fire.tracks == nullptr) {
		return out;
	}
	anim::ViewmodelAnimator reaching(p_set.rig, refs, anim::ViewmodelTuning{});
	anim::ViewmodelAnimator free(p_set.rig, refs, anim::ViewmodelTuning{});
	anim::ViewmodelAnimator plain(p_set.rig, refs, anim::ViewmodelTuning{});
	reaching.set_locomotion(render::viewmodel_locomotion(p_item));
	free.set_locomotion(render::viewmodel_locomotion(p_item));
	reaching.set_hand_reach(p_set.hand_reach);
	const int32_t root = p_set.rig.find("Wep_Root");
	const std::array<int32_t, 2> hands{ p_set.rig.find(kHands[0]), p_set.rig.find(kHands[1]) };
	anim::ViewmodelFrameInput in;
	in.speed = 4.0f;
	anim::LocalPose rp, fp, pp;
	anim::ViewmodelOutput ro, fo, po;
	std::vector<anim::Xform> rm, fm, pm;
	double shot = 0.0;
	const int frames = kSettleFrames + int(double(fire.seconds) / kFrameSeconds);
	for (int f = 0; f < frames; ++f) {
		in.time_s += kFrameSeconds;
		in.dt = float(kFrameSeconds);
		std::vector<anim::VmCue> cues;
		if (f == kSettleFrames) {
			shot = in.time_s;
			cues.push_back(anim::VmCue{ anim::VmCue::Kind::Shot, in.time_s, false });
		}
		reaching.step(in, cues, rp, ro);
		free.step(in, cues, fp, fo);
		plain.step(in, cues, pp, po);
		if (f < kSettleFrames) {
			continue;
		}
		anim::model_pose(p_set.rig, rp, rm);
		anim::model_pose(p_set.rig, fp, fm);
		anim::model_pose(p_set.rig, pp, pm);
		const float frame = float(in.time_s - shot) * fire.tracks->fps;
		for (size_t h = 0; h < hands.size(); ++h) {
			bool tagged = false;
			for (const anim::VmReachTag &t : fire.reach) {
				tagged = tagged || (t.arm == h && t.value == 0.0f && frame >= t.from_frame && frame < t.to_frame);
			}
			if (tagged && ro.action_clip == uint32_t(anim::VmClip::Fire) && ro.action_weight >= 1.0f) {
				++out.window_frames;
				out.window_off_free =
						std::max(out.window_off_free, anim::vec_dist(rm[size_t(hands[h])].t, fm[size_t(hands[h])].t));
			} else if (!tagged && frame > 0.0f && frame < fire.reach.front().from_frame) {
				const anim::Vec3 place = anim::xform_mul(
						rm[size_t(root)], anim::xform_mul(anim::xform_inverse(pm[size_t(root)]), pm[size_t(hands[h])]))
												 .t;
				out.held_off = std::max(out.held_off, anim::vec_dist(rm[size_t(hands[h])].t, place));
			}
		}
	}
	return out;
}

Drift hand_drift(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item, const Gait &p_gait,
		const Action &p_action) {
	const anim::ViewmodelTuning tuning;
	const std::array<anim::VmClipRef, anim::kVmClips> refs = render::viewmodel_clip_refs(p_item);
	anim::ViewmodelAnimator moving(p_set.rig, refs, tuning);
	anim::ViewmodelAnimator plain(p_set.rig, refs, tuning);
	moving.set_locomotion(render::viewmodel_locomotion(p_item));
	moving.set_hand_reach(p_set.hand_reach);
	const bool standing = !(p_gait.speed > 0.0f) && !p_gait.sprinting;
	const int32_t root = p_set.rig.find("Wep_Root");
	const std::array<int32_t, 2> hands{ p_set.rig.find(kHands[0]), p_set.rig.find(kHands[1]) };
	const std::array<int32_t, 2> shoulders{ p_set.rig.find("LeftArm"), p_set.rig.find("RightArm") };
	const std::array<int32_t, 2> elbows{ p_set.rig.find("LeftForeArm"), p_set.rig.find("RightForeArm") };
	const anim::VmClipRef &reload = refs[size_t(p_action.empty ? anim::VmClip::ReloadEmpty : anim::VmClip::Reload)];
	const float reload_seconds =
			reload.seconds > 0.0f ? reload.seconds : (reload.tracks ? reload.tracks->duration_seconds() : 0.0f);
	const double action_seconds = p_action.reload ? double(reload_seconds) : (p_action.fire ? kFireSeconds : 1.0);
	const int frames = kSettleFrames + int(action_seconds / kFrameSeconds) + 12;
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
		if (p_action.fire && f == kSettleFrames) {
			cues.push_back(anim::VmCue{ anim::VmCue::Kind::Shot, in.time_s, false });
			cues.push_back(anim::VmCue{ anim::VmCue::Kind::BoltStarted, in.time_s, false });
		}
		moving.step(in, cues, moving_pose, moving_out);
		plain.step(in, cues, plain_pose, plain_out);
		if (f < kSettleFrames) {
			continue;
		}
		anim::model_pose(p_set.rig, moving_pose, moving_model);
		anim::model_pose(p_set.rig, plain_pose, plain_model);
		const anim::Xform moving_frame = standing ? anim::Xform{} : moving_model[size_t(root)];
		const anim::Xform plain_frame = standing ? anim::Xform{} : plain_model[size_t(root)];
		for (size_t h = 0; h < hands.size(); ++h) {
			const anim::Xform on_frame =
					anim::xform_mul(anim::xform_inverse(plain_frame), plain_model[size_t(hands[h])]);
			const anim::Vec3 place = anim::xform_mul(moving_frame, on_frame).t;
			const auto at = [&](int32_t p_joint) { return moving_model[size_t(p_joint)].t; };
			const double arm_length =
					anim::vec_dist(at(shoulders[h]), at(elbows[h])) + anim::vec_dist(at(elbows[h]), at(hands[h]));
			const double off = anim::vec_dist(at(hands[h]), place);
			if (anim::vec_dist(at(shoulders[h]), place) > arm_length - kHeldMetres) {
				++drift.straight[h];
			}
			drift.worst[h] = std::max(drift.worst[h], off);
		}
	}
	return drift;
}

}

TEST_SUITE("render-viewmodel-reload-moving") {
	TEST_CASE("1p reload while walking, running and sprinting: each kit's weapons stay in the hands that hold them "
			  "without the locomotion deltas in every frame, and standing the hand reach leaves the clips' hands where "
			  "they are") {
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
		weapons.push_back(Weapon{ 2, "weapons/sa18igla/u_sa18igla", {}, {}, 0.0, {} });
		for (const std::string extra : { "weapons/m40a5/u_m40a5", "weapons/remington870/u_870",
					 "weapons/model98b/u_m98b", "weapons/p90/u_p90" }) {
			bool listed = false;
			for (uint8_t team = 1; team <= host::kKitTeams; ++team) {
				for (const host::LoadoutWeapon &lw : host::loadout_weapons(catalog, team)) {
					if (lw.unlock == extra) {
						listed = true;
						if (seen.insert(std::to_string(team) + lw.unlock).second) {
							weapons.push_back(Weapon{ team, lw.unlock, {}, {}, 0.0, {} });
						}
					}
				}
			}
			CHECK_MESSAGE(listed, extra, " is not in the loadout catalog");
		}
		REQUIRE(weapons.size() >= 10);
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
			const anim::Skeleton &rig = (*set)->rig;
			for (size_t a = 0; a < 2; ++a) {
				const anim::HandReachArm &arm = (*set)->hand_reach.arms[a];
				const std::string side = a == 0 ? "Left" : "Right";
				const bool usable = arm.usable(rig.size());
				if (!usable || rig.bone(size_t(arm.arm)).name != side + "Arm" ||
						rig.bone(size_t(arm.fore)).name != side + "ForeArm" ||
						rig.bone(size_t(arm.hand)).name != side + "Hand" ||
						rig.bone(size_t(arm.effector)).name != "IK_Joint_" + side + "Hand" || arm.reach != 1.0f) {
					w.error = "the rig's hand reach is not " + side + "Arm, ForeArm, Hand to IK_Joint_" + side +
							"Hand at reach 1";
					for (const std::string &n : (*set)->notes) {
						if (n.find("hand reach") != std::string::npos) {
							w.error += "; " + n;
						}
					}
					return;
				}
			}
			const render::ViewmodelItem &item = (*set)->items[0];
			if (item.clips[size_t(render::ViewmodelClipRole::Bolt)].source == render::ViewmodelClipSource::Clip) {
				w.bolt = item.clips[size_t(render::ViewmodelClipRole::Bolt)].controller;
				w.bolt_root = bolt_root_travel(**set, item);
			}
			for (const Gait &g : kGaits) {
				for (const Action &a : kActions) {
					const Drift d = hand_drift(**set, item, g, a);
					char line[300];
					std::snprintf(line, sizeof(line),
							"%s %s: LeftHand %.1f mm, RightHand %.1f mm; frames with the arm straight to its place: "
							"%d left, %d right",
							a.name, g.name, d.worst[0] * 1000.0, d.worst[1] * 1000.0, d.straight[0], d.straight[1]);
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
			if (!w.bolt.empty()) {
				MESSAGE(w.unlock, " Bolt ", w.bolt, ": Wep_Root up to ", w.bolt_root * 1000.0,
						" mm from its IdlePose place");
				CHECK_MESSAGE(w.bolt_root < kBoltRootMetres, w.unlock, " Bolt ", w.bolt, " takes Wep_Root ",
						w.bolt_root * 1000.0, " mm from its IdlePose place");
			}
			if (w.worst > kHeldMetres) {
				drifting.push_back(w.unlock + " " + w.worst_case);
			}
		}
		MESSAGE(weapons.size(), " weapons, ", std::size(kGaits), " gaits, ", std::size(kActions), " actions; ",
				drifting.size(), " with a hand more than ", kHeldMetres * 1000.0,
				" mm off its place (on the weapon while moving, in the view standing)");
		for (const std::string &d : drifting) {
			MESSAGE("drift: ", d);
		}
		CHECK(drifting.empty());
		for (const std::string bag : { "weapons/gadgets/ammobag/u_ammobag", "weapons/gadgets/medicbag/u_medkit" }) {
			render::ViewmodelSetSpec spec = render::team_viewmodel_items(1, std::span<const std::string>(&bag, 1));
			spec.superbundle = kLevel;
			core::Result<std::unique_ptr<render::ViewmodelSet>> set =
					render::load_viewmodel_set(**sources, spec, nullptr);
			REQUIRE_MESSAGE(set.ok(), bag, ": ", (set.ok() ? "" : set.error().message));
			if (!set.ok() || (*set)->items.size() != 1) {
				continue;
			}
			for (const std::string &note : (*set)->notes) {
				if (note.find(" 1p Fire") != std::string::npos || note.find(" reach:") != std::string::npos ||
						note.find("1p locomotion") != std::string::npos) {
					MESSAGE(note);
				}
			}
			const BagThrow b = bag_throw_while_running(**set, (*set)->items[0]);
			MESSAGE(bag, " Fire ", (*set)->items[0].clips[size_t(render::ViewmodelClipRole::Fire)].controller, ": ",
					b.reach_tags, " reach tags; running, ", b.window_frames, " hand frames in their window, at most ",
					b.window_off_free * 1000.0, " mm from the hand the deltas move freely; before it the hand at most ",
					b.held_off * 1000.0, " mm off its place");
			CHECK(b.reach_tags == 2);
			CHECK(b.window_frames > 20);
			CHECK(b.window_off_free < 1e-4);
			CHECK(b.held_off < kHeldMetres);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}
