#include <doctest/doctest.h>

#include "bf3/anim/pose.h"
#include "bf3/anim/vault_anim.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/math.h"
#include "bf3/core/thread_pool.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/loadout.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/render/viewmodel_vault.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;

namespace {

constexpr const char *kVaultLevel = "Win32/Levels/MP_Subway/MP_Subway";
constexpr double kVaultFrameSeconds = 1.0 / 60.0;

}

TEST_SUITE("render-viewmodel-vault") {
	TEST_CASE("1p vault: each kit weapon type plays its vault layer's additive clip from the move's time to the "
			  "sequence's end, and the clip moves the camera joint") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kVaultLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		for (const std::string unlock : { "weapons/m16a4/u_m16a4", "weapons/m9/u_m9" }) {
			render::ViewmodelSetSpec spec = render::team_viewmodel_items(1, std::span<const std::string>(&unlock, 1));
			spec.superbundle = kVaultLevel;
			core::Result<std::unique_ptr<render::ViewmodelSet>> set =
					render::load_viewmodel_set(**sources, spec, nullptr);
			REQUIRE_MESSAGE(set.ok(), unlock, ": ", (set.ok() ? "" : set.error().message));
			REQUIRE((*set)->items.size() == 1);
			const render::ViewmodelItem &item = (*set)->items[0];
			for (const std::string &note : (*set)->notes) {
				if (note.find("1p vault") != std::string::npos) {
					MESSAGE(note);
				}
			}
			REQUIRE(item.vault != nullptr);
			render::ViewmodelSetSpec deferred_spec = spec;
			deferred_spec.vault = false;
			core::Result<std::unique_ptr<render::ViewmodelSet>> deferred =
					render::load_viewmodel_set(**sources, deferred_spec, nullptr);
			REQUIRE_MESSAGE(deferred.ok(), unlock, ": ", (deferred.ok() ? "" : deferred.error().message));
			REQUIRE((*deferred)->items.size() == 1);
			CHECK((*deferred)->items[0].vault == nullptr);
			CHECK((*deferred)->items[0].weapon_ant_ref == item.weapon_ant_ref);
			std::vector<std::string> deferred_notes;
			const std::vector<std::shared_ptr<const render::ViewmodelVaultClips>> later =
					render::load_viewmodel_set_vaults(**deferred, deferred_notes);
			REQUIRE(later.size() == 1);
			REQUIRE(later[0] != nullptr);
			for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
				const render::ViewmodelClip &a = item.vault->clips[k], &b = later[0]->clips[k];
				CHECK(a.source == b.source);
				CHECK(a.name == b.name);
				CHECK(a.tracks.end_frame == b.tracks.end_frame);
				CHECK(a.seconds == b.seconds);
				CHECK(item.vault->path[k] == later[0]->path[k]);
				CHECK(item.vault->play[k].start_tick == later[0]->play[k].start_tick);
				CHECK(item.vault->play[k].end_tick == later[0]->play[k].end_tick);
				CHECK(item.vault->play[k].clip_ticks_per_tick == later[0]->play[k].clip_ticks_per_tick);
				CHECK(item.vault->play[k].frames_per_tick == later[0]->play[k].frames_per_tick);
			}
			const anim::VmVault vault = render::viewmodel_vault((*set)->rig, item);
			REQUIRE(vault.camera_joint >= 0);
			for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
				REQUIRE_MESSAGE(vault.clips[k].clip != nullptr, unlock, " kind ", k);
				anim::ViewmodelAnimator animator(
						(*set)->rig, render::viewmodel_clip_refs(item), anim::ViewmodelTuning{});
				animator.set_vault(vault);
				anim::ViewmodelAnimator sprinting(
						(*set)->rig, render::viewmodel_clip_refs(item), anim::ViewmodelTuning{});
				sprinting.set_vault(vault);
				anim::LocalPose pose, sprint_pose;
				anim::ViewmodelOutput out, sprint_out;
				float sprint_off = 0.0f;
				const float seconds = vault.play[k].seconds();
				const float move = std::min(seconds, 0.5f);
				uint32_t frames = 0, wrong = 0, after_move = 0;
				float worst_time = 0.0f, camera_m = 0.0f, camera_deg = 0.0f;
				double t = 0.0;
				for (; t < double(seconds) + 0.25; t += kVaultFrameSeconds) {
					anim::ViewmodelFrameInput in;
					in.time_s = 10.0 + t;
					in.dt = float(kVaultFrameSeconds);
					if (t < double(move)) {
						in.vault = uint8_t(k);
						in.vault_seconds = float(t);
					}
					animator.step(in, {}, pose, out);
					anim::ViewmodelFrameInput run = in;
					run.sprinting = true;
					run.speed = 6.0f;
					sprinting.step(run, {}, sprint_pose, sprint_out);
					if (out.vault_clip == k) {
						for (size_t j = 0; j < pose.joints.size(); ++j) {
							const anim::Vec3 a = pose.joints[j].t, b = sprint_pose.joints[j].t;
							sprint_off = std::max(sprint_off,
									std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) +
											std::fabs(pose.joints[j].q.w - sprint_pose.joints[j].q.w));
						}
					}
					if (t < double(seconds) - kVaultFrameSeconds) {
						frames += out.vault_clip == k ? 1u : 0u;
						wrong += out.vault_clip != k ? 1u : 0u;
						after_move += out.vault_clip == k && t >= double(move) ? 1u : 0u;
						worst_time = std::max(worst_time, std::fabs(out.vault_seconds - float(t)));
						const anim::Vec3 d = out.camera_offset.t;
						camera_m = std::max(camera_m, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
						camera_deg = std::max(camera_deg,
								2.0f * std::acos(std::clamp(std::fabs(out.camera_offset.q.w), 0.0f, 1.0f)) /
										core::kDegToRadF);
					}
				}
				MESSAGE(unlock << " kind " << k << ": " << frames << " frames over " << seconds << " s (" << after_move
							   << " after the move's input ends), time off by at most " << worst_time
							   << " s, camera joint up to " << camera_m << " m and " << camera_deg
							   << " deg; sprinting changes the vault pose by " << sprint_off);
				CHECK(wrong == 0);
				CHECK(after_move > 0);
				CHECK(worst_time < 1e-4f);
				CHECK(sprint_off < 1e-5f);
				CHECK((camera_m > 0.001f || camera_deg > 0.1f));
				CHECK(out.vault_clip == anim::kNoVaultAnim);
			}
		}
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		std::vector<std::pair<uint8_t, std::string>> kit_weapons;
		for (uint8_t team = 1; team <= host::kKitTeams; ++team) {
			for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
				const host::LoadoutKit *k = (*gunplay)->loadouts.kit(team, kit);
				REQUIRE(k != nullptr);
				for (size_t slot = 0; slot < k->defaults.size(); ++slot) {
					if (const host::LoadoutItem *item = k->find(slot, k->defaults[slot])) {
						const std::pair<uint8_t, std::string> key{ team, item->unlock };
						if (std::find(kit_weapons.begin(), kit_weapons.end(), key) == kit_weapons.end()) {
							kit_weapons.push_back(key);
						}
					}
				}
			}
		}
		std::vector<int> clips(kit_weapons.size(), -1);
		core::parallel_for(kit_weapons.size(), [&](size_t i) {
			render::ViewmodelSetSpec spec = render::team_viewmodel_items(
					kit_weapons[i].first, std::span<const std::string>(&kit_weapons[i].second, 1));
			spec.superbundle = kVaultLevel;
			core::Result<std::unique_ptr<render::ViewmodelSet>> set =
					render::load_viewmodel_set(**sources, spec, nullptr);
			if (set && (*set)->items.size() == 1) {
				const anim::VmVault v = render::viewmodel_vault((*set)->rig, (*set)->items[0]);
				clips[i] = int(std::count_if(
						v.clips.begin(), v.clips.end(), [](const anim::VmClipRef &c) { return c.clip != nullptr; }));
			}
		});
		std::string without;
		size_t full = 0;
		for (size_t i = 0; i < kit_weapons.size(); ++i) {
			full += clips[i] == int(anim::kVaultAnimKinds) ? 1u : 0u;
			if (clips[i] != int(anim::kVaultAnimKinds)) {
				without += " " + kit_weapons[i].second + " (" + std::to_string(clips[i]) + ")";
			}
		}
		MESSAGE("the kits' default items: " << full << " of " << kit_weapons.size() << " have all three 1p vault clips;"
											<< (without.empty() ? std::string(" none without") : without));
		CHECK(full > 0);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		MESSAGE("install unchanged: " << (diff.clean() ? "yes" : diff.summary()));
		CHECK(diff.clean());
	}
}
