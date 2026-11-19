#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skinning.h"
#include "bf3/anim/vault_anim.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::anim;

namespace {

constexpr uint32_t kM16AntRef = 357063601u;
constexpr std::array<uint32_t, kVaultAnimKinds> kBindingTriggers = { 357063172u, 357088519u, 357041652u };
constexpr std::array<const char *, kVaultAnimKinds> kTriggers = { "Game.Character.VaultHigh",
	"Game.Character.VaultUpHigh", "Game.Character.VaultLow" };
constexpr std::array<const char *, kVaultAnimKinds> kFirstPerson = { "1P_VaultOverHighLayer", "1P_VaultUpLayer",
	"1P_VaultOverLowLayer" };
constexpr std::array<const char *, kVaultAnimKinds> kThirdPerson = { "3P_Loco_VaultHighChooser",
	"3P_Loco_VaultUpHighChooser", "3P_Loco_VaultLowChooser" };
constexpr std::array<const char *, kVaultAnimKinds> kRifleThirdPerson = { "Rifle_VaultOverHigh", "Rifle_VaultUpHigh",
	"Rifle_VaultOverLow" };

struct VaultData {
	bool ok = false;
	std::string why;
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::ant::AntInstallSource> source;
	std::unique_ptr<bf3::core::ant::AntLibrary> library;
	CrowdClipSet clips;
};

VaultData *vault_data() {
	static VaultData *data = [] {
		auto *d = new VaultData();
		if (!d->reader.locate({})) {
			d->why = d->reader.describe();
			return d;
		}
		d->before = d->reader.snapshot();
		auto install = bf3::core::Install::open(d->reader);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
		d->install = *install;
		auto source = bf3::core::ant::AntInstallSource::open(d->install);
		REQUIRE_MESSAGE(source.ok(), (source.ok() ? "" : source.error().message));
		d->source = std::move(*source);
		auto lib = load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		auto clips = load_crowd_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		d->clips = std::move(*clips);
		d->ok = true;
		return d;
	}();
	return data;
}

VaultData *vault_or_skip() {
	VaultData *d = vault_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

ChooserIndices rifle(const VaultData &p_d) {
	ChooserIndices idx;
	auto type = weapon_type_index(*p_d.library, kM16AntRef);
	REQUIRE_MESSAGE(type.ok(), (type.ok() ? "" : type.error().message));
	idx.weapon_type = *type;
	auto specific = weapon_choice_index(*p_d.library, kM16AntRef);
	REQUIRE_MESSAGE(specific.ok(), (specific.ok() ? "" : specific.error().message));
	idx.specific_weapon = int64_t(*specific);
	return idx;
}

}

TEST_SUITE("anim-vault") {
	TEST_CASE("vault controllers: each trigger signal gates one 1p layer node and leads to one 3p chooser node") {
		VaultData *d = vault_or_skip();
		if (d == nullptr) {
			return;
		}
		auto c = find_vault_controllers(*d->library);
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			MESSAGE(c->trigger_names[k] << ": 1p node " << c->first_person[k].node << " -> " << c->first_person[k].name
										<< "; 3p node " << c->third_person[k].node << " -> " << c->third_person[k].name
										<< ", blend in " << c->third_person[k].blend_in_ticks << " ticks");
			CHECK(c->trigger_names[k] == kTriggers[k]);
			CHECK((c->triggers[k].index() & 0xFFFFu) == (kBindingTriggers[k] & 0xFFFFu));
			CHECK(c->first_person[k].name == kFirstPerson[k]);
			CHECK(c->third_person[k].name == kThirdPerson[k]);
			CHECK(c->third_person[k].blend_in_ticks == 3.0f);
		}
	}

