#include <doctest/doctest.h>

#include "bf3/anim/layers.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/sim/command.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include "test_support.h"
#include "weapons_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;
using anim::VmClip;

#ifndef OPENBF3_VIEWMODEL_TUNING_CFG
#error "OPENBF3_VIEWMODEL_TUNING_CFG must name bf3-opensource/config/viewmodel_tuning.cfg"
#endif

namespace m2a_fake {

struct Presented {
	double time_s = 0;
	uint8_t phase = 0;
	double phase_start_s = 0, phase_end_s = 0;
	uint8_t flags = 0;
	int32_t magazine = 0;
	sw::RecoilOffset recoil{};
	bool zoomed = false;
};

inline Presented present(const sw::SoldierWeaponsState &p_prev, const sw::SoldierWeaponsState &p_curr, double p_fraction, double p_time_s) {
	Presented p;
	const sw::WeaponSlotState &a = p_prev.current();
	const sw::WeaponSlotState &b = p_curr.current();
	const float f = float(p_fraction);
	p.time_s = p_time_s;
	p.phase = uint8_t(b.phase);
	p.phase_start_s = sw::units_to_seconds(b.phase_start);
	p.phase_end_s = sw::units_to_seconds(b.phase_end);
	p.flags = b.flags;
	p.magazine = b.magazine;
	p.recoil.pitch = a.recoil.pitch + (b.recoil.pitch - a.recoil.pitch) * f;
	p.recoil.yaw = a.recoil.yaw + (b.recoil.yaw - a.recoil.yaw) * f;
	p.zoomed = (b.flags & sw::slot_flags::kZoomWasDown) != 0;
	return p;
}

}

