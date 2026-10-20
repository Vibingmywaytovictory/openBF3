#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/game_server.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "m2a_check.h"
#include "m2a_game.h"

#include "gunplay_fixture.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace bf3;
namespace sw = bf3::sim::weapons;
namespace tg = bf3::test::gunplay;
namespace gs = bf3::godot_side;

namespace {

constexpr bool kBtLanded = true;
constexpr bool kTpLanded = true;
constexpr bool kAu3Landed = true;
constexpr bool kFx3Landed = true;

void pending(const char *p_key, const char *p_row, const char *p_reason) {
	std::printf("m2a.contract3: PENDING (%s): %s -- %s\n", p_key, p_row, p_reason);
	const std::string text = std::string("PENDING (") + p_key + "): " + p_row;
	MESSAGE(text);
}

static_assert(sizeof(host::PresentEvent) == 64);
static_assert(std::is_trivially_copyable_v<host::PresentedOtherWeapon>);
static_assert(std::is_trivially_copyable_v<host::PresentClock>);
static_assert(std::is_trivially_copyable_v<fx::MuzzleSample>);
static_assert(std::is_trivially_copyable_v<fx::FxTriggerStats>);
static_assert(std::is_trivially_copyable_v<audio::MuzzlePoint>);
static_assert(std::is_trivially_copyable_v<audio::WeaponAudioStats>);
static_assert(std::is_trivially_copyable_v<audio::ImpactAudioStats>);
static_assert(std::is_trivially_copyable_v<anim::CrowdSoldierInput>);
static_assert(!std::is_copy_constructible_v<audio::WeaponAudioRouter>);
static_assert(!std::is_copy_constructible_v<fx::FxTriggers>);
static_assert(std::is_base_of_v<host::CommandSource, host::FirefightBrain>);
static_assert(std::is_base_of_v<audio::MixTap, audio::GameAudioCheck>);
static_assert(std::is_base_of_v<audio::WeaponAudioObserver, audio::GameAudioCheck>);

static_assert(uint8_t(host::FirefightMode::Pairs) == 0 && uint8_t(host::FirefightMode::Free) == 1);
static_assert(uint8_t(audio::WeaponAudioForm::None) == 0 && uint8_t(audio::WeaponAudioForm::Driver) == 1 &&
		uint8_t(audio::WeaponAudioForm::OneShot) == 2);
static_assert(uint8_t(fx::TriggerPurpose::Muzzle) == 0 && uint8_t(fx::TriggerPurpose::Shell) == 1 && uint8_t(fx::TriggerPurpose::Tracer) == 2 &&
		uint8_t(fx::TriggerPurpose::ImpactChoice) == 3 && uint8_t(fx::TriggerPurpose::ImpactEffect) == 4 &&
		uint8_t(fx::TriggerPurpose::ImpactSpread) == 5 && uint8_t(fx::TriggerPurpose::Decal) == 6 && uint8_t(fx::TriggerPurpose::Detonation) == 7);
static_assert(uint8_t(gs::M2aCase::Range) == 0 && uint8_t(gs::M2aCase::Fx) == 6 && uint8_t(gs::M2aCase::Play) == 7 &&
		uint8_t(gs::M2aCase::Firefight) == 8 && gs::kM2aCaseCount == 9);
static_assert(gs::m2a_case_name(gs::M2aCase::Play) == "play" && gs::m2a_case_name(gs::M2aCase::Firefight) == "firefight");
static_assert(gs::m2a_case_from_name("audio") == gs::M2aCase::Audio && !gs::m2a_case_from_name("Audio").has_value());
static_assert(gs::m2a_case_track(gs::M2aCase::Audio) == gs::M2aTrack::Au3 && gs::m2a_case_track(gs::M2aCase::Fx) == gs::M2aTrack::Fx3 &&
		gs::m2a_case_track(gs::M2aCase::Firefight) == gs::M2aTrack::Bt && gs::m2a_case_track(gs::M2aCase::Play) == gs::M2aTrack::Wave2);
static_assert(uint8_t(host::PresentTimeline::Predicted) == 0 && uint8_t(host::PresentTimeline::Render) == 1 &&
		uint8_t(host::PresentTimeline::Immediate) == 2);

static_assert(audio::kMaxWeaponVoices == 128 && audio::kMaxOneShotVoices == 128 && audio::kGameMixerInstances == 256);
static_assert(fx::kMaxDecals == 256 && fx::kDecalFadeSeconds == 30.0f && fx::kTracerConvergeM == 30.0f);
static_assert(audio::kGameCheckDistances == std::array<float, 6>{ 5.0f, 20.0f, 100.0f, 150.0f, 280.0f, 320.0f });
static_assert(anim::kMaxCrowdWeapons == 8 && anim::kCrowdLanesListenMax == 4);

static_assert(anim::kNoCrowdWeapon == 0xFF);
static_assert(std::is_same_v<decltype(&anim::CrowdEvaluator::weapon_of), uint8_t (anim::CrowdEvaluator::*)(uint32_t) const>);
static_assert(std::is_same_v<decltype(&anim::CrowdEvaluator::weapon_palette),
		std::span<const anim::Mat34> (anim::CrowdEvaluator::*)(uint32_t) const>);
static_assert(std::is_same_v<decltype(&anim::CrowdEvaluator::muzzle_model), bool (anim::CrowdEvaluator::*)(uint32_t, anim::Xform &) const>);

static_assert(anim::crowd_lanes(0, false) == 2 && anim::crowd_lanes(0, true) == 2);
static_assert(anim::crowd_lanes(16, false) == 6 && anim::crowd_lanes(8, false) == 4 && anim::crowd_lanes(4, false) == 2);
static_assert(anim::crowd_lanes(16, true) == 4 && anim::crowd_lanes(12, true) == 4 && anim::crowd_lanes(11, true) == 3);
static_assert(anim::crowd_lanes(10, true) == 2 && anim::crowd_lanes(8, true) == 2 && anim::crowd_lanes(2, true) == 2);

static_assert(anim::crowd_team_of_wire(0) == 0 && anim::crowd_team_of_wire(1) == 0 && anim::crowd_team_of_wire(2) == 1 &&
		anim::crowd_team_of_wire(3) == 0);

static_assert(audio::sound_seed(7, 5) == 0xf90639b22b02a930ull);
static_assert(audio::sound_seed(0, 0) == 0xe220a8397b1dcdafull);
static_assert(audio::sound_seed(7, 5) == sw::splitmix64((uint64_t(7) << 32) | 5));
constexpr uint64_t kEventSeed = 0x0123456789ABCDEFull;
static_assert(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Muzzle, 0) == 0x13182864bd1a9954ull);
static_assert(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::ImpactChoice, 1) == 0x20c584c7b9e3dadcull);
static_assert(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::ImpactEffect, 2) == 0xe876e246dab06e54ull);
static_assert(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Decal, 0) == 0x06baab98ba50a250ull);
static_assert(host::firefight_seed(0x5EED5EED12345678ull, 3) == (0x5EED5EED12345678ull ^ 3u));