	TEST_CASE("vault routes: the rifle's 1p and 3p sequences play their whole clip, sequence ticks times Scale") {
		VaultData *d = vault_or_skip();
		if (d == nullptr) {
			return;
		}
		auto c = find_vault_controllers(*d->library);
		REQUIRE(c.ok());
		const ChooserIndices idx = rifle(*d);
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			for (const bool first : { true, false }) {
				const VaultController &vc = first ? c->first_person[k] : c->third_person[k];
				auto route = route_vault(*d->library, vc, idx);
				REQUIRE_MESSAGE(route.ok(), (route.ok() ? "" : route.error().message));
				auto play = vault_play_of(*route);
				REQUIRE_MESSAGE(play.ok(), (play.ok() ? "" : play.error().message));
				std::string tags;
				for (const AntEventTag &t : route->event_tags) {
					tags += " " + std::to_string(t.id) + "@" + std::to_string(t.tick);
				}
				std::string branches;
				for (const AntBranchTag &b : route->branches) {
					branches += " " + b.type + "@" + std::to_string(b.time) + "+" + std::to_string(b.length);
				}
				MESSAGE((first ? "1p " : "3p ")
						<< vc.name << " -> " << route->controller << ": sequence ticks " << play->start_tick << " .. "
						<< play->end_tick << " x " << play->clip_ticks_per_tick << " = clip ticks " << route->num_ticks
						<< ", " << play->seconds() << " s; event tags" << tags << "; branches" << branches);
				CHECK(route->sequence);
				CHECK(std::fabs((play->end_tick - play->start_tick) * play->clip_ticks_per_tick -
							  float(route->num_ticks)) < 1e-3f);
				if (!first) {
					CHECK(route->controller == kRifleThirdPerson[k]);
				}
			}
		}
	}

	TEST_CASE("vault 3p clips bind to the crowd rig; over vaults keep the trajectory on the start height and lift the "
			  "hips, the up vault lifts the trajectory") {
		VaultData *d = vault_or_skip();
		if (d == nullptr) {
			return;
		}
		auto c = find_vault_controllers(*d->library);
		REQUIRE(c.ok());
		const ChooserIndices idx = rifle(*d);
		const int32_t hips = d->clips.rig.find("Hips");
		REQUIRE(hips >= 0);
		REQUIRE(d->clips.root_joint >= 0);
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			auto v = load_crowd_vault(d->clips, *d->library, *c, k, idx);
			REQUIRE_MESSAGE(v.ok(), (v.ok() ? "" : v.error().message));
			CHECK(v->clip.mask.size() == d->clips.rig.size());
			CHECK(v->blend_in_seconds == doctest::Approx(3.0 / 60.0));
			bf3::core::ant::ClipPose scratch;
			LocalPose pose;
			std::vector<Xform> model;
			std::string trace;
			Vec3 root0{}, hips0{};
			float root_rise = 0.0f, root_end_rise = 0.0f, hips_rise = 0.0f;
			const int frames = 12;
			for (int f = 0; f <= frames; ++f) {
				const float s = v->play.seconds() * float(f) / float(frames);
				REQUIRE(sample_pose(d->clips.rig, v->clip.clip, v->clip.tracks, v->play.frame_at(s), pose, scratch));
				const Vec3 root_t = pose.joints[size_t(d->clips.root_joint)].t;
				pose.joints[size_t(d->clips.root_joint)].t = d->clips.rig.bone(size_t(d->clips.root_joint)).rest.t;
				model_pose(d->clips.rig, pose, model);
				const Vec3 h = model[size_t(hips)].t;
				if (f == 0) {
					root0 = root_t;
					hips0 = h;
				}
				root_rise = std::max(root_rise, std::fabs(root_t.y - root0.y));
				root_end_rise = root_t.y - root0.y;
				hips_rise = std::max(hips_rise, h.y - hips0.y);
				char line[160];
				std::snprintf(line, sizeof(line), "\n  %.3f s: root %+.2f %+.2f %+.2f, hips %+.2f %+.2f %+.2f",
						double(s), double(root_t.x - root0.x), double(root_t.y - root0.y), double(root_t.z - root0.z),
						double(h.x - hips0.x), double(h.y - hips0.y), double(h.z - hips0.z));
				trace += line;
			}
			MESSAGE(v->controller << ": " << v->play.seconds()
								  << " s, root and hips against frame 0 with the root pinned:" << trace);
			CHECK(v->root_start.y == doctest::Approx(root0.y));
			if (k == 1) {
				CHECK(root_end_rise == doctest::Approx(1.09).epsilon(0.01));
			} else {
				CHECK(root_rise < 0.01f);
				CHECK(hips_rise > 0.3f);
			}
		}
	}

	TEST_CASE("vault 3p in the crowd: a vault input plays the clip from its time, the clip outlasts the input, then "
			  "locomotion returns") {
		VaultData *d = vault_or_skip();
		if (d == nullptr) {
			return;
		}
		auto c = find_vault_controllers(*d->library);
		REQUIRE(c.ok());
		const ChooserIndices idx = rifle(*d);
		std::array<CrowdVaultClip, kVaultAnimKinds> clips;
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			auto v = load_crowd_vault(d->clips, *d->library, *c, k, idx);
			REQUIRE(v.ok());
			clips[k] = std::move(*v);
		}
		EbxSkeleton ebx;
		ebx.partition = "rig";
		std::vector<Xform> rest;
		model_pose(d->clips.rig, rest_pose(d->clips.rig), rest);
		for (size_t i = 0; i < d->clips.rig.size(); ++i) {
			ebx.bone_names.push_back(d->clips.rig.bone(i).name);
			ebx.parents.push_back(d->clips.rig.bone(i).parent);
			ebx.local_pose.push_back(mat_from_xform(d->clips.rig.bone(i).rest));
			ebx.model_pose.push_back(mat_from_xform(rest[i]));
		}
		SkinBinds binds;
		for (uint32_t i = 0; i < 4; ++i) {
			REQUIRE(binds.bind(ebx, i, d->clips.rig).ok());
		}
		CrowdWeapon weapon;
		weapon.binds = &binds;
		for (size_t k = 0; k < kVaultAnimKinds; ++k) {
			weapon.vault[k] = &clips[k];
		}
		CrowdTeam team;
		team.binds = &binds;
		team.weapons = std::span<const CrowdWeapon>(&weapon, 1);
		const std::vector<CrowdTeam> teams{ team, team };
		CrowdConfig config;
		config.temporal_lod = false;
		auto e = CrowdEvaluator::make(d->clips, teams, 1, config);
		REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
		CrowdEvaluator &ev = **e;
		const int32_t hips = d->clips.rig.find("Hips");
		const size_t root = size_t(d->clips.root_joint);

		std::vector<CrowdSoldierInput> in(1);
		in[0].vault = 0;
		in[0].vault_seconds = 0.4f;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		uint8_t kind = kNoVaultAnim;
		float seconds = 0.0f;
		CHECK(ev.vault_of(0, kind, seconds));
		CHECK(kind == 0);
		CHECK(seconds == doctest::Approx(0.4));
		CHECK(ev.combat_stats().vaulting == 1);
		LocalPose pose;
		bf3::core::ant::ClipPose scratch;
		REQUIRE(sample_pose(
				d->clips.rig, clips[0].clip.clip, clips[0].clip.tracks, clips[0].play.frame_at(0.4f), pose, scratch));
		pose.joints[root].t = d->clips.rig.bone(root).rest.t;
		std::vector<Xform> model;
		model_pose(d->clips.rig, pose, model);
		CHECK(std::fabs(ev.model(0)[size_t(hips)].t.y - model[size_t(hips)].t.y) < 1e-4f);

		in[0].vault = kNoVaultAnim;
		ev.set_inputs(in, 0.3);
		ev.evaluate_all_here();
		CHECK(ev.vault_of(0, kind, seconds));
		CHECK(seconds == doctest::Approx(0.7));
		ev.set_inputs(in, 0.5);
		ev.evaluate_all_here();
		CHECK(!ev.vault_of(0, kind, seconds));
		CHECK(ev.combat_stats().vaulting == 0);

		in[0].vault = 1;
		in[0].vault_seconds = 0.9f;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		REQUIRE(sample_pose(
				d->clips.rig, clips[1].clip.clip, clips[1].clip.tracks, clips[1].play.frame_at(0.9f), pose, scratch));
		const float rise = pose.joints[root].t.y - clips[1].root_start.y;
		MESSAGE("up vault at 0.9 s: the clip's trajectory rises " << rise << " m above its start");
		pose.joints[root].t = d->clips.rig.bone(root).rest.t;
		pose.joints[root].t.y += rise;
		model_pose(d->clips.rig, pose, model);
		CHECK(rise > 0.5f);
		CHECK(std::fabs(ev.model(0)[size_t(hips)].t.y - model[size_t(hips)].t.y) < 1e-4f);

		in[0].vault = kNoVaultAnim;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		CHECK(ev.vault_of(0, kind, seconds));
		REQUIRE(sample_pose(d->clips.rig, clips[1].clip.clip, clips[1].clip.tracks, clips[1].play.frame_at(seconds),
				pose, scratch));
		pose.joints[root].t = d->clips.rig.bone(root).rest.t;
		model_pose(d->clips.rig, pose, model);
		CHECK(std::fabs(ev.model(0)[size_t(hips)].t.y - model[size_t(hips)].t.y) < 1e-4f);
	}

	TEST_CASE("install unchanged") {
		VaultData *d = vault_or_skip();
		if (d == nullptr) {
			return;
		}
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(d->before, d->reader.snapshot());
		MESSAGE("install unchanged: " << (diff.clean() ? "yes" : diff.summary()));
		CHECK(diff.clean());
	}
}
