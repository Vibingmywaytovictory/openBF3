#include <doctest/doctest.h>

#include "bf3/anim/pose.h"
#include "bf3/anim/vault_anim.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/math.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/render/viewmodel_vault.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
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
			const anim::VmVault vault = render::viewmodel_vault((*set)->rig, item);
			REQUIRE(vault.camera_joint >= 0);
			for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
				REQUIRE_MESSAGE(vault.clips[k].clip != nullptr, unlock, " kind ", k);
				anim::ViewmodelAnimator animator(
						(*set)->rig, render::viewmodel_clip_refs(item), anim::ViewmodelTuning{});
				animator.set_vault(vault);
				anim::LocalPose pose;
				anim::ViewmodelOutput out;
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
							   << " s, camera joint up to " << camera_m << " m and " << camera_deg << " deg");
				CHECK(wrong == 0);
				CHECK(after_move > 0);
				CHECK(worst_time < 1e-4f);
				CHECK((camera_m > 0.001f || camera_deg > 0.1f));
				CHECK(out.vault_clip == anim::kNoVaultAnim);
			}
		}
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		MESSAGE("install unchanged: " << (diff.clean() ? "yes" : diff.summary()));
		CHECK(diff.clean());
	}
}
