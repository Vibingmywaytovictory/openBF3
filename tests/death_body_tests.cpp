#include "bf3/core/gameplay/death_body.h"

#include <doctest/doctest.h>

#include <utility>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace {

constexpr float kTick = float(1.0 / 30.0);

DeathHitDirection direction_of(Vec3 p_d) {
	return death_hit_direction(DeathFrame{}, p_d);
}

}

TEST_SUITE("death-body-unit") {
	TEST_CASE("death record: the hit-direction code of R5-DT4 TV1, ties to front and back, a vertical hit to world X") {
		CHECK(direction_of({ 0.0f, 0.0f, -1.0f }) == DeathHitDirection::Front);
		CHECK(direction_of({ 0.0f, 0.0f, 1.0f }) == DeathHitDirection::Back);
		CHECK(direction_of({ 1.0f, 0.0f, 0.0f }) == DeathHitDirection::Right);
		CHECK(direction_of({ -1.0f, 0.0f, 0.0f }) == DeathHitDirection::Left);
		CHECK(direction_of({ 0.7071f, 0.0f, -0.7071f }) == DeathHitDirection::Front);
		CHECK(direction_of({ 0.6f, -0.8f, 0.0f }) == DeathHitDirection::Right);
		CHECK(direction_of({ 0.0f, -1.0f, 0.0f }) == DeathHitDirection::Right);
		const DeathFrame turned = death_frame_of_yaw(3.14159265f);
		CHECK(death_hit_direction(turned, { 0.0f, -1.0f, 0.0f }) == DeathHitDirection::Left);
		CHECK(death_hit_direction(turned, { 0.0f, 0.0f, 1.0f }) == DeathHitDirection::Front);
		DeathCounter c;
		DeathInput none;
		CHECK(make_death_record(none, c).direction == DeathHitDirection::None);
	}

	TEST_CASE("death record: the movement signs of TV2 with the 0.1 m/s dead zone, exactly 0.1 counts") {
		DeathCounter c;
		auto speeds = [&](Vec3 p_v) {
			DeathInput in;
			in.velocity = p_v;
			const DeathRecord r = make_death_record(in, c);
			return std::pair<int, int>(r.forward_speed, r.right_speed);
		};
		CHECK(speeds({ 0.0f, 0.0f, 3.0f }) == std::pair<int, int>(1, 0));
		CHECK(speeds({ 0.0f, 0.0f, -0.05f }) == std::pair<int, int>(0, 0));
		CHECK(speeds({ 2.0f, 0.0f, 0.0f }) == std::pair<int, int>(0, -1));
		CHECK(speeds({ -0.1f, 0.0f, 0.0f }) == std::pair<int, int>(0, 1));
		CHECK(speeds({ 0.0f, -9.0f, 0.0f }) == std::pair<int, int>(0, 0));
		const DeathFrame f = death_frame_of_yaw(1.57079633f);
		CHECK(f.forward.x == doctest::Approx(1.0f));
		CHECK(f.right.z == doctest::Approx(-1.0f));
	}

	TEST_CASE("death record: bone types of TV3, the round-robin index of TV4 and RandomValue in (0, 1]") {
		DeathCounter c;
		const std::vector<std::pair<uint32_t, DeathBone>> bones{ { 0, DeathBone::Body }, { 1, DeathBone::Head },
			{ 2, DeathBone::RightArm }, { 3, DeathBone::LeftArm }, { 4, DeathBone::RightLeg },
			{ 5, DeathBone::LeftLeg }, { 6, DeathBone::Body }, { 200, DeathBone::Body } };
		for (const auto &[type, bone] : bones) {
			DeathInput in;
			in.bone_type = type;
			CHECK(make_death_record(in, c).bone == bone);
		}
		DeathCounter seven;
		std::vector<int> indexes;
		for (int i = 0; i < 7; ++i) {
			indexes.push_back(make_death_record(DeathInput{}, seven).animation_index);
		}
		CHECK(indexes == std::vector<int>{ 0, 1, 2, 3, 4, 0, 1 });
		DeathRecord r;
		r.random = 0;
		CHECK(r.random_value() == doctest::Approx(1.0 / 16777216.0).epsilon(1e-9));
		r.random = kDeathRandomMask;
		CHECK(r.random_value() == 1.0f);
		DeathInput in;
		in.random = 0xFFFFFFFFu;
		in.explosion = true;
		const DeathRecord big = make_death_record(in, c);
		CHECK(big.random == kDeathRandomMask);
		CHECK(big.explosion);
		CHECK(big.valid);
	}

	TEST_CASE("ragdoll blend: TV5 rises with the graph, settles at 0.6 and ramps to 1 in 0.1 s, fully in on update 7") {
		RagdollBlend b;
		b.activate();
		const float targets[7] = { 0.0f, 0.3f, 0.6f, 0.6f, 0.6f, 0.6f, 0.6f };
		const float want[7] = { 0.0f, 0.3f, 0.6f, 0.6f, 0.73333f, 0.86667f, 1.0f };
		for (int u = 0; u < 7; ++u) {
			if (b.copies_pose()) {
				b.pose_copied();
			}
			b.update(kTick, targets[u]);
			CHECK(b.weight() == doctest::Approx(want[u]).epsilon(1e-4));
			CHECK(b.fully_blended_in() == (u == 6));
		}
		CHECK_FALSE(b.copies_pose());
	}

	TEST_CASE("ragdoll blend: TV6 a graph that never raises the blend goes full ragdoll on update 121") {
		RagdollBlend b;
		b.activate();
		int first = -1;
		for (int u = 1; u <= 200 && first < 0; ++u) {
			b.pose_copied();
			b.update(kTick, 0.0f);
			if (b.weight() >= 1.0f) {
				first = u;
			}
		}
		CHECK(first == 121);
		RagdollBlend idle;
		idle.update(kTick, 1.0f);
		CHECK(idle.weight() == 0.0f);
	}

	TEST_CASE("ragdoll settle: TV7 freezes on update 17 when every body is slow, one body at 0.51 m/s keeps it awake") {
		RagdollSettle s;
		int first = -1;
		for (int u = 1; u <= 40 && first < 0; ++u) {
			if (s.update(kTick, 0.5f, 0.5f)) {
				first = u;
			}
		}
		CHECK(first == 17);
		RagdollSettle awake;
		for (int u = 1; u <= 40; ++u) {
			CHECK_FALSE(awake.update(kTick, 0.51f, 0.0f));
		}
	}

	TEST_CASE("ragdoll revive: TV9 holds 1.0 s, then the root moves to the soldier over 0.7 s ending on update 50") {
		RagdollBlend b;
		b.activate();
		for (int u = 0; u < 130; ++u) {
			b.pose_copied();
			b.update(kTick, 1.0f);
		}
		REQUIRE(b.weight() == 1.0f);
		b.blend_out();
		CHECK(b.blending_out());
		int root_from = -1, root_done = -1, done = -1;
		for (int u = 1; u <= 60; ++u) {
			b.update(kTick, 0.0f);
			if (root_from < 0 && b.root_fraction() < 1.0f) {
				root_from = u;
			}
			if (root_done < 0 && b.root_fraction() == 0.0f) {
				root_done = u;
			}
			if (done < 0 && b.blend_out_done()) {
				done = u;
			}
		}
		CHECK(root_from == 30);
		CHECK(root_done == 50);
		CHECK(done == 50);
		CHECK(b.weight() == 0.0f);
		CHECK_FALSE(b.fully_blended_in());
	}
}