net::OtherSoldier other_with(uint8_t p_slot, uint8_t p_generation, uint8_t p_weapon, uint8_t p_burst, uint8_t p_team) {
	net::OtherSoldier o;
	o.slot = p_slot;
	o.generation = p_generation;
	o.weapon = p_weapon;
	o.burst_seq = p_burst;
	o.firing = (p_burst & 1) != 0;
	net::set_other_team(o, p_team);
	return o;
}

host::PresentationFrame two_others_frame() {
	host::PresentationFrame f;
	f.valid = true;
	f.publish_ns = 1'000'000'000ull;
	f.other_count = 3;
	f.others[0].slot = 4;
	f.others[0].count = 2;
	f.others[0].samples[0] = net::TimedOther{ 10, other_with(4, 1, 2, 3, 1) };
	f.others[0].samples[1] = net::TimedOther{ 12, other_with(4, 2, 5, 4, 2) };
	f.others[1].slot = 6;
	f.others[1].count = 0;
	f.others[2].slot = 9;
	f.others[2].count = 1;
	f.others[2].samples[0] = net::TimedOther{ 11, other_with(9, 7, 0xFF, 1, 2) };
	return f;
}

}

TEST_SUITE("m2a-contract3") {
	TEST_CASE("m2a.contract3: defaults of the W3.0 structs (FirefightConfig §10.2, crowd inputs, audio and FX configs)") {
		const host::FirefightConfig ff;
		CHECK(ff.mode == host::FirefightMode::Pairs);
		CHECK(ff.count == 64);
		CHECK_FALSE(ff.observer.valid);
		CHECK(ff.aim_sigma_start_deg == 1.5f);
		CHECK(ff.aim_sigma_end_deg == 0.5f);
		CHECK(ff.aim_sigma_decay_s == 1.0f);
		CHECK(ff.burst_min_rounds == 3);
		CHECK(ff.burst_max_rounds == 6);
		CHECK(ff.burst_pause_min_s == 0.3f);
		CHECK(ff.burst_pause_max_s == 0.6f);
		CHECK(ff.semi_rate_hz == 2.0f);
		CHECK(ff.lane_fan_deg == 35.0f);
		CHECK(ff.lane_min_m == 15.0f);
		CHECK(ff.lane_max_m == 60.0f);
		CHECK(ff.pair_min_m == 20.0f);
		CHECK(ff.pair_max_m == 40.0f);
		CHECK(ff.retarget_ticks == 30);
		CHECK(ff.walk);
		CHECK(ff.stats == nullptr);
		const host::GameServerConfig gc;
		CHECK(gc.firefight == nullptr);

		const anim::CrowdSoldierInput in;
		CHECK(in.pose == 0);
		CHECK(in.target_pose == 0);
		CHECK(in.transition == 0.0f);
		CHECK(in.weapon == 0);
		CHECK(in.fire_seq == 0);
		CHECK_FALSE(in.ads);
		CHECK_FALSE(in.dead);
		const anim::CrowdWeapon cw;
		CHECK(cw.binds == nullptr);
		CHECK(cw.reload == nullptr);
		CHECK(cw.reload_rate == 1.0f);
		CHECK(cw.muzzle_joint == -1);
		CHECK(cw.weapon_local.empty());
		const anim::CrowdTeam team;
		CHECK(team.weapons.empty());

		const audio::WeaponAudioConfig wc;
		CHECK(wc.max_weapon_voices == audio::kMaxWeaponVoices);
		const audio::SimMixerClockConfig cc;
		CHECK(cc.rate == audio::kMixRate);
		CHECK(cc.bias_frames == 0);
		const fx::FxTriggerConfig fc;
		CHECK(fc.tier == fx::kTierHigh);
		CHECK(fc.own_slot == 0xFF);
		CHECK((fc.impacts && fc.decals && fc.tracers && fc.shells));
		const gs::M2aLoadInputs li;
		CHECK(li.db == nullptr);
		CHECK(li.own_team == 1);
		const gs::M2aFrame mf;
		CHECK(mf.events.empty());
		CHECK(mf.muzzles.empty());
		CHECK(mf.own_slot == 0xFF);
		const host::PresentedOtherWeapon pw;
		CHECK_FALSE(pw.valid);
		CHECK(pw.def == sw::kNoWeapon);
		const host::PresentClock pc;
		CHECK((!pc.predicted_valid && !pc.render_valid));
		CHECK(pc.valid(host::PresentTimeline::Immediate));
	}

	TEST_CASE("m2a.contract3: seeds of §4.4 (cl's effect seed pinned as a hash)") {
		CHECK(host::effect_seed(0x5EED5EED12345678ull, 3, 42) == 0xb5be2a90e372ab24ull);
		CHECK(host::effect_seed(0x5EED5EED12345678ull, 3, 42) == sw::splitmix64(0x5EED5EED12345678ull ^ ((uint64_t(3) << 32) | 42u)));

		CHECK(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Muzzle, 0) != fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Shell, 0));
		CHECK(fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Muzzle, 0) != fx::trigger_seed(kEventSeed, fx::TriggerPurpose::Muzzle, 1));
	}

	TEST_CASE("m2a.contract3: G1: the team on the wire maps 1 / 2 and fits 52 others without the own state") {
		net::OtherSoldier o;
		for (const uint8_t t : { uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(200) }) {
			net::set_other_team(o, t);
			CHECK(o.team == (t == 1 || t == 2 ? t : 0));
		}
		net::Snapshot s;
		s.seq = 7;
		s.server_tick = 100;
		for (uint8_t i = 0; i < 127; ++i) {
			s.others.push_back(other_with(uint8_t(i + 1), 1, uint8_t(i % 8), uint8_t(i % 16), uint8_t(1 + i % 2)));
		}
		size_t dropped = 0;
		const std::vector<uint8_t> bytes = net::encode_snapshot(s, &dropped);
		CHECK(bytes.size() <= net::kMaxDatagramBytes);
		CHECK(dropped == 127 - 52);
		const auto d = net::decode_snapshot(bytes);
		REQUIRE(d);
		if (!d) {
			return;
		}
		REQUIRE(d->others.size() == 52);
		for (size_t i = 0; i < d->others.size(); ++i) {
			CHECK(d->others[i].team == uint8_t(1 + i % 2));
		}
	}

	TEST_CASE("m2a.contract3: G2: present_other_weapon draws the fields of the sample present_others draws") {
		host::PresentationFrame f = two_others_frame();

		for (const double rt : { 9.0, 10.0, 10.5, 11.0, 11.999, 12.0, 12.5, 30.0 }) {
			f.render_ticks_at_publish = rt;
			const uint64_t now = f.publish_ns;
			CHECK(host::present_render_ticks(f, now) == doctest::Approx(rt));
			std::array<host::PresentedSoldier, 8> drawn{};
			const size_t n = host::present_others(f, now, drawn);
			std::array<host::PresentedOtherWeapon, 8> weapons{};
			const size_t m = host::present_other_weapons(f, rt, weapons);
			REQUIRE(n == 2);
			REQUIRE(m == n);
			for (size_t i = 0; i < n; ++i) {
				CAPTURE(rt);
				CAPTURE(i);
				CHECK(weapons[i].valid);
				CHECK(weapons[i].slot == drawn[i].slot);
				CHECK(weapons[i].generation == drawn[i].generation);
				const host::PresentedOtherWeapon one = host::present_other_weapon(f, drawn[i].slot, rt);
				CHECK(one.generation == weapons[i].generation);
				CHECK(one.sample_tick == weapons[i].sample_tick);
			}

			const host::PresentedOtherWeapon w4 = host::present_other_weapon(f, 4, rt);
			const bool newer = rt >= 12.0;
			CHECK(w4.generation == (newer ? 2 : 1));
			CHECK(w4.def == (newer ? 5 : 2));
			CHECK(w4.team == (newer ? 2 : 1));
			CHECK(w4.burst_seq == (newer ? 4 : 3));
			CHECK(w4.sample_tick == (newer ? 12u : 10u));

			CHECK(host::present_other_weapon(f, 9, rt).def == sw::kNoWeapon);
		}

		CHECK_FALSE(host::present_other_weapon(f, 6, 11.0).valid);
		CHECK_FALSE(host::present_other_weapon(f, 77, 11.0).valid);
	}

	TEST_CASE("m2a.contract3: the presentation clocks of a frame (Predicted: own tick - 1 + fraction; Render: render ticks)") {
		host::PresentationFrame f;
		CHECK_FALSE(host::present_clock(f, 5).predicted_valid);
		CHECK_FALSE(host::present_clock(f, 5).render_valid);
		f.valid = true;
		f.tick = 100;
		f.tick_fraction = 0.5;
		f.publish_ns = 2'000'000'000ull;
		f.render_ticks_at_publish = 90.25;
		const host::PresentClock c = host::present_clock(f, f.publish_ns);
		REQUIRE(c.predicted_valid);
		REQUIRE(c.render_valid);
		CHECK(c.predicted == 99 * sw::kTimeUnitsPerTick + sw::kTimeUnitsPerTick / 2);
		CHECK(c.render == 90 * sw::kTimeUnitsPerTick + sw::kTimeUnitsPerTick / 4);
		CHECK(c.now(host::PresentTimeline::Predicted) == c.predicted);
		CHECK(c.now(host::PresentTimeline::Render) == c.render);

		const host::PresentClock later = host::present_clock(f, f.publish_ns + 4'000'000ull);
		CHECK(later.render > c.render);
		CHECK(later.predicted >= c.predicted);
	}

	TEST_CASE("m2a.contract3: G3: GameServer::init runs the firefight hook (none: unchanged; a config: bt's bots)") {
		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		if (level == nullptr) {
			return;
		}
		{
			host::GameServer server;
			host::GameServerConfig cfg;
			cfg.level = level;
			cfg.max_slots = 8;
			CHECK(server.init(cfg).ok());
		}
		{
			host::GameServer server;
			host::GameServerConfig cfg;
			cfg.level = level;
			cfg.max_slots = 8;
			auto ff = std::make_shared<host::FirefightConfig>();
			ff->count = 0;
			cfg.firefight = ff;
			CHECK(server.init(cfg).ok());
		}
		host::GameServer server;
		host::GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 16;
		auto ff = std::make_shared<host::FirefightConfig>();
		ff->mode = host::FirefightMode::Free;
		ff->count = 4;
		ff->walk = false;
		cfg.firefight = ff;
		const core::Status st = server.init(cfg);
		if constexpr (kBtLanded) {
			CHECK_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
			CHECK(server.stats().soldiers >= 4);
		} else {

			CHECK_FALSE(st.ok());
			if (!st.ok()) {
				CHECK(st.error().message.find("W3.0 stub") != std::string::npos);
			}
			pending("bt", "GameServer::init adds FirefightConfig::count bots", "firefight_brain.cpp is W3.0's stub");
		}
	}

	TEST_CASE("m2a.contract3: G4 and G5: send_kit_select before start; make_client_gunplay_inputs without gunplay tables") {
		{
			host::ClientSession session;
			net::KitSelect k;
			k.kit = 2;
			k.team = 1;
			for (int i = 0; i < 40; ++i) {
				session.send_kit_select(k);
			}
			session.stop();
		}
		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->gunplay == nullptr);
		const core::Result<std::shared_ptr<const host::ClientGunplayInputs>> in = host::make_client_gunplay_inputs(*level);
		CHECK_FALSE(in);
		if (!in) {
			CHECK(in.error().message.find("SharedLoadOptions::gunplay") != std::string::npos);
		}
	}

	TEST_CASE("m2a.contract3: the M2a check's case parser refuses hostile command lines") {
		const auto two = gs::parse_m2a_cases("range,reload");
		REQUIRE(two);
		if (two) {
			CHECK((*two == std::vector<gs::M2aCase>{ gs::M2aCase::Range, gs::M2aCase::Reload }));
		}
		const auto all = gs::parse_m2a_cases("all");
		REQUIRE(all);
		if (all) {
			CHECK(all->size() == gs::kM2aAllCases.size());
		}
		const auto play = gs::parse_m2a_cases("play");
		REQUIRE(play);
		if (play) {
			CHECK((*play == std::vector<gs::M2aCase>{ gs::M2aCase::Play }));
		}
		for (const char *bad : { "", ",", "range,", ",range", "range,,fx", "range,range", "Range", "all,range", "nope", "fx;audio" }) {
			CAPTURE(bad);
			CHECK_FALSE(gs::parse_m2a_cases(bad));
		}
	}

	TEST_CASE("m2a.contract3: tp's 3p weapon route: strip the team weapon part, CrowdTeam::weapons views, def -> index") {
		render::SoldierAssets assets;
		for (render::SoldierTeam &team : assets.teams) {
			render::SoldierPart body;
			body.prepared.vertices = 100;
			body.prepared.triangles = 50;
			render::SoldierPart weapon;
			weapon.weapon = true;
			weapon.prepared.vertices = 30;
			weapon.prepared.triangles = 20;
			team.parts.push_back(body);
			team.parts.push_back(weapon);
			team.vertices = 130;
			team.triangles = 70;
		}
		CHECK(render::strip_team_weapon_parts(assets) == 2);
		CHECK(render::strip_team_weapon_parts(assets) == 0);
		for (const render::SoldierTeam &team : assets.teams) {
			REQUIRE(team.parts.size() == 1);
			CHECK_FALSE(team.parts[0].weapon);
			CHECK(team.vertices == 100);
			CHECK(team.triangles == 50);
		}

		render::CrowdWeaponSet set;
		for (uint16_t d = 0; d < 10; ++d) {
			render::CrowdWeaponPart part;
			part.spec.def = uint16_t(100 + d);
			part.have_reload = (d % 2) == 0;
			part.reload_rate = 1.0f + float(d);
			part.muzzle_joint = int32_t(d);
			part.weapon_local.assign(3, anim::xform_identity());
			if (d == 1) {
				part.weapon_joints.weight.assign(3, 1.0f);
			}
			set.teams[0].push_back(std::move(part));
		}
		const std::vector<anim::CrowdWeapon> us = render::crowd_weapons_of(set, 0);
		REQUIRE(us.size() == anim::kMaxCrowdWeapons);
		CHECK(us[0].binds == &set.teams[0][0].binds);
		CHECK(us[0].reload == &set.teams[0][0].reload);
		CHECK(us[1].reload == nullptr);
		CHECK(us[0].weapon_joints == nullptr);
		CHECK(us[1].weapon_joints == &set.teams[0][1].weapon_joints);
		CHECK(us[3].reload_rate == 4.0f);
		CHECK(us[5].muzzle_joint == 5);
		CHECK(us[2].weapon_local.size() == 3);
		CHECK(render::crowd_weapons_of(set, 1).empty());
		CHECK(render::crowd_weapons_of(set, 2).empty());
		CHECK(render::crowd_weapon_index(set, 0, 100) == 0);
		CHECK(render::crowd_weapon_index(set, 0, 107) == 7);
		CHECK(render::crowd_weapon_index(set, 0, 108) == -1);
		CHECK(render::crowd_weapon_index(set, 1, 100) == -1);
		CHECK(render::crowd_weapon_index(set, 5, 100) == -1);
		CHECK(set.texture("none") == nullptr);
	}

	TEST_CASE("m2a.contract3: the tracks' bodies (pending until each lands)") {
		if constexpr (kAu3Landed) {

			audio::MixerCore mixer(audio::MixerConfig{});
			audio::SimMixerClock clock(mixer);
			host::PresentClock now;
			now.predicted_valid = now.render_valid = true;
			now.predicted = 100 * sw::kTimeUnitsPerTick;
			now.render = 90 * sw::kTimeUnitsPerTick;
			clock.set_now(now);
			CHECK(clock.frame_of(0, host::PresentTimeline::Immediate) == mixer.earliest_frame());
			CHECK(clock.frame_of(now.predicted - sw::kTimeUnitsPerSecond, host::PresentTimeline::Predicted) == mixer.earliest_frame());
			CHECK(clock.late() == 1);
			CHECK(clock.frame_of(now.render + sw::kTimeUnitsPerSecond / 2, host::PresentTimeline::Render) == clock.heard_frame() + audio::kMixRate / 2);
		} else {
			pending("au3", "SimMixerClock::frame_of on the two timelines", "weapon_audio.cpp is au3's");
		}
		if constexpr (kFx3Landed) {
			fx::FxWorld world;
			fx::EffectLibrary::Sources sources;
			fx::EffectLibrary library(sources);
			fx::FxTriggers triggers(world, library, std::make_shared<const fx::FireEffectTable>());
			triggers.on_muzzles({});
			triggers.on_events({}, host::PresentClock{}, fx::FxView{});
			CHECK(triggers.stats().events == 0);
			CHECK(triggers.decals().empty());
			CHECK(triggers.sounds().empty());
		} else {
			pending("fx3", "FxTriggers on an empty frame", "triggers.cpp is fx3's");
		}
		if constexpr (kTpLanded) {
			MESSAGE("tp: CrowdEvaluator::muzzle_model, weapon_of, weapon_palette and load_crowd_weapons are covered by anim.s3.unit's two-weapon block");
		} else {
			pending("tp", "CrowdEvaluator::muzzle_model, weapon_of, weapon_palette; load_crowd_weapons", "crowd_eval.cpp's bodies and crowd_weapons.cpp are tp's");
		}
	}
}
