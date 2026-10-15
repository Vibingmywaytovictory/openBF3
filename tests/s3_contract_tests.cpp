// S3 interface contract (docs/design/s3.md §11, §14): every Godot-free S3 header compiles on its own set of
// includes, the plain types keep the properties the crowd relies on (inputs copied per frame), and the inline
// helpers behave as the design says: temporal LOD intervals from the retail GameAnimationSettings values,
// phase advance on hostile numbers, and the gate constants. No data, no Godot. Nothing here calls a function the
// track has yet to implement.
#include <doctest/doctest.h>

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/crowd_stage.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/locomotion.h"
#include "bf3/anim/temporal_lod.h"
#include "bf3/core/worker_group.h"
#include "bf3/render/soldier_assets.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace anim = bf3::anim;

static_assert(std::is_trivially_copyable_v<anim::CrowdSoldierInput>);
static_assert(std::is_trivially_copyable_v<anim::TemporalLodTable>);
static_assert(std::is_trivially_copyable_v<anim::LocoWeights>);
static_assert(std::is_trivially_copyable_v<anim::LocoClipInfo>);
static_assert(std::is_trivially_copyable_v<anim::CrowdFrameStats>);
static_assert(!std::is_copy_constructible_v<bf3::core::WorkerGroup>);
static_assert(!std::is_copy_constructible_v<anim::CrowdEvaluator>);
static_assert(anim::kLocoClips == 5);
static_assert(anim::kCrowdClips == 8);
static_assert(anim::kStagePhases == 8);
static_assert(uint8_t(anim::LocoClip::Idle) == 0 && uint8_t(anim::CrowdClip::Idle) == 0);
static_assert(anim::TemporalLodTable::kTiers == 6);
static_assert(anim::kMaxPaletteBinds >= 113); // the US team's measured binds (s3.md §4.3)
static_assert(anim::s3_gate::kSoldiers == 64 && anim::s3_gate::kSoldiers <= anim::kMaxCrowdSoldiers);
static_assert(anim::CrowdStageSpec{}.soldiers() == anim::s3_gate::kSoldiers);
static_assert(anim::s3_gate::kWidth == 2560 && anim::s3_gate::kHeight == 1440);
static_assert(anim::s3_gate::kMainThreadMs == 2.0 && anim::s3_gate::kFrameMs == 16.7 && anim::s3_gate::kMinSeconds >= 30.0);
static_assert((anim::crowd_flags::kReloadStart & anim::crowd_flags::kHidden) == 0);

TEST_CASE("s3 contract: temporal LOD defaults are the retail GameAnimationSettings values") {
	const anim::TemporalLodTable t;
	const float distance[6] = { 2.0f, 30.0f, 60.0f, 90.0f, 120.0f, 150.0f };
	const float delta[6] = { 0.02f, 0.04f, 0.06f, 0.08f, 0.10f, 0.12f };
	for (int i = 0; i < 6; ++i) {
		CHECK(t.tiers[size_t(i)].distance == distance[i]);
		CHECK(t.tiers[size_t(i)].delta_time == delta[i]);
	}
	CHECK(t.far_distance == 208.0f);
	CHECK(t.far_policy == anim::FarPolicy::Freeze);
	CHECK(t.client_enable);
}

TEST_CASE("s3 contract: temporal LOD interval and tier by distance") {
	anim::TemporalLodTable t;
	CHECK(anim::temporal_lod_interval(t, 0.0f) == 0.0f);
	CHECK(anim::temporal_lod_interval(t, 1.99f) == 0.0f);
	CHECK(anim::temporal_lod_interval(t, 2.0f) == 0.02f);
	CHECK(anim::temporal_lod_interval(t, 29.9f) == 0.02f);
	CHECK(anim::temporal_lod_interval(t, 30.0f) == 0.04f);
	CHECK(anim::temporal_lod_interval(t, 150.0f) == 0.12f);
	CHECK(anim::temporal_lod_interval(t, 208.0f) == 0.12f);
	CHECK(anim::temporal_lod_interval(t, 208.5f) < 0.0f);
	CHECK(anim::temporal_lod_tier(t, 1.0f) == -1);
	CHECK(anim::temporal_lod_tier(t, 2.0f) == 0);
	CHECK(anim::temporal_lod_tier(t, 61.0f) == 2);
	CHECK(anim::temporal_lod_tier(t, 300.0f) == 6);
	// Hostile or odd numbers never freeze a soldier by accident.
	CHECK(anim::temporal_lod_interval(t, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
	CHECK(anim::temporal_lod_interval(t, -5.0f) == 0.0f);
	t.far_policy = anim::FarPolicy::LastTier;
	CHECK(anim::temporal_lod_interval(t, 1.0e9f) == 0.12f);
	CHECK(anim::temporal_lod_interval(t, std::numeric_limits<float>::infinity()) == 0.12f);
	t.client_enable = false;
	CHECK(anim::temporal_lod_interval(t, 100.0f) == 0.0f);
}

TEST_CASE("s3 contract: phase advance wraps and rejects hostile numbers") {
	CHECK(anim::advance_phase(0.0f, 0.3f, 0.6f) == doctest::Approx(0.5f));
	CHECK(anim::advance_phase(0.9f, 0.12f, 0.6f) == doctest::Approx(0.1f));
	CHECK(anim::advance_phase(0.5f, 6.0f, 0.6f) == doctest::Approx(0.5f).epsilon(1e-4));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(anim::advance_phase(nan, 0.1f, 0.6f) == 0.0f);
	CHECK(anim::advance_phase(0.2f, nan, 0.6f) == 0.0f);
	CHECK(anim::advance_phase(0.2f, inf, 0.6f) == 0.0f);
	CHECK(anim::advance_phase(0.2f, -1.0f, 0.6f) == 0.0f);
	CHECK(anim::advance_phase(0.2f, 0.1f, 0.0f) == 0.0f);
	CHECK(anim::advance_phase(0.2f, 0.1f, 1.0e-30f) == 0.0f);
	CHECK(anim::advance_phase(1.5f, 0.1f, 0.6f) == 0.0f);
	for (int i = 0; i < 1000; ++i) {
		const float p = anim::advance_phase(float(i % 997) / 997.0f, float(i) * 0.0137f, 0.5f + float(i % 7) * 0.1f);
		CHECK((p >= 0.0f && p < 1.0f));
	}
}

TEST_CASE("s3 contract: default clip and asset names are the verified ones") {
	const anim::CrowdClipNames n;
	CHECK(n.package == "b_basicassetsmp");
	CHECK(n.controllers[size_t(anim::CrowdClip::RunFwd)] == "Rifle_RunFwd");
	CHECK(n.controllers[size_t(anim::CrowdClip::AimDown)] == "Rifle_StandAimDwn");
	CHECK(n.reload_chooser == "3P_Wep_Reload_NoAddon_Stand_Chooser");
	CHECK(n.root_joint == "AITrajectory");
	const bf3::render::SoldierSpec s;
	CHECK(s.body_skeleton == "animations/skeletons/veniceantske01");
	CHECK(s.teams[0].weapon_unlock == "weapons/m16a4/m16a4");
	CHECK(s.teams[1].weapon_unlock == "weapons/ak74m/ak74");
	CHECK(s.settings_partition == "default/settings_win32");
}