namespace {

constexpr uint64_t kSeed = 0x766d2d756e6974ull;
constexpr float kFrameRates[] = { 30.0f, 60.0f, 144.0f, 240.0f };

std::string read_file(const char *p_path) {
	std::ifstream f(p_path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

anim::ViewmodelTuning shipped_tuning() {
	const std::string text = read_file(OPENBF3_VIEWMODEL_TUNING_CFG);
	REQUIRE_MESSAGE(!text.empty(), "viewmodel_tuning.cfg not found at " << OPENBF3_VIEWMODEL_TUNING_CFG);
	core::Result<anim::ViewmodelTuning> t = anim::parse_viewmodel_tuning(text);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return t.ok() ? *t : anim::ViewmodelTuning{};
}

struct Rig {
	anim::Skeleton skeleton;
	std::vector<core::ant::DecodedClip> clips;
	std::vector<anim::ClipTracks> tracks;
	std::array<anim::VmClipRef, anim::kVmClips> refs{};
};

anim::Skeleton three_bones() {
	std::vector<anim::Bone> bones(3);
	bones[0].name = "Root";
	bones[1].name = "Arm";
	bones[1].parent = 0;
	bones[1].rest.t = { 0, 0.1f, 0 };
	bones[2].name = "Wep_Root";
	bones[2].parent = 1;
	bones[2].rest.t = { 0, 0, 0.3f };
	core::Result<anim::Skeleton> s = anim::Skeleton::make(std::move(bones));
	REQUIRE(s.ok());
	return s.ok() ? std::move(*s) : anim::Skeleton{};
}

void add_clip(Rig &r_rig, VmClip p_role, float p_deg0, float p_deg1, uint16_t p_frames, bool p_stand_in = false) {
	core::ant::DecodedClip c;
	c.codec = core::ant::ClipCodec::Raw;
	c.num_quats = 1;
	c.end_frame = p_frames;
	auto q = [](float p_deg) {
		const float h = p_deg * 3.14159265f / 360.0f;
		return core::ant::Quat{ 0, std::sin(h), 0, std::cos(h) };
	};
	if (p_frames == 0) {
		c.num_keys = 1;
		c.key_times = { 0 };
		c.quats = { q(p_deg0) };
	} else {
		c.num_keys = 2;
		c.key_times = { 0, p_frames };
		c.quats = { q(p_deg0), q(p_deg1) };
	}
	anim::ClipTracks t;
	t.rotation_bone = { 1 };
	t.fps = 30.0f;
	t.end_frame = p_frames;
	t.channels_bound = 1;
	r_rig.clips.push_back(std::move(c));
	r_rig.tracks.push_back(t);
	anim::VmClipRef &ref = r_rig.refs[size_t(p_role)];
	ref.seconds = t.duration_seconds();
	ref.stand_in = p_stand_in;
}

void finish_rig(Rig &r_rig, const std::vector<VmClip> &p_roles) {
	for (size_t i = 0; i < p_roles.size(); ++i) {
		anim::VmClipRef &ref = r_rig.refs[size_t(p_roles[i])];
		ref.clip = &r_rig.clips[i];
		ref.tracks = &r_rig.tracks[i];
	}
}

Rig rifle_rig(uint16_t p_reload_frames, bool p_fire_clip) {
	Rig r;
	r.skeleton = three_bones();
	std::vector<VmClip> roles;
	auto add = [&](VmClip p_role, float p_a, float p_b, uint16_t p_frames, bool p_stand_in = false) {
		add_clip(r, p_role, p_a, p_b, p_frames, p_stand_in);
		roles.push_back(p_role);
	};
	add(VmClip::Idle, 0, 0, 0);
	add(VmClip::ZoomIdle, 40, 40, 0);
	add(VmClip::Reload, 0, 90, p_reload_frames);
	add(VmClip::ReloadEmpty, 0, 120, uint16_t(p_reload_frames + 10));
	add(VmClip::Deploy, -60, 0, 20);
	add(VmClip::ZoomIn, 0, 40, 15);
	add(VmClip::ZoomOut, 40, 0, 9);
	if (p_fire_clip) {
		add(VmClip::Fire, 0, 5, 6);
	}
	finish_rig(r, roles);
	return r;
}

float bone1_yaw_deg(const anim::LocalPose &p_pose) {
	const anim::Quat &q = p_pose.joints[1].q;
	return float(2.0 * std::atan2(double(q.y), double(q.w)) * 180.0 / 3.14159265358979);
}

sim::Command command(sim::Tick p_tick, const sw::WeaponInput &p_in) {
	sim::Command c;
	c.tick = p_tick;
	c.sequence = p_tick;
	c.buttons = sw::with_weapon_input(0, p_in);
	return c;
}

struct SimRun {
	sim::Tick spawn = 0;
	std::vector<sw::SoldierWeaponsState> states;
	std::vector<bool> sprinting;
	std::vector<sw::TimedWeaponEvent> timed;
	std::vector<sw::ShotEvent> shots;
};

template <typename InputFn, typename SprintFn>
SimRun run_sim(const sw::WeaponTable &p_table, const std::string &p_variant, uint32_t p_ticks, InputFn p_input, SprintFn p_sprint, bool p_m2a_rules = false) {
	SimRun run;
	run.spawn = 100;
	const uint16_t def = tw::id_of(p_table, p_variant);
	const uint16_t loadout[3] = { def, sw::kNoWeapon, sw::kNoWeapon };
	sw::SoldierWeaponsState st;
	const core::Status spawned = sw::spawn_soldier_weapons(st, p_table, loadout, 0, run.spawn);
	REQUIRE(spawned.ok());
	run.states.push_back(st);
	run.sprinting.push_back(false);
	sw::WeaponTickOutput out;
	for (uint32_t i = 1; i <= p_ticks; ++i) {
		const sim::Tick t = run.spawn + i;
		const sw::WeaponInput in = p_input(t);
		sw::WeaponContext ctx;
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = ctx.eye_start;
		ctx.sprinting = p_sprint(t);
		ctx.sway.move = ctx.sprinting ? sw::SwayMove::Sprinting : sw::SwayMove::Base;
		ctx.sway.zoom = in.zoom ? sw::Zoom::Zoom : sw::Zoom::NoZoom;
		if (p_m2a_rules) {
			sw::step_weapons_with_rules(p_table, st, command(t, in), ctx, kSeed, run.spawn, out);
		} else {
			sw::step_soldier_weapons(p_table, st, command(t, in), ctx, kSeed, out);
		}
		run.states.push_back(st);
		run.sprinting.push_back(ctx.sprinting);
		for (uint8_t k = 0; k < out.timed_count; ++k) {
			run.timed.push_back(out.timed[k]);
		}
		for (uint8_t k = 0; k < out.shot_count; ++k) {
			run.shots.push_back(out.shots[k]);
		}
	}
	return run;
}

m2a_fake::Presented present_at(const SimRun &p_run, double p_t, bool *r_sprinting = nullptr) {
	const double tick = 1.0 / double(sim::kSimHz);
	const double t0 = sw::units_to_seconds(sw::tick_end(p_run.spawn));
	double rel = (p_t - t0) / tick;
	rel = std::clamp(rel, 0.0, double(p_run.states.size() - 1) - 1e-9);
	const size_t i = size_t(std::floor(rel));
	const size_t j = std::min(i + 1, p_run.states.size() - 1);
	if (r_sprinting) {
		*r_sprinting = p_run.sprinting[j];
	}
	return m2a_fake::present(p_run.states[i], p_run.states[j], rel - double(i), p_t);
}

std::vector<anim::VmCue> cues_between(const SimRun &p_run, double p_from, double p_to) {
	std::vector<anim::VmCue> out;
	struct Timed {
		double t;
		anim::VmCue cue;
	};
	std::vector<Timed> all;
	for (const sw::TimedWeaponEvent &e : p_run.timed) {
		const double t = sw::units_to_seconds(e.time);
		if (!(t > p_from && t <= p_to)) {
			continue;
		}
		anim::VmCue c;
		c.time_s = t;
		bool keep = true;
		switch (e.event) {
			case sw::weapon_events::kDeployStarted:
				c.kind = anim::VmCue::Kind::DeployStarted;
				break;
			case sw::weapon_events::kDeployFinished:
				c.kind = anim::VmCue::Kind::DeployFinished;
				break;
			case sw::weapon_events::kReloadStarted: {
				c.kind = anim::VmCue::Kind::ReloadStarted;
				const m2a_fake::Presented p = present_at(p_run, t);
				c.empty_reload = p.magazine == 0;
				break;
			}
			case sw::weapon_events::kReloadCommitted:
				c.kind = anim::VmCue::Kind::ReloadCommitted;
				break;
			case sw::weapon_events::kReloadFinished:
				c.kind = anim::VmCue::Kind::ReloadFinished;
				break;
			case sw::weapon_events::kReloadCancelled:
				c.kind = anim::VmCue::Kind::ReloadCancelled;
				break;
			case sw::weapon_events::kBoltStarted:
				c.kind = anim::VmCue::Kind::BoltStarted;
				break;
			default:
				keep = false;
		}
		if (keep) {
			all.push_back({ t, c });
		}
	}
	for (const sw::ShotEvent &s : p_run.shots) {
		const double t = sw::units_to_seconds(s.time);
		if (t > p_from && t <= p_to) {
			anim::VmCue c;
			c.kind = anim::VmCue::Kind::Shot;
			c.time_s = t;
			all.push_back({ t, c });
		}
	}
	std::stable_sort(all.begin(), all.end(), [](const Timed &p_a, const Timed &p_b) { return p_a.t < p_b.t; });
	for (const Timed &t : all) {
		out.push_back(t.cue);
	}
	return out;
}

double first_event(const SimRun &p_run, uint32_t p_event, double p_after = -1.0) {
	for (const sw::TimedWeaponEvent &e : p_run.timed) {
		const double t = sw::units_to_seconds(e.time);
		if (e.event == p_event && t > p_after) {
			return t;
		}
	}
	return -1.0;
}

double settle_time(const anim::SpringParams &p_p, anim::SpringState p_state, float p_target, float p_dt) {
	const int frames = int(std::ceil(3.0 / p_dt));
	double last_moving = 0.0;
	for (int i = 1; i <= frames; ++i) {
		anim::step_spring(p_p, p_target, p_dt, p_state);
		if (!anim::spring_at_rest(p_p, p_target, p_state)) {
			last_moving = double(i) * p_dt;
		}
	}
	return last_moving + p_dt;
}

}

TEST_SUITE("anim-viewmodel-unit") {
	TEST_CASE("anim.viewmodel: tuning file round-trips; the shipped viewmodel_tuning.cfg parses") {
		const anim::ViewmodelTuning d;
		core::Result<anim::ViewmodelTuning> back = anim::parse_viewmodel_tuning(anim::format_viewmodel_tuning(d));
		REQUIRE(back.ok());
		CHECK(*back == d);
		CHECK(back->sway_max == d.sway_max);
		CHECK(back->fov_transition_specific == d.fov_transition_specific);
		const anim::ViewmodelTuning shipped = shipped_tuning();
		core::Result<anim::ViewmodelTuning> again = anim::parse_viewmodel_tuning(anim::format_viewmodel_tuning(shipped));
		REQUIRE(again.ok());
		CHECK(*again == shipped);
		CHECK(again->camera_kick_pitch_deg == shipped.camera_kick_pitch_deg);
		CHECK(again->sprint_blend_seconds == shipped.sprint_blend_seconds);
		std::printf("anim.viewmodel: shipped tuning: sway %.0f/%.0f, bob %.0f/%.0f, kick %.0f/%.0f, camera %.0f/%.0f (stiffness/damping)\n",
				double(shipped.sway.stiffness), double(shipped.sway.damping), double(shipped.bob.stiffness), double(shipped.bob.damping),
				double(shipped.kick.stiffness), double(shipped.kick.damping), double(shipped.camera_recoil.stiffness), double(shipped.camera_recoil.damping));
	}

	TEST_CASE("anim.viewmodel: hostile tuning text fails with the line number and never crashes") {
		struct Bad {
			const char *text;
			const char *needle;
		};
		const Bad bad[] = {
			{ "sway_scale = 0.01\nnot_a_key = 1\n", "line 2" },
			{ "sway_scale = 0.01\nsway_scale = 0.02\n", "given twice" },
			{ "kick_back = abc\n", "needs a number" },
			{ "kick_back = 1e30\n", "outside" },
			{ "kick_back = nan\n", "needs a number" },
			{ "kick_back = inf\n", "needs a number" },
			{ "kick.stiffness = -5\n", "outside" },
			{ "kick.damping = 1001\n", "outside" },
			{ "fov_transition_specific = yes\n", "true or false" },
			{ "sway.settle_seconds\n", "expected `key = value`" },
			{ "\n\n\nads_in_seconds = 0\n", "line 4" },
			{ "kick_back = 0.01 0.02\n", "needs a number" },
		};
		for (const Bad &b : bad) {
			core::Result<anim::ViewmodelTuning> r = anim::parse_viewmodel_tuning(b.text);
			CHECK_MESSAGE(!r.ok(), b.text);
			if (!r.ok()) {
				CHECK_MESSAGE(r.error().message.find(b.needle) != std::string::npos, r.error().message);
			}
		}

		core::Result<anim::ViewmodelTuning> ok = anim::parse_viewmodel_tuning("# c\r\n\r\nkick_back = 0.03 # trailing\r\nsway_max=0.04");
		REQUIRE(ok.ok());
		CHECK(ok->kick_back == 0.03f);
		CHECK(ok->sway_max == 0.04f);

		std::string junk(100000, '\x01');
		core::Result<anim::ViewmodelTuning> j = anim::parse_viewmodel_tuning(junk);
		REQUIRE(!j.ok());
		CHECK(j.error().message.size() < 200);

		const std::string base = read_file(OPENBF3_VIEWMODEL_TUNING_CFG);
		std::mt19937_64 rng(kSeed);
		uint32_t failed = 0, parsed = 0;
		for (int i = 0; i < 3000; ++i) {
			std::string s = base;
			const int edits = 1 + int(rng() % 8);
			for (int e = 0; e < edits && !s.empty(); ++e) {
				const size_t at = size_t(rng() % s.size());
				switch (rng() % 3) {
					case 0:
						s[at] = char(rng() & 0xFF);
						break;
					case 1:
						s.erase(at, 1 + size_t(rng() % 16));
						break;
					default:
						s.insert(at, 1, "=.#\n-e9"[rng() % 7]);
						break;
				}
			}
			core::Result<anim::ViewmodelTuning> r = anim::parse_viewmodel_tuning(s);
			if (r.ok()) {
				++parsed;
				CHECK(std::isfinite(r->kick.stiffness));
				CHECK(r->kick.stiffness >= 0.1f);
				CHECK(r->ads_in_seconds >= 0.01f);
			} else {
				++failed;
			}
		}
		std::printf("anim.viewmodel: tuning fuzz: 3000 mutations, %u parsed, %u failed cleanly\n", parsed, failed);
	}

	TEST_CASE("anim.viewmodel: E19 springs come to rest within settle_seconds at 30 / 60 / 144 / 240 fps") {
		const anim::ViewmodelTuning t = shipped_tuning();
		struct Case {
			const char *name;
			const anim::SpringParams *p;
			float impulse_peak;
			float step_from;
		};
		const Case cases[] = {
			{ "sway", &t.sway, 0.0f, t.sway_max },
			{ "bob", &t.bob, 0.0f, t.bob_amplitude * 1.5f },
			{ "kick", &t.kick, std::max(t.kick_back, t.kick_pitch_deg), 0.0f },
			{ "camera_recoil", &t.camera_recoil, t.camera_kick_pitch_deg, 0.0f },
		};
		for (const Case &c : cases) {
			double worst = 0;
			for (const float fps : kFrameRates) {
				const float dt = 1.0f / fps;
				double settle = 0;
				if (c.impulse_peak > 0.0f) {

					anim::SpringState s;
					s.v = anim::spring_impulse_for_peak(*c.p, c.impulse_peak);
					settle = std::max(settle, settle_time(*c.p, s, 0.0f, dt));

					anim::SpringState q;
					q.v = s.v;
					float peak = 0;
					for (int i = 0; i < int(fps); ++i) {
						anim::step_spring(*c.p, 0.0f, dt, q);
						peak = std::max(peak, q.x);
					}
					CHECK_MESSAGE(std::fabs(peak - c.impulse_peak) <= 0.05f * c.impulse_peak + 1e-4f, c.name << " peak " << peak << " at " << fps);

					anim::SpringState b;
					double next = 0, now = 0;
					int fired = 0;
					while (fired < 10) {
						while (fired < 10 && next <= now) {
							b.v += anim::spring_impulse_for_peak(*c.p, c.impulse_peak);
							next += 0.075;
							++fired;
						}
						anim::step_spring(*c.p, 0.0f, dt, b);
						now += dt;
					}
					CHECK(std::isfinite(b.x));
					settle = std::max(settle, settle_time(*c.p, b, 0.0f, dt));
				} else {
					anim::SpringState s;
					s.x = c.step_from;
					settle = std::max(settle, settle_time(*c.p, s, 0.0f, dt));
				}
				worst = std::max(worst, settle);
				CHECK_MESSAGE(settle <= double(c.p->settle_seconds) + 1e-6, c.name << " settles in " << settle << " s at " << fps << " fps, configured "
																				 << c.p->settle_seconds);
			}
			std::printf("anim.viewmodel: E19 spring %-13s at rest after %.3f s (worst of 30/60/144/240 fps), configured %.2f s\n", c.name, worst,
					double(c.p->settle_seconds));
		}

		for (const anim::SpringParams p : { anim::SpringParams{ 10000, 0, 1e-3f, 1 }, anim::SpringParams{ 0.1f, 1000, 1e-3f, 1 },
					 anim::SpringParams{ 10000, 1000, 1e-3f, 1 } }) {
			anim::SpringState s;
			s.v = 1000;
			for (int i = 0; i < 400; ++i) {
				anim::step_spring(p, 1.0f, 0.25f, s);
			}
			CHECK(std::isfinite(s.x));
			CHECK(std::isfinite(s.v));
			CHECK(std::fabs(s.x) < 1e3f);
		}

		anim::SpringState n{ std::nanf(""), 0 };
		anim::step_spring(t.kick, 0.0f, 0.016f, n);
		CHECK(std::isfinite(n.x));
	}

	TEST_CASE("anim.viewmodel: E19 ADS world FOV 55 -> 40 in the transition time +- 1 frame and back; viewmodel FOV 55 at hip") {
		for (const bool specific : { true, false }) {
			anim::AdsParams p;
			p.hip_fov = 55;
			p.zoom_fov = 40;
			p.fov_in_seconds = specific ? 0.12f : 0.15f;
			p.fov_out_seconds = 0.15f;
			p.zoom_in_seconds = 0.09f;
			p.zoom_out_seconds = 0.15f;
			p.render_fov = 55;
			p.zoom_render_fov = 15;
			for (const float fps : kFrameRates) {
				const float dt = 1.0f / fps;
				anim::AdsCamera cam;
				const double press = 0.5 + 0.37 * dt;
				const double release = 2.0 + 0.61 * dt;
				double reached_in = -1, reached_out = -1;
				bool hip_ok = true;
				bool monotonic = true;
				float last = 55;
				for (int i = 1; i <= int(3.0 * fps); ++i) {
					const double now = double(i) * dt;
					const bool zoomed = now >= press && now < release;
					const anim::AdsFrame f = cam.step(p, zoomed, dt);
					if (now < press) {
						hip_ok = hip_ok && f.world_fov == 55.0f && f.viewmodel_fov == 55.0f && f.look_scale == 1.0f;
					}
					if (zoomed && reached_in < 0 && f.world_fov == 40.0f) {
						reached_in = now - press;
					}
					if (!zoomed && now >= release && reached_out < 0 && f.world_fov == 55.0f) {
						reached_out = now - release;
						hip_ok = hip_ok && f.viewmodel_fov == 55.0f;
					}
					if (zoomed) {
						monotonic = monotonic && f.world_fov <= last;
					}
					last = f.world_fov;
					if (zoomed && now - press > 0.3) {
						CHECK(f.look_scale == doctest::Approx(0.5f));
						CHECK(f.viewmodel_fov == doctest::Approx(15.0f));
						CHECK(f.blend == 1.0f);
					}
				}
				const double tin = p.fov_in_seconds, tout = p.fov_out_seconds;
				CHECK(hip_ok);
				CHECK(monotonic);
				CHECK_MESSAGE(std::fabs(reached_in - tin) <= double(dt) + 1e-6, "in " << reached_in << " at " << fps);
				CHECK_MESSAGE(std::fabs(reached_out - tout) <= double(dt) + 1e-6, "out " << reached_out << " at " << fps);
				std::printf("anim.viewmodel: E19 ADS %s %5.0f fps: 55 -> 40 after %.4f s (target %.2f +- %.4f), 40 -> 55 after %.4f s (target %.2f)\n",
						specific ? "specific" : "generic ", double(fps), reached_in, tin, double(dt), reached_out, tout);
			}
		}

		anim::AdsParams none;
		none.can_zoom = false;
		anim::AdsCamera cam;
		for (int i = 0; i < 60; ++i) {
			CHECK(cam.step(none, true, 1.0f / 60.0f).world_fov == 55.0f);
		}
	}

	TEST_CASE("anim.viewmodel: E19 the camera's recoil offset equals the interpolated predicted recoil within 1e-4 deg") {
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
		auto fire = [](sim::Tick p_t) {
			sw::WeaponInput in;
			in.fire = p_t > 160 && p_t < 260;
			return in;
		};
		const SimRun run = run_sim(*table, "weapons/m16a4/u_m16a4", 400, fire, [](sim::Tick) { return false; });
		CHECK(run.shots.size() > 10);
		const anim::ViewmodelTuning tuning = shipped_tuning();
		Rig rig = rifle_rig(60, false);
		anim::ViewmodelAnimator animator(rig.skeleton, rig.refs, tuning);
		for (const float fps : kFrameRates) {
			animator.reset();
			const float dt = 1.0f / fps;
			const double t0 = sw::units_to_seconds(sw::tick_end(run.spawn));
			double worst = 0, max_recoil = 0;
			uint32_t frames = 0;
			anim::LocalPose pose;
			anim::ViewmodelOutput out;
			for (double now = t0 + dt; now < t0 + 6.0; now += dt) {
				const m2a_fake::Presented p = present_at(run, now);
				const std::vector<anim::VmCue> cues = cues_between(run, now - dt, now);
				anim::ViewmodelFrameInput in;
				in.time_s = now;
				in.dt = dt;
				in.phase = p.phase;
				animator.step(in, cues, pose, out);
				const double view_yaw = 0.3 + 0.2 * std::sin(now), view_pitch = 0.1 * std::cos(now);
				const anim::ViewAngles a = anim::camera_view_angles(view_yaw, view_pitch, p.recoil.pitch, p.recoil.yaw, out.camera_recoil_pitch_deg,
						out.camera_recoil_yaw_deg);
				constexpr double kDeg = 3.14159265358979323846 / 180.0;
				const double rp = (a.pitch - view_pitch) / kDeg - double(out.camera_recoil_pitch_deg);
				const double ry = (a.yaw - view_yaw) / kDeg - double(out.camera_recoil_yaw_deg);
				worst = std::max({ worst, std::fabs(rp - double(p.recoil.pitch)), std::fabs(ry - double(p.recoil.yaw)) });
				max_recoil = std::max(max_recoil, double(std::fabs(p.recoil.pitch)));
				++frames;
			}
			CHECK(max_recoil > 0.2);
			CHECK_MESSAGE(worst <= 1e-4, worst);
			std::printf("anim.viewmodel: E19 recoil %5.0f fps: %u frames, max predicted pitch %.3f deg, worst |camera - predicted| %.2e deg\n",
					double(fps), frames, max_recoil, worst);
		}

		const anim::ViewAngles c = anim::camera_view_angles(0, 1.57, 5.0f, 0, 0, 0);
		CHECK(c.pitch == doctest::Approx(89.99 * 3.14159265358979323846 / 180.0));
	}

	TEST_CASE("anim.viewmodel: E5 the reload clip starts within 1 frame of kReloadStarted; fire again within 1 frame of the finish") {
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();

		const sim::Tick fire_from = 160, reload_at = 200;
		auto input = [&](sim::Tick p_t) {
			sw::WeaponInput in;
			in.fire = (p_t >= fire_from && p_t < fire_from + 36) || p_t >= reload_at + 60;
			in.reload = p_t == reload_at;
			return in;
		};
		const SimRun run = run_sim(*table, "weapons/m16a4/u_m16a4", 600, input, [](sim::Tick) { return false; });
		const double started = first_event(run, sw::weapon_events::kReloadStarted);
		const double finished = first_event(run, sw::weapon_events::kReloadFinished);
		REQUIRE(started > 0);
		REQUIRE(finished > started);
		double first_shot_after = -1;
		for (const sw::ShotEvent &s : run.shots) {
			const double t = sw::units_to_seconds(s.time);
			if (t >= finished) {
				first_shot_after = t;
				break;
			}
		}
		REQUIRE(first_shot_after > 0);
		const double reload_time = finished - started;
		for (const bool fire_clip : { false, true }) {

			const uint16_t frames = uint16_t(std::lround((reload_time + 0.5) * 30.0));
			Rig rig = rifle_rig(frames, fire_clip);
			anim::ViewmodelAnimator animator(rig.skeleton, rig.refs, shipped_tuning());
			for (const float fps : kFrameRates) {
				animator.reset();
				const float dt = 1.0f / fps;
				const double t0 = sw::units_to_seconds(sw::tick_end(run.spawn));
				double clip_seen = -1, shot_seen = -1, clip_start = -1;
				float reload_weight_after_cut = 1.0f;
				anim::LocalPose pose;
				anim::ViewmodelOutput out;
				for (double now = t0 + dt; now < t0 + 10.0; now += dt) {
					const m2a_fake::Presented p = present_at(run, now);
					const std::vector<anim::VmCue> cues = cues_between(run, now - dt, now);
					anim::ViewmodelFrameInput in;
					in.time_s = now;
					in.dt = dt;
					in.phase = p.phase;
					in.phase_start_s = p.phase_start_s;
					in.phase_end_s = p.phase_end_s;
					const uint32_t shots_before = out.shots;
					animator.step(in, cues, pose, out);
					if (clip_seen < 0 && out.action_clip == uint32_t(VmClip::Reload)) {
						clip_seen = now;
						clip_start = out.action_start_s;
						CHECK(out.action_time_s == doctest::Approx(float(now - started)).epsilon(1e-3));
					}
					if (now >= finished && shot_seen < 0 && out.shots > shots_before) {
						shot_seen = now;
					}
					if (shot_seen >= 0 && now >= shot_seen + double(anim::kViewmodelCrossfadeSeconds) + 1e-6) {
						if (out.action_clip == uint32_t(VmClip::Reload)) {
							reload_weight_after_cut = std::min(reload_weight_after_cut, out.action_weight);
						} else {
							reload_weight_after_cut = 0.0f;
						}
					}
				}
				CHECK(clip_start == doctest::Approx(started));
				CHECK_MESSAGE(clip_seen - started >= 0.0, clip_seen);
				CHECK_MESSAGE(clip_seen - started < double(dt) + 1e-9, "reload clip seen " << (clip_seen - started) << " s after kReloadStarted");
				CHECK_MESSAGE(first_shot_after - finished <= double(dt) + 1e-9, "first shot " << (first_shot_after - finished) << " s after the finish");
				CHECK_MESSAGE(shot_seen - first_shot_after < double(dt) + 1e-9, "shot presented " << (shot_seen - first_shot_after) << " s late");
				CHECK_MESSAGE(reload_weight_after_cut <= 1e-6f, "the reload clip still weighs " << reload_weight_after_cut << " after the cut");
				std::printf("anim.viewmodel: E5 %s %5.0f fps: ReloadTime %.3f s, clip %.3f s; clip on screen %.4f s after kReloadStarted, first shot "
							"%.4f s after kReloadFinished (presented %.4f s later), clip cut within %.2f s\n",
						fire_clip ? "fire clip   " : "no fire clip", double(fps), reload_time, double(frames) / 30.0, clip_seen - started,
						first_shot_after - finished, shot_seen - first_shot_after, double(anim::kViewmodelCrossfadeSeconds));
			}
		}
	}

	TEST_CASE("anim.viewmodel: E19 zoom refused while deploying or sprinting (presentation rule; the sim rule is sv's)") {
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
		const sim::Tick sprint_from = 200, sprint_to = 260;
		auto zoom = [](sim::Tick) {
			sw::WeaponInput in;
			in.zoom = true;
			return in;
		};
		auto sprinting = [&](sim::Tick p_t) { return p_t >= sprint_from && p_t <= sprint_to; };
		for (const bool m2a_rules : { false, true }) {
			const SimRun run = run_sim(*table, "weapons/m16a4/u_m16a4", 400, zoom, sprinting, m2a_rules);
			const double deployed = first_event(run, sw::weapon_events::kDeployFinished);
			REQUIRE(deployed > 0);

			bool sim_refuses = true;
			for (size_t i = 1; i < run.states.size(); ++i) {
				const sw::WeaponSlotState &s = run.states[i].current();
				if (s.phase == sw::WeaponPhase::Deploying || run.sprinting[i]) {
					sim_refuses = sim_refuses && (s.flags & sw::slot_flags::kZoomWasDown) == 0;
				}
			}

			anim::AdsParams p;
			p.fov_in_seconds = 0.12f;
			anim::AdsCamera cam;
			const float dt = 1.0f / 144.0f;
			const double t0 = sw::units_to_seconds(sw::tick_end(run.spawn));
			bool refused_ok = true, aimed_between = false, aimed_after = false;
			for (double now = t0 + dt; now < t0 + 6.0; now += dt) {
				bool sprint = false;
				const m2a_fake::Presented pr = present_at(run, now, &sprint);
				const bool allowed = anim::ads_allowed(pr.phase, sprint);
				const anim::AdsFrame f = cam.step(p, pr.zoomed && allowed, dt);
				if (!allowed) {
					refused_ok = refused_ok && !f.zoomed;
				}
				const double sprint_start = sw::units_to_seconds(sw::tick_start(sprint_from));
				const double sprint_end = sw::units_to_seconds(sw::tick_end(sprint_to));
				if (now > deployed + 0.2 && now < sprint_start) {
					aimed_between = aimed_between || f.world_fov == 40.0f;
				}
				if (now > sprint_end + 0.2) {
					aimed_after = aimed_after || f.world_fov == 40.0f;
				}
			}
			CHECK(refused_ok);
			CHECK(aimed_between);
			CHECK(aimed_after);
			const char *what = m2a_rules ? "step_weapons_with_rules" : "step_soldier_weapons";
			std::printf("anim.viewmodel: E19 zoom refused (presentation) with %s: %s; the sim's own flag %s\n", what, refused_ok ? "PASS" : "FAIL",
					sim_refuses ? "clear on refused ticks" : "set on refused ticks");
			if (m2a_rules) {
				if (sim_refuses) {
					std::printf("anim.viewmodel: step_weapons_with_rules refuses zoom while deploying and sprinting (sv's rule has landed)\n");
				} else {
					std::printf("anim.viewmodel: PENDING (sv): step_weapons_with_rules still sets kZoomWasDown on refused ticks (weapon_rules.cpp is the W2.0 "
								"forwarding stub); the presentation refuses it meanwhile\n");
				}
			}
		}
		CHECK(!anim::ads_allowed(uint8_t(sw::WeaponPhase::Deploying), false));
		CHECK(!anim::ads_allowed(uint8_t(sw::WeaponPhase::Ready), true));
		CHECK(anim::ads_allowed(uint8_t(sw::WeaponPhase::Ready), false));
		CHECK(anim::ads_allowed(uint8_t(sw::WeaponPhase::Reloading), false));
		CHECK(anim::kVmPhaseDeploying == uint8_t(sw::WeaponPhase::Deploying));
	}

	TEST_CASE("anim.viewmodel: animator layers: deploy cuts in, cancel fades out, knife stand-in, zoom clip and reversal") {
		const anim::ViewmodelTuning tuning = shipped_tuning();
		Rig rig = rifle_rig(60, false);
		anim::ViewmodelAnimator a(rig.skeleton, rig.refs, tuning);
		anim::LocalPose pose;
		anim::ViewmodelOutput out;
		const float dt = 1.0f / 60.0f;
		double now = 0;
		auto frame = [&](float p_ads, std::vector<anim::VmCue> p_cues = {}) {
			now += dt;
			anim::ViewmodelFrameInput in;
			in.time_s = now;
			in.dt = dt;
			in.ads = p_ads;
			a.step(in, p_cues, pose, out);
		};

		frame(0);
		CHECK(std::fabs(bone1_yaw_deg(pose)) < 1e-3f);
		CHECK(out.base_clip == uint32_t(VmClip::Idle));

		anim::VmCue deploy;
		deploy.kind = anim::VmCue::Kind::DeployStarted;
		deploy.time_s = now + dt;
		frame(0, { deploy });
		CHECK(out.action_clip == uint32_t(VmClip::Deploy));
		CHECK(out.action_weight == 1.0f);
		CHECK(bone1_yaw_deg(pose) == doctest::Approx(-60.0f).epsilon(0.01));
		for (int i = 0; i < 60; ++i) {
			frame(0);
		}
		CHECK(out.action_clip == uint32_t(VmClip::Idle));

		anim::VmCue reload;
		reload.kind = anim::VmCue::Kind::ReloadStarted;
		reload.time_s = now + 0.5 * dt;
		frame(0, { reload });
		CHECK(out.action_clip == uint32_t(VmClip::Reload));
		CHECK(out.action_weight > 0.0f);
		CHECK(out.action_weight < 0.2f);
		for (int i = 0; i < 12; ++i) {
			frame(0);
		}
		CHECK(out.action_weight == 1.0f);
		anim::VmCue cancel;
		cancel.kind = anim::VmCue::Kind::ReloadCancelled;
		cancel.time_s = now + dt;
		frame(0, { cancel });
		CHECK(out.action_weight == doctest::Approx(1.0f));
		for (int i = 0; i < 6; ++i) {
			frame(0);
		}
		CHECK(out.action_weight < 1.0f);
		for (int i = 0; i < 6; ++i) {
			frame(0);
		}
		CHECK(out.action_clip == uint32_t(VmClip::Idle));

		frame(0.5f);
		CHECK(out.base_clip == uint32_t(VmClip::ZoomIn));
		CHECK(bone1_yaw_deg(pose) == doctest::Approx(20.0f).epsilon(0.02));
		frame(1.0f);
		CHECK(out.base_clip == uint32_t(VmClip::ZoomIdle));
		CHECK(bone1_yaw_deg(pose) == doctest::Approx(40.0f).epsilon(0.01));
		frame(0.75f);
		CHECK(out.base_clip == uint32_t(VmClip::ZoomOut));

		frame(0.8f);
		CHECK(out.base_clip == uint32_t(VmClip::ZoomIn));
		CHECK(bone1_yaw_deg(pose) == doctest::Approx(32.0f).epsilon(0.03));
		frame(0.0f);

		for (int i = 0; i < 30; ++i) {
			now += dt;
			anim::ViewmodelFrameInput in;
			in.time_s = now;
			in.dt = dt;
			in.sprinting = true;
			a.step(in, {}, pose, out);
		}
		CHECK(out.sprint_weight == 1.0f);
		CHECK(out.base_clip == uint32_t(VmClip::Sprint));
		CHECK(out.weapon_offset.t.y < -0.04f);

		Rig knife;
		knife.skeleton = three_bones();
		add_clip(knife, VmClip::Idle, 0, 0, 0);
		finish_rig(knife, { VmClip::Idle });
		knife.refs[size_t(VmClip::Knife)].seconds = 0.4f;
		knife.refs[size_t(VmClip::Knife)].stand_in = true;
		knife.refs[size_t(VmClip::Deploy)].seconds = 0.5f;
		knife.refs[size_t(VmClip::Deploy)].stand_in = true;
		anim::ViewmodelAnimator k(knife.skeleton, knife.refs, tuning);
		now = 0;
		anim::VmCue stab;
		stab.kind = anim::VmCue::Kind::Shot;
		stab.time_s = dt;
		float min_z = 0;
		for (int i = 0; i < 30; ++i) {
			now += dt;
			anim::ViewmodelFrameInput in;
			in.time_s = now;
			in.dt = dt;
			std::vector<anim::VmCue> cues;
			if (i == 0) {
				cues.push_back(stab);
			}
			k.step(in, cues, pose, out);
			if (i == 0) {
				CHECK(out.action_clip == uint32_t(VmClip::Knife));
				CHECK(out.action_procedural);
			}
			min_z = std::min(min_z, out.weapon_offset.t.z);
		}
		CHECK(min_z < -0.1f);

		anim::VmCue d2;
		d2.kind = anim::VmCue::Kind::DeployStarted;
		d2.time_s = now + dt;
		now += dt;
		{
			anim::ViewmodelFrameInput in;
			in.time_s = now;
			in.dt = dt;
			k.step(in, std::vector<anim::VmCue>{ d2 }, pose, out);
		}
		CHECK(out.weapon_offset.t.y < -0.2f);
		for (int i = 0; i < 40; ++i) {
			now += dt;
			anim::ViewmodelFrameInput in;
			in.time_s = now;
			in.dt = dt;
			k.step(in, {}, pose, out);
		}
		CHECK(std::fabs(out.weapon_offset.t.y) < 1e-3f);
		CHECK(out.action_clip == uint32_t(VmClip::Idle));
	}

	TEST_CASE("anim.viewmodel: the hold-and-release flow: start, hold loop, release from its entry tick, redeploy or "
			  "idle") {
		const anim::ViewmodelTuning tuning = shipped_tuning();
		Rig rig;
		rig.skeleton = three_bones();
		add_clip(rig, VmClip::Idle, 0, 0, 0);
		add_clip(rig, VmClip::ThrowStart, 0, 25, 25);
		add_clip(rig, VmClip::ThrowHold, 30, 30, 35);
		add_clip(rig, VmClip::Throw, 0, 40, 40);
		add_clip(rig, VmClip::Deploy, -50, 0, 25);
		finish_rig(rig, { VmClip::Idle, VmClip::ThrowStart, VmClip::ThrowHold, VmClip::Throw, VmClip::Deploy });
		anim::VmThrowFlow flow;
		flow.release_from_s = 14.0f / 60.0f;
		flow.release_until_s = 50.0f / 60.0f;
		flow.release_in_s = 11.0f / 60.0f;
		flow.hold_from_s = 42.0f / 60.0f;
		flow.hold_loop_s = 1.0f;
		flow.release_end_s = 70.0f / 60.0f;
		REQUIRE(flow.valid());
		const float dt = 1.0f / 60.0f;
		struct Seen {
			uint32_t clip;
			double at;
			float time;
		};
		struct Cue {
			anim::VmCue::Kind kind;
			double at;
		};
		double release_end = -1;
		auto run = [&](std::vector<Cue> p_cues, double p_until, bool p_flow, std::vector<Seen> &r_seen,
						   float &r_max_hold_time, bool p_switch_back = false) {
			anim::ViewmodelAnimator a(rig.skeleton, rig.refs, tuning);
			if (p_flow) {
				a.set_throw_flow(flow);
			}
			release_end = -1;
			anim::LocalPose pose;
			anim::ViewmodelOutput out;
			r_seen.clear();
			r_max_hold_time = 0.0f;
			size_t next = 0;
			for (int i = 1; double(i) * dt <= p_until; ++i) {
				const double now = double(i) * dt;
				std::vector<anim::VmCue> cues;
				while (next < p_cues.size() && p_cues[next].at <= now) {
					anim::VmCue c;
					c.kind = p_cues[next].kind;
					c.time_s = p_cues[next].at;
					cues.push_back(c);
					++next;
				}
				anim::ViewmodelFrameInput in;
				in.time_s = now;
				in.dt = dt;
				in.switch_back = p_switch_back;
				a.step(in, cues, pose, out);
				release_end = out.release_end_s;
				if (r_seen.empty() || r_seen.back().clip != out.action_clip) {
					r_seen.push_back(Seen{ out.action_clip, out.action_start_s, out.action_time_s });
				}
				if (out.action_clip == uint32_t(VmClip::ThrowHold)) {
					r_max_hold_time = std::max(r_max_hold_time, out.action_time_s);
				}
				CHECK(std::isfinite(bone1_yaw_deg(pose)));
			}
		};
		using K = anim::VmCue::Kind;
		std::vector<Seen> seen;
		float hold_t = 0;

		run({ { K::HoldStarted, 0.1 }, { K::Shot, 0.6 }, { K::Thrown, 0.6 }, { K::ReloadStarted, 0.8 } }, 3.0, true,
				seen, hold_t);
		REQUIRE(seen.size() == 5);
		CHECK(seen[0].clip == uint32_t(VmClip::Idle));
		CHECK(seen[1].clip == uint32_t(VmClip::ThrowStart));
		CHECK(seen[1].at == doctest::Approx(0.1));
		CHECK(seen[2].clip == uint32_t(VmClip::Throw));
		CHECK(seen[2].at == doctest::Approx(0.6));
		CHECK(seen[2].time == doctest::Approx(11.0f / 60.0f).epsilon(0.02));
		CHECK(seen[3].clip == uint32_t(VmClip::Deploy));
		CHECK(seen[3].at == doctest::Approx(0.6 + 59.0 / 60.0));
		CHECK(seen[4].clip == uint32_t(VmClip::Idle));
		CHECK(release_end == doctest::Approx(0.6 + 59.0 / 60.0));

		const std::vector<Cue> quick = { { K::HoldStarted, 0.1 }, { K::Thrown, 0.6 }, { K::ReloadStarted, 0.8 } };
		run(quick, 3.0, true, seen, hold_t, true);
		REQUIRE(seen.size() >= 4);
		CHECK(seen[3].clip == uint32_t(VmClip::Deploy));
		flow.skip_undeploy = true;
		run(quick, 3.0, true, seen, hold_t, true);
		REQUIRE(seen.size() >= 3);
		CHECK(seen[2].clip == uint32_t(VmClip::Throw));
		for (const Seen &s : seen) {
			CHECK(s.clip != uint32_t(VmClip::Deploy));
		}
		CHECK(release_end == doctest::Approx(0.6 + 59.0 / 60.0));
		run(quick, 3.0, true, seen, hold_t, false);
		REQUIRE(seen.size() >= 4);
		CHECK(seen[3].clip == uint32_t(VmClip::Deploy));
		flow.skip_undeploy = false;

		run({ { K::HoldStarted, 0.1 }, { K::Thrown, 1.9 } }, 3.5, true, seen, hold_t);
		REQUIRE(seen.size() == 5);
		CHECK(seen[1].clip == uint32_t(VmClip::ThrowStart));
		CHECK(seen[2].clip == uint32_t(VmClip::ThrowHold));
		CHECK(seen[2].at == doctest::Approx(0.1 + 42.0 / 60.0));
		CHECK(hold_t < 1.0f);
		CHECK(hold_t > 0.9f);
		CHECK(seen[3].clip == uint32_t(VmClip::Throw));
		CHECK(seen[3].at == doctest::Approx(1.9));
		CHECK(seen[3].time < 0.02f);
		CHECK(seen[4].clip == uint32_t(VmClip::Idle));

		run({ { K::HoldStarted, 0.1 }, { K::Thrown, 0.15 } }, 2.0, true, seen, hold_t);
		REQUIRE(seen.size() >= 3);
		CHECK(seen[2].clip == uint32_t(VmClip::Throw));
		CHECK(seen[2].at == doctest::Approx(0.1 + 14.0 / 60.0));

		run({ { K::HoldStarted, 0.1 }, { K::Thrown, 0.6 } }, 2.0, false, seen, hold_t);
		REQUIRE(seen.size() >= 2);
		CHECK(seen[1].clip == uint32_t(VmClip::Throw));
		CHECK(seen[1].at == doctest::Approx(0.6));

		anim::VmThrowFlow broken = flow;
		broken.hold_loop_s = 0.0f;
		CHECK_FALSE(broken.valid());
		broken = flow;
		broken.release_end_s = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(broken.valid());
	}

	TEST_CASE("anim.viewmodel: E19 the animator's springs are at rest within the settle time after shots, a turn and a run") {
		const anim::ViewmodelTuning tuning = shipped_tuning();
		Rig rig = rifle_rig(60, false);
		anim::ViewmodelAnimator a(rig.skeleton, rig.refs, tuning);
		const float settle = std::max({ tuning.sway.settle_seconds, tuning.bob.settle_seconds, tuning.kick.settle_seconds, tuning.camera_recoil.settle_seconds });
		for (const float fps : kFrameRates) {
			a.reset();
			const float dt = 1.0f / fps;
			anim::LocalPose pose;
			anim::ViewmodelOutput out;
			double now = 0, stop = 2.0, rest_at = -1;
			double next_shot = 0.5;
			for (int i = 1; now < 6.0; ++i) {
				now = double(i) * dt;
				anim::ViewmodelFrameInput in;
				in.time_s = now;
				in.dt = dt;
				const bool active = now < stop;
				in.speed = active ? 5.0f : 0.0f;
				in.view_yaw_rate = active ? 3.0f * float(std::sin(now * 3.0)) : 0.0f;
				in.view_pitch_rate = active ? 1.0f : 0.0f;
				std::vector<anim::VmCue> cues;
				while (active && next_shot <= now) {
					anim::VmCue c;
					c.time_s = next_shot;
					cues.push_back(c);
					next_shot += 0.075;
				}
				a.step(in, cues, pose, out);
				if (now >= stop) {
					if (!out.springs_at_rest) {
						rest_at = -1;
					} else if (rest_at < 0) {
						rest_at = now;
					}
				}
			}
			REQUIRE(rest_at > 0);
			CHECK_MESSAGE(rest_at - stop <= double(settle) + 1e-6, (rest_at - stop));
			std::printf("anim.viewmodel: E19 animator %5.0f fps: every spring at rest %.3f s after the input stops (configured at most %.2f s)\n",
					double(fps), rest_at - stop, double(settle));
		}
	}
}
