#include "m2a_viewmodel_demo.h"

#include "fp_camera.h"
#include "godot_util.h"
#include "viewmodel.h"

#include "bf3/core/util/format.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace sw = bf3::sim::weapons;

namespace {

constexpr double kDeg = 180.0 / 3.14159265358979323846;
constexpr double kRecoilToleranceDeg = 1e-4;
constexpr float kFovTolerance = 1e-3f;
constexpr float kE19GenericFovSeconds = 0.15f;

struct Row {
	std::string id;
	bool pass = true;
	std::string detail;
};

void print(const std::string &p_line) {
	UtilityFunctions::print(to_godot(p_line));
}

using bf3::core::fmt;

std::string short_unlock(const std::string &p_unlock) {
	std::string u = p_unlock.substr(p_unlock.rfind('/') + 1);
	return u.rfind("u_", 0) == 0 ? u.substr(2) : u;
}

double wrap_pi(double p_a) {
	while (p_a > 3.14159265358979323846) {
		p_a -= 2.0 * 3.14159265358979323846;
	}
	while (p_a < -3.14159265358979323846) {
		p_a += 2.0 * 3.14159265358979323846;
	}
	return p_a;
}

bool is_reload_clip(uint32_t p_clip) {
	return p_clip == uint32_t(bf3::anim::VmClip::Reload) || p_clip == uint32_t(bf3::anim::VmClip::ReloadEmpty);
}

struct Accum {

	uint32_t hip_marks = 0, hip_bad = 0;
	uint32_t ads_marks = 0, ads_bad = 0;
	float ads_fov_seen = 0;
	std::string fov_bad;

	uint32_t vm_hip_bad = 0;
	std::set<float> render_fovs;

	double worst_in_frames = 0, worst_out_frames = 0;
	uint32_t transitions = 0, transitions_bad = 0;
	std::string transition_lines;

	double worst_recoil_deg = 0;
	double max_recoil_deg = 0;
	uint64_t frames = 0;

	double worst_unsettled_s = 0;
	uint32_t settle_marks = 0, settle_bad = 0;

	std::array<double, 4> spring_worst_s{};
	std::array<double, 4> spring_excess_frames{ -1e9, -1e9, -1e9, -1e9 };
	std::array<uint64_t, 4> spring_inputs{};

	float fov_seconds_min = 1e9f, fov_seconds_max = -1e9f;

	uint64_t refused_frames = 0, refused_bad = 0, sim_flag_frames = 0;

	uint32_t reloads = 0, clip_start_bad = 0, fire_bad = 0, cut_bad = 0;
	double worst_clip_start_s = 0, worst_fire_after_s = 0, worst_cut_s = 0;
	std::string e5_lines;

	double present_us_sum = 0, present_us_max = 0;
	uint64_t present_frames = 0;
	std::map<std::string, std::string> ads_info;

	uint32_t throws = 0, throws_bad = 0;
	std::string throw_lines;
};

const sw::TimedWeaponEvent *find_event(const vm_script::KitRun &p_run, uint32_t p_event, uint8_t p_slot, double p_from_s) {
	for (const sw::TimedWeaponEvent &e : p_run.timed) {
		if (e.event == p_event && e.weapon_slot == p_slot && sw::units_to_seconds(e.time) >= p_from_s - 1e-9) {
			return &e;
		}
	}
	return nullptr;
}

const sw::ShotEvent *find_shot(const vm_script::KitRun &p_run, uint8_t p_slot, double p_from_s) {
	for (const sw::ShotEvent &s : p_run.shots) {
		if (s.weapon_slot == p_slot && sw::units_to_seconds(s.time) >= p_from_s - 1e-9) {
			return &s;
		}
	}
	return nullptr;
}

void run_pass(Bf3M2aViewmodelDemo &p_demo, size_t p_kit, double p_fps, Accum &r_acc) {
	const Bf3M2aViewmodelDemo::Kit &kit = p_demo.kits()[p_kit];
	const vm_script::KitRun &run = kit.run;
	Bf3Viewmodel *vm = p_demo.viewmodel(kit.team);
	p_demo.viewmodel(1)->select_item(-1);
	p_demo.viewmodel(2)->select_item(-1);
	Camera3D *camera = p_demo.camera();
	const bf3::anim::ViewmodelTuning &tuning = p_demo.tuning();
	const double dt = 1.0 / p_fps;
	const double t0 = run.start_s();
	const double t_end = run.end_s();

	struct Pending {
		double at;
		uint8_t slot;
		std::string label;
		bool done = false;
	};
	std::vector<Pending> marks;
	for (const vm_script::Mark &m : run.marks) {
		marks.push_back({ m.time_s, m.slot, m.label });
	}

	struct Transition {
		uint8_t slot;
		double press, release;
		double reach_in = -1, reach_out = -1;
		float t_in = 0, t_out = 0;
		bool released_seen = false;
	};
	std::vector<Transition> transitions;
	for (uint8_t slot = 0; slot < vm_script::kKitItems; ++slot) {
		const vm_script::Mark *press = run.mark(slot, "zoom_press");
		const vm_script::Mark *release = run.mark(slot, "zoom_release");
		if (press && release) {
			transitions.push_back({ slot, press->time_s, release->time_s });
		}
	}

	struct Reload {
		uint8_t slot;
		std::string label;
		double started = -1, finished = -1, shot = -1;
		double clip_on = -1, shot_on = -1, cut = -1;
		uint32_t clip = 0;
		uint32_t shots_before = 0;
	};
	std::vector<Reload> reloads;
	for (const vm_script::Mark &m : run.marks) {
		if (m.label != "reload_start" && m.label != "reload_empty_start") {
			continue;
		}
		Reload r;
		r.slot = m.slot;
		r.label = m.label;
		if (const sw::TimedWeaponEvent *e = find_event(run, sw::weapon_events::kReloadStarted, m.slot, m.time_s - 1e-6)) {
			r.started = sw::units_to_seconds(e->time);
			if (const sw::TimedWeaponEvent *f = find_event(run, sw::weapon_events::kReloadFinished, m.slot, r.started)) {
				r.finished = sw::units_to_seconds(f->time);
				if (const sw::ShotEvent *s = find_shot(run, m.slot, r.finished)) {
					r.shot = sw::units_to_seconds(s->time);
				}
			}
		}
		reloads.push_back(r);
	}

	struct Throw {
		uint8_t slot = 0;
		double hold = -1, thrown = -1;
		struct Seen {
			uint32_t clip;
			double t, start;
			float time;
		};
		std::vector<Seen> seen;
	};
	std::vector<Throw> throws;
	for (const vm_script::Mark &m : run.marks) {
		if (m.label != "throw") {
			continue;
		}
		Throw th;
		th.slot = m.slot;
		const double from = m.time_s - 2.0;
		if (const sw::TimedWeaponEvent *h = find_event(run, sw::weapon_events::kHoldStarted, m.slot, from)) {
			th.hold = sw::units_to_seconds(h->time);
			if (const sw::TimedWeaponEvent *e = find_event(run, sw::weapon_events::kThrown, m.slot, th.hold)) {
				th.thrown = sw::units_to_seconds(e->time);
			}
		}
		throws.push_back(th);
	}

	double last_input = t0;
	std::array<double, 4> spring_last_input{ t0, t0, t0, t0 };
	float last_blend = 0;
	bool last_sprint = false;
	double prev = t0 - dt;
	std::vector<bf3::host::PresentEvent> events;
	for (uint64_t n = 0;; ++n) {
		const double t = t0 + double(n) * dt;
		if (t > t_end + 1e-9) {
			break;
		}
		events.clear();
		vm_script::events_between(run, prev, t, events);
		p_demo.present_kit(p_kit, prev, t, float(dt));
		prev = t;
		const vm_script::View &view = p_demo.last_view();
		const bf3::anim::AdsFrame &ads = vm->ads();
		const bf3::anim::AdsParams &params = vm->ads_params();
		const bf3::anim::ViewmodelOutput &out = vm->output();
		const float cam_fov = float(camera->get_fov());
		const uint8_t slot = view.weapon.active_slot;
		++r_acc.frames;
		r_acc.present_us_sum += vm->last_present_us();
		r_acc.present_us_max = std::max(r_acc.present_us_max, vm->last_present_us());
		++r_acc.present_frames;
		const bf3::render::ViewmodelItem *item = vm->current_item();
		if (item == nullptr) {
			continue;
		}
		if (params.can_zoom) {
			r_acc.fov_seconds_min = std::min({ r_acc.fov_seconds_min, params.fov_in_seconds, params.fov_out_seconds });
			r_acc.fov_seconds_max = std::max({ r_acc.fov_seconds_max, params.fov_in_seconds, params.fov_out_seconds });
		}

		{
			double cam_yaw = 0, cam_pitch = 0;
			camera_view_angles_of(camera, cam_yaw, cam_pitch);
			const double off_pitch = (cam_pitch - view.pitch) * kDeg - double(out.camera_recoil_pitch_deg);
			const double off_yaw = wrap_pi(cam_yaw - view.yaw) * kDeg - double(out.camera_recoil_yaw_deg);
			const double e = std::max(std::fabs(off_pitch - double(view.weapon.recoil.pitch)), std::fabs(off_yaw - double(view.weapon.recoil.yaw)));
			r_acc.worst_recoil_deg = std::max(r_acc.worst_recoil_deg, e);
			r_acc.max_recoil_deg = std::max(r_acc.max_recoil_deg, double(std::fabs(view.weapon.recoil.pitch)));
		}

		bool input = view.yaw_rate != 0.0f || view.pitch_rate != 0.0f || view.speed > 0.0f || view.sprinting != last_sprint ||
				view.sprinting || std::fabs(ads.blend - last_blend) > 1e-7f;
		for (const bf3::host::PresentEvent &e : events) {
			if (e.kind == bf3::host::PresentKind::OwnShot || (e.kind == bf3::host::PresentKind::OwnTimed && e.id == sw::weapon_events::kThrown)) {
				input = true;
			}
		}
		last_blend = ads.blend;
		last_sprint = view.sprinting;
		if (input) {
			last_input = t;
		} else if (!out.springs_at_rest) {
			r_acc.worst_unsettled_s = std::max(r_acc.worst_unsettled_s, t - last_input);
		}

		if (const bf3::anim::ViewmodelAnimator *animator = vm->animator()) {
			const std::array<bf3::anim::SpringState, 7> st = animator->springs();
			bool shot = false;
			for (const bf3::host::PresentEvent &e : events) {
				if (e.kind == bf3::host::PresentKind::OwnShot || (e.kind == bf3::host::PresentKind::OwnTimed && e.id == sw::weapon_events::kThrown)) {
					shot = true;
				}
			}
			const bool spring_input[4] = { view.yaw_rate != 0.0f || view.pitch_rate != 0.0f, view.speed > 0.0f || view.sprinting, shot, shot };
			const bf3::anim::SpringParams *params_of[4] = { &tuning.sway, &tuning.bob, &tuning.kick, &tuning.camera_recoil };
			const bool at_rest[4] = {
				bf3::anim::spring_at_rest(tuning.sway, 0.0f, st[0]) && bf3::anim::spring_at_rest(tuning.sway, 0.0f, st[1]),
				bf3::anim::spring_at_rest(tuning.bob, 0.0f, st[2]),
				bf3::anim::spring_at_rest(tuning.kick, 0.0f, st[3]) && bf3::anim::spring_at_rest(tuning.kick, 0.0f, st[4]),
				bf3::anim::spring_at_rest(tuning.camera_recoil, 0.0f, st[5]) && bf3::anim::spring_at_rest(tuning.camera_recoil, 0.0f, st[6]),
			};
			for (size_t k = 0; k < 4; ++k) {
				if (spring_input[k]) {
					spring_last_input[k] = t;
					++r_acc.spring_inputs[k];
				} else if (!at_rest[k]) {
					const double since_input = t - spring_last_input[k];
					r_acc.spring_worst_s[k] = std::max(r_acc.spring_worst_s[k], since_input);
					r_acc.spring_excess_frames[k] = std::max(r_acc.spring_excess_frames[k], (since_input - double(params_of[k]->settle_seconds)) / dt);
				}
			}
		}

		if (view.zoom_held && (view.weapon.phase == sw::WeaponPhase::Deploying || view.sprinting)) {
			++r_acc.refused_frames;
			if (ads.zoomed || std::fabs(cam_fov - params.hip_fov) > kFovTolerance) {
				++r_acc.refused_bad;
			}
			if (view.weapon.zoomed) {
				++r_acc.sim_flag_frames;
			}
		}

		std::vector<Pending> later;
		for (Pending &m : marks) {
			if (m.done || t + 1e-9 < m.at || m.slot != slot) {
				continue;
			}
			m.done = true;
			if (m.label == "hip") {
				++r_acc.hip_marks;
				if (std::fabs(cam_fov - 55.0f) > kFovTolerance) {
					++r_acc.hip_bad;
					r_acc.fov_bad += fmt(" hip %s %.3f", short_unlock(item->unlock).c_str(), double(cam_fov));
				}
				r_acc.render_fovs.insert(item->render_fov);
				if (std::fabs(vm->applied_weapon_fov() - params.render_fov) > kFovTolerance || std::fabs(vm->applied_arms_fov() - params.render_fov) > kFovTolerance ||
						(item->render_fov > 0 && std::fabs(params.render_fov - item->render_fov) > kFovTolerance)) {
					++r_acc.vm_hip_bad;
					r_acc.fov_bad += fmt(" vmfov %s %.2f/%.2f vs %.2f", short_unlock(item->unlock).c_str(), double(vm->applied_weapon_fov()),
							double(vm->applied_arms_fov()), double(params.render_fov));
				}
			} else if (m.label == "ads") {
				++r_acc.ads_marks;
				r_acc.ads_fov_seen = cam_fov;
				if (std::fabs(cam_fov - 40.0f) > kFovTolerance || !ads.zoomed) {
					++r_acc.ads_bad;
					r_acc.fov_bad += fmt(" ads %s %.3f", short_unlock(item->unlock).c_str(), double(cam_fov));
				}
				r_acc.ads_info[short_unlock(item->unlock)] = fmt("look scale %.2f (LookSpeedMultiplier %.2f), viewmodel FOV %.1f (RenderFov %.1f, ZoomRenderFov %.1f), vignette %.2f",
						double(vm->look_scale()), double(params.look_speed_zoom), double(vm->applied_weapon_fov()), double(params.render_fov),
						double(params.zoom_render_fov), double(vm->vignette_strength()));
			} else if (m.label == "settle") {

				later.push_back({ m.at + double(std::max(tuning.sway.settle_seconds, tuning.bob.settle_seconds)) + dt, m.slot, "settled" });
			} else if (m.label == "settled") {
				++r_acc.settle_marks;
				if (!out.springs_at_rest) {
					++r_acc.settle_bad;
				}
			}
		}

		marks.insert(marks.end(), later.begin(), later.end());

		for (Transition &tr : transitions) {
			if (tr.slot != slot) {
				continue;
			}
			if (tr.reach_in < 0 && t + 1e-9 >= tr.press && ads.fov_progress >= 1.0f && std::fabs(cam_fov - params.zoom_fov) <= kFovTolerance) {
				tr.reach_in = t;
				tr.t_in = params.fov_in_seconds;
			}
			if (tr.reach_out < 0 && t + 1e-9 >= tr.release && ads.fov_progress <= 0.0f && std::fabs(cam_fov - params.hip_fov) <= kFovTolerance) {
				tr.reach_out = t;
				tr.t_out = params.fov_out_seconds;
			}
		}

		for (Throw &th : throws) {
			if (th.slot != slot || th.hold < 0 || t + 1e-9 < th.hold) {
				continue;
			}
			if (th.seen.empty() || th.seen.back().clip != out.action_clip) {
				th.seen.push_back({ out.action_clip, t, out.action_start_s, out.action_time_s });
			}
		}

		for (Reload &r : reloads) {
			if (r.slot != slot || r.started < 0) {
				continue;
			}
			if (r.clip_on < 0 && t + 1e-9 >= r.started && is_reload_clip(out.action_clip) && std::fabs(out.action_start_s - r.started) < 1e-6) {
				r.clip_on = t;
				r.clip = out.action_clip;
			}
			if (r.finished >= 0 && t + 1e-9 < r.finished) {
				r.shots_before = out.shots;
			}
			if (r.shot >= 0 && r.shot_on < 0 && t + 1e-9 >= r.finished && out.shots > r.shots_before) {
				r.shot_on = t;
			}
			if (r.shot_on >= 0 && r.cut < 0 && (!is_reload_clip(out.action_clip) || out.action_weight <= 1e-3f)) {
				r.cut = t;
			}
		}
	}

	const bf3::render::ViewmodelKit &kit_def = p_demo.kit_defs()[kit.index];
	const std::string kit_name = kit_def.team + " " + kit_def.kit.substr(kit_def.kit.rfind('/') + 1);
	const bf3::render::ViewmodelSet &set = p_demo.set(kit.team);
	for (const Transition &tr : transitions) {
		++r_acc.transitions;
		const int item = kit.item[tr.slot];
		const std::string name = item >= 0 ? short_unlock(set.items[size_t(item)].unlock) : "?";
		const double in = tr.reach_in < 0 ? 1e9 : tr.reach_in - tr.press;
		const double outv = tr.reach_out < 0 ? 1e9 : tr.reach_out - tr.release;
		const double ein = std::fabs(in - double(tr.t_in)) / dt;
		const double eout = std::fabs(outv - double(tr.t_out)) / dt;
		r_acc.worst_in_frames = std::max(r_acc.worst_in_frames, ein);
		r_acc.worst_out_frames = std::max(r_acc.worst_out_frames, eout);
		const bool ok = ein <= 1.0 + 1e-6 && eout <= 1.0 + 1e-6;
		if (!ok) {
			++r_acc.transitions_bad;
		}
		r_acc.transition_lines += fmt("    %s %s @%3.0f fps: in %.4f s (FOV time %.3f, %+.2f frames)  out %.4f s (FOV time %.3f, %+.2f frames)%s\n", kit_name.c_str(),
				name.c_str(), p_fps, in, double(tr.t_in), (in - double(tr.t_in)) / dt, outv, double(tr.t_out), (outv - double(tr.t_out)) / dt, ok ? "" : "  FAIL");
	}
	for (const Throw &th : throws) {
		using bf3::anim::VmClip;
		using Role = bf3::render::ViewmodelClipRole;
		++r_acc.throws;
		const int item = kit.item[th.slot];
		if (item < 0) {
			++r_acc.throws_bad;
			continue;
		}
		const bf3::render::ViewmodelItem &it = set.items[size_t(item)];
		const bf3::anim::VmThrowFlow &f = it.throw_flow;
		bool retail = f.valid();
		for (const Role r : { Role::ThrowStart, Role::ThrowHold, Role::Throw, Role::Deploy }) {
			retail = retail && it.clips[size_t(r)].source == bf3::render::ViewmodelClipSource::Clip;
		}
		const double eps = 1e-6;
		std::string order;
		for (const Throw::Seen &e : th.seen) {
			order += std::string(order.empty() ? "" : " > ") + bf3::anim::vm_clip_name(VmClip(e.clip));
		}
		const bool shape = th.seen.size() == 4 && th.seen[0].clip == uint32_t(VmClip::ThrowStart) &&
				th.seen[1].clip == uint32_t(VmClip::Throw) && th.seen[2].clip == uint32_t(VmClip::Deploy) &&
				th.seen[3].clip == uint32_t(VmClip::Idle);
		bool timing = false;
		double entry = 0, redeploy = 0, deploy_len = 0;
		if (shape && th.thrown >= 0) {
			entry = double(th.seen[1].time) - (th.seen[1].t - th.seen[1].start);
			redeploy = th.seen[2].start - th.thrown;
			deploy_len = th.seen[3].t - th.seen[2].start;
			const double deploy_s = double(it.clips[size_t(Role::Deploy)].seconds);
			timing = std::fabs(th.seen[0].start - th.hold) <= eps && std::fabs(th.seen[1].start - th.thrown) <= eps &&
					std::fabs(entry - double(f.release_in_s)) <= 1e-4 &&
					std::fabs(redeploy - double(f.release_end_s - f.release_in_s)) <= eps &&
					deploy_len >= deploy_s - eps && deploy_len <= deploy_s + dt + eps;
		}
		const bool ok = retail && shape && timing;
		r_acc.throws_bad += ok ? 0 : 1;
		r_acc.throw_lines += fmt("    %s %s @%3.0f fps (%s): %s; %s %.3f s / %s %.3f s / %s %.3f s / %s %.3f s; hold "
								 "%.3f s, release entered at %.4f s (data %.4f), "
								 "redeploy %.4f s after the throw (data %.4f), deploy shown %.4f s%s\n",
				kit_name.c_str(), short_unlock(it.unlock).c_str(), p_fps, it.throw_flow_name.c_str(), order.c_str(),
				it.clips[size_t(Role::ThrowStart)].name.c_str(), double(it.clips[size_t(Role::ThrowStart)].seconds),
				it.clips[size_t(Role::ThrowHold)].name.c_str(), double(it.clips[size_t(Role::ThrowHold)].seconds),
				it.clips[size_t(Role::Throw)].name.c_str(), double(it.clips[size_t(Role::Throw)].seconds),
				it.clips[size_t(Role::Deploy)].name.c_str(), double(it.clips[size_t(Role::Deploy)].seconds),
				th.thrown - th.hold, entry, double(f.release_in_s), redeploy, double(f.release_end_s - f.release_in_s),
				deploy_len, ok ? "" : "  FAIL");
	}
	for (const Reload &r : reloads) {
		++r_acc.reloads;
		const int item = kit.item[r.slot];
		const std::string name = item >= 0 ? short_unlock(set.items[size_t(item)].unlock) : "?";
		const double start_lat = r.clip_on < 0 ? 1e9 : r.clip_on - r.started;
		const double fire_lat = r.shot_on < 0 ? 1e9 : r.shot_on - r.finished;
		const double cut_lat = r.cut < 0 ? 1e9 : r.cut - r.shot_on;
		const bool start_ok = r.started >= 0 && start_lat >= -1e-9 && start_lat <= dt + 1e-9;
		const bool fire_ok = r.shot >= 0 && fire_lat >= -1e-9 && fire_lat <= dt + 1e-9;
		const bool cut_ok = r.cut >= 0 && cut_lat <= double(bf3::anim::kViewmodelCrossfadeSeconds) + dt + 1e-9;
		r_acc.clip_start_bad += start_ok ? 0 : 1;
		r_acc.fire_bad += fire_ok ? 0 : 1;
		r_acc.cut_bad += cut_ok ? 0 : 1;
		if (start_lat < 1e8) {
			r_acc.worst_clip_start_s = std::max(r_acc.worst_clip_start_s, start_lat);
		}
		if (fire_lat < 1e8) {
			r_acc.worst_fire_after_s = std::max(r_acc.worst_fire_after_s, fire_lat);
		}
		if (cut_lat < 1e8) {
			r_acc.worst_cut_s = std::max(r_acc.worst_cut_s, cut_lat);
		}
		r_acc.e5_lines += fmt("    %s %s %s @%3.0f fps: %s on screen %+.4f s after kReloadStarted, finish %.3f s after the start, shot on screen %+.4f s after "
							  "the finish (sim shot %+.4f s), clip cut %.3f s later%s\n",
				kit_name.c_str(), name.c_str(), r.label.c_str(), p_fps, bf3::anim::vm_clip_name(bf3::anim::VmClip(r.clip)), start_lat, r.finished - r.started,
				fire_lat, r.shot - r.finished, cut_lat, start_ok && fire_ok && cut_ok ? "" : "  FAIL");
	}
}

}

int run_m2a_viewmodel_check(Bf3M2aViewmodelDemo &p_demo) {
	std::vector<Row> rows;
	const bf3::anim::ViewmodelTuning base = p_demo.tuning();
	print("[vm] tuning " + p_demo.tuning_source() + fmt(" (fov_transition_specific %s)", base.fov_transition_specific ? "true" : "false"));
	const double fps_list[] = { 60.0, 144.0 };
	const bool modes[] = { true, false };
	Accum all;
	Accum per_mode[2];
	for (int mi = 0; mi < 2; ++mi) {
		bf3::anim::ViewmodelTuning t = base;
		t.fov_transition_specific = modes[mi];
		p_demo.set_tuning(t);
		for (size_t k = 0; k < p_demo.kits().size(); ++k) {
			for (const double fps : fps_list) {
				Accum a;
				run_pass(p_demo, k, fps, a);
				for (Accum *dst : { &all, &per_mode[mi] }) {
					dst->hip_marks += a.hip_marks;
					dst->hip_bad += a.hip_bad;
					dst->ads_marks += a.ads_marks;
					dst->ads_bad += a.ads_bad;
					dst->fov_bad += a.fov_bad;
					dst->vm_hip_bad += a.vm_hip_bad;
					dst->render_fovs.insert(a.render_fovs.begin(), a.render_fovs.end());
					dst->worst_in_frames = std::max(dst->worst_in_frames, a.worst_in_frames);
					dst->worst_out_frames = std::max(dst->worst_out_frames, a.worst_out_frames);
					dst->transitions += a.transitions;
					dst->transitions_bad += a.transitions_bad;
					dst->transition_lines += a.transition_lines;
					dst->worst_recoil_deg = std::max(dst->worst_recoil_deg, a.worst_recoil_deg);
					dst->max_recoil_deg = std::max(dst->max_recoil_deg, a.max_recoil_deg);
					dst->frames += a.frames;
					dst->worst_unsettled_s = std::max(dst->worst_unsettled_s, a.worst_unsettled_s);
					dst->settle_marks += a.settle_marks;
					dst->settle_bad += a.settle_bad;
					for (size_t s = 0; s < 4; ++s) {
						dst->spring_worst_s[s] = std::max(dst->spring_worst_s[s], a.spring_worst_s[s]);
						dst->spring_excess_frames[s] = std::max(dst->spring_excess_frames[s], a.spring_excess_frames[s]);
						dst->spring_inputs[s] += a.spring_inputs[s];
					}
					dst->fov_seconds_min = std::min(dst->fov_seconds_min, a.fov_seconds_min);
					dst->fov_seconds_max = std::max(dst->fov_seconds_max, a.fov_seconds_max);
					dst->refused_frames += a.refused_frames;
					dst->refused_bad += a.refused_bad;
					dst->sim_flag_frames += a.sim_flag_frames;
					dst->reloads += a.reloads;
					dst->clip_start_bad += a.clip_start_bad;
					dst->fire_bad += a.fire_bad;
					dst->cut_bad += a.cut_bad;
					dst->worst_clip_start_s = std::max(dst->worst_clip_start_s, a.worst_clip_start_s);
					dst->worst_fire_after_s = std::max(dst->worst_fire_after_s, a.worst_fire_after_s);
					dst->worst_cut_s = std::max(dst->worst_cut_s, a.worst_cut_s);
					dst->throws += a.throws;
					dst->throws_bad += a.throws_bad;
					if (mi == 0) {
						dst->throw_lines += a.throw_lines;
					}
					if (mi == 0) {
						dst->e5_lines += a.e5_lines;
					}
					if (fps == 60.0 && mi == 0) {
						dst->present_us_sum += a.present_us_sum;
						dst->present_us_max = std::max(dst->present_us_max, a.present_us_max);
						dst->present_frames += a.present_frames;
					}
					for (const auto &kv : a.ads_info) {
						dst->ads_info[kv.first] = kv.second;
					}
				}
			}
		}
	}
	p_demo.set_tuning(base);

	auto row = [&](const std::string &p_id, bool p_pass, const std::string &p_detail) { rows.push_back({ p_id, p_pass, p_detail }); };
	std::string fovs;
	for (const float f : all.render_fovs) {
		fovs += fmt(" %.0f", double(f));
	}
	row("e19.fov.hip", all.hip_marks > 0 && all.hip_bad == 0, fmt("%u hip marks, Camera3D vertical FOV 55 (+-%.0e) at %u", all.hip_marks, double(kFovTolerance), all.hip_marks - all.hip_bad));
	row("e19.fov.ads", all.ads_marks > 0 && all.ads_bad == 0, fmt("%u aimed marks, Camera3D vertical FOV 40 at %u", all.ads_marks, all.ads_marks - all.ads_bad));
	row("e19.vmfov.hip", all.hip_marks > 0 && all.vm_hip_bad == 0,
			fmt("weapon and arms fov_override == RenderFov at %u of %u hip marks; RenderFov values:%s", all.hip_marks - all.vm_hip_bad, all.hip_marks, fovs.c_str()));
	for (int mi = 0; mi < 2; ++mi) {
		const Accum &a = per_mode[mi];
		const char *mode = modes[mi] ? "specific" : "generic";
		row(std::string("e19.fov.in_out.") + mode, a.transitions > 0 && a.transitions_bad == 0,
				fmt("%u transitions at 60 / 144 fps, worst |in - FOV time| %.2f frames, |out - FOV time| %.2f frames (<= 1)", a.transitions, a.worst_in_frames,
						a.worst_out_frames));
	}
	{

		const Accum &g = per_mode[1];
		const bool literal = g.fov_seconds_min <= g.fov_seconds_max && std::fabs(g.fov_seconds_min - kE19GenericFovSeconds) <= 1e-6f &&
				std::fabs(g.fov_seconds_max - kE19GenericFovSeconds) <= 1e-6f;
		row("e19.fov.generic_0.15", literal && g.transitions > 0 && g.transitions_bad == 0,
				fmt("generic mode: the FOV times in use %.4f..%.4f s (== %.2f s, m2a.md E19), %u transitions within +-1 frame, worst %.2f / %.2f frames",
						double(g.fov_seconds_min), double(g.fov_seconds_max), double(kE19GenericFovSeconds), g.transitions, g.worst_in_frames, g.worst_out_frames));
	}
	row("e19.recoil", all.frames > 0 && all.worst_recoil_deg <= kRecoilToleranceDeg && all.max_recoil_deg > 0.1,
			fmt("%llu frames, worst |camera recoil offset - interpolated predicted recoil| %.2e deg (<= %.0e), largest recoil %.2f deg", (unsigned long long)all.frames,
					all.worst_recoil_deg, kRecoilToleranceDeg, all.max_recoil_deg));
	const bf3::anim::ViewmodelTuning &tu = base;
	const double settle_max = std::max({ tu.sway.settle_seconds, tu.bob.settle_seconds, tu.kick.settle_seconds, tu.camera_recoil.settle_seconds });
	row("e19.springs", all.settle_marks > 0 && all.settle_bad == 0 && all.worst_unsettled_s <= settle_max + 1.0 / 60.0 + 1e-6,
			fmt("worst unsettled frame %.3f s after the last spring input (<= largest settle_seconds %.2f + 1 frame); at rest at %u of %u settle marks "
				"(mark + sway / bob settle_seconds)",
					all.worst_unsettled_s, settle_max, all.settle_marks - all.settle_bad, all.settle_marks));
	{

		const char *names[4] = { "sway", "bob", "kick", "camera" };
		const bf3::anim::SpringParams *ps[4] = { &tu.sway, &tu.bob, &tu.kick, &tu.camera_recoil };
		for (size_t k = 0; k < 4; ++k) {
			const double excess = all.spring_excess_frames[k] < -1e8 ? 0.0 : all.spring_excess_frames[k];
			row(std::string("e19.springs.") + names[k], all.spring_inputs[k] > 0 && excess <= 1.0 + 1e-6,
					fmt("%llu input frames; worst %.3f s not at rest after the last input (settle_seconds %.2f; %+.2f frames past it, <= 1)",
							(unsigned long long)all.spring_inputs[k], all.spring_worst_s[k], double(ps[k]->settle_seconds), excess));
		}
	}
	row("e19.zoom_refused", all.refused_frames > 0 && all.refused_bad == 0,
			fmt("%llu frames with zoom held while deploying or sprinting: ADS presented in %llu (presentation rule anim::ads_allowed, §16 Q21)",
					(unsigned long long)all.refused_frames, (unsigned long long)all.refused_bad));
	print(fmt("[vm] info e19.zoom_refused.sim: %s (the sim set kZoomWasDown in %llu of those frames; step_weapons_with_rules is the stub until sv lands the rule)",
			all.sim_flag_frames > 0 ? "PENDING (sv)" : "the sim refuses too", (unsigned long long)all.sim_flag_frames));
	row("e5.clip_start", all.reloads > 0 && all.clip_start_bad == 0,
			fmt("%u reloads: the reload clip on screen at most %.4f s after kReloadStarted (<= 1 display frame)", all.reloads, all.worst_clip_start_s));
	row("e5.fire_after_finish", all.reloads > 0 && all.fire_bad == 0,
			fmt("%u reloads: the first shot on screen at most %.4f s after kReloadFinished (<= 1 display frame)", all.reloads, all.worst_fire_after_s));
	row("e5.clip_cut", all.reloads > 0 && all.cut_bad == 0,
			fmt("%u reloads: the shot takes the reload clip off within %.3f s (<= crossfade %.2f + 1 frame; authored rate, §16 Q4)", all.reloads, all.worst_cut_s,
					double(bf3::anim::kViewmodelCrossfadeSeconds)));

	print("[vm] grenade throws (specific FOV mode passes):\n" + all.throw_lines);
	row("vm.grenade_flow", all.throws > 0 && all.throws_bad == 0,
			fmt("%u scripted throws: the hand grenade's FireStart > FireRelease > Deploy > idle clips from "
				"1P_WeaponHandlingWrapper's state flow, the release "
				"entered at its branch-in tick on kThrown, the redeploy at its end branch, the deploy clip played "
				"whole (%u bad)",
					all.throws, all.throws_bad));
	{
		uint32_t items = 0, covered = 0, clips = 0, stand_ins = 0, na = 0;
		std::string lines;

		const std::vector<bf3::render::ViewmodelKit> &kits = p_demo.kit_defs();
		for (size_t k = 0; k < kits.size(); ++k) {
			const int team = kits[k].team == "RU" ? 2 : 1;
			const bf3::render::ViewmodelSet &set = p_demo.set(team);
			for (const std::string &u : kits[k].unlocks) {
				const bf3::render::ViewmodelItem *it = nullptr;
				for (const bf3::render::ViewmodelItem &i : set.items) {
					if (i.unlock == u) {
						it = &i;
					}
				}
				++items;
				if (it == nullptr) {
					lines += fmt("    %s %s: NOT LOADED\n", kits[k].team.c_str(), short_unlock(u).c_str());
					continue;
				}
				const bf3::render::ViewmodelCoverage c = bf3::render::viewmodel_coverage(*it);
				covered += c.covered ? 1 : 0;
				clips += c.clips;
				stand_ins += c.stand_ins;
				na += c.not_applicable;
				std::string l = fmt("    %s %-14s %-12s", kits[k].team.c_str(), kits[k].kit.substr(kits[k].kit.rfind('/') + 1).c_str(),
						short_unlock(u).c_str());
				for (const bf3::render::ViewmodelCoverageEntry &e : c.entries) {
					l += fmt(" | %s: %s", e.what, e.applicable ? bf3::render::viewmodel_clip_source_name(e.source) : "n/a");
					if (e.applicable && !e.name.empty()) {
						l += " " + e.name;
					}
				}
				lines += l + (c.covered ? "\n" : "  NOT COVERED\n");
			}
		}
		print("[vm] E11 viewmodel coverage (deploy, fire, reload per item of the 8 loadouts):\n" + lines);
		row("e11.coverage", covered == items && kits.size() == 2 * bf3::render::kViewmodelKitsPerTeam && items == kits.size() * 4,
				fmt("%u of %u items of the %zu loadouts read from the data (%s) covered: %u clips, %u counted stand-ins, %u not applicable", covered, items,
						kits.size(), std::string(bf3::render::kViewmodelKitsMode).c_str(), clips, stand_ins, na));
		row("e11.kits_match_data", p_demo.kit_defs_difference().empty(),
				p_demo.kit_defs_difference().empty() ? std::string("the data's 8 default loadouts equal the documented table (m2a.md §5.6, kits.h)")
													 : "the data differs from the documented table: " + p_demo.kit_defs_difference());
	}

	{
		uint32_t data_slots = 0, gpu = 0, items = 0, knives = 0, knives_hidden = 0, dogtag_drawn = 0;
		std::string lines;
		for (const int team : { 1, 2 }) {
			const bf3::render::ViewmodelSet &set = p_demo.set(team);
			for (size_t i = 0; i < set.items.size(); ++i) {
				const bf3::render::ViewmodelItem &it = set.items[i];
				std::set<uint32_t> used;
				for (const bf3::render::PreparedSurface &sf : it.weapon.prepared.surfaces) {
					if (sf.material_index < it.weapon_looks.size() && it.weapon_looks[sf.material_index].alpha_test) {
						used.insert(sf.material_index);
					}
				}
				uint32_t want = uint32_t(used.size());
				for (const bf3::render::ViewmodelAttachment &a : it.attachments) {
					std::set<uint32_t> au;
					for (const bf3::render::PreparedSurface &sf : a.part.prepared.surfaces) {
						if (sf.material_index < a.looks.size() && a.looks[sf.material_index].alpha_test) {
							au.insert(sf.material_index);
						}
					}
					want += uint32_t(au.size());
				}
				const uint32_t have = uint32_t(p_demo.viewmodel(team)->alpha_scissor_materials(int(i)));
				data_slots += want;
				gpu += have;
				++items;
				if (it.kind == bf3::render::ViewmodelItemKind::Knife) {
					++knives;
					knives_hidden += it.hidden_surfaces > 0 ? 1 : 0;
					for (const bf3::render::PreparedSurface &sf : it.weapon.prepared.surfaces) {
						if (sf.material_index < it.weapon.materials.slots.size() &&
								it.weapon.materials.slots[sf.material_index].texture("Diffuse").find(bf3::render::kDogTagTexture) != std::string::npos) {
							++dogtag_drawn;
						}
					}
				}
				if (want > 0 || have > 0) {
					lines += fmt("    %s %s: %u alpha-tested slots in the data, %u alpha-scissor materials\n", team == 2 ? "RU" : "US", short_unlock(it.unlock).c_str(),
							want, have);
				}
			}
		}
		print("[vm] alpha-tested viewmodel slots (level shader database):\n" + lines);
		row("vm.alpha_test", data_slots == gpu,
				fmt("%u alpha-tested drawn slots over %u items, %u drawn with an alpha scissor (the knife's dog-tag chain when --vm-dogtags)", data_slots,
						items, gpu));
		row("vm.knife_dog_tags", knives > 0 && (p_demo.knife_dog_tags() ? dogtag_drawn > 0 : (knives_hidden == knives && dogtag_drawn == 0)),
				fmt("%u knives: dog tags %s (%u with hidden surfaces, %u dog-tag surfaces drawn; render::kDogTagTexture stand-in)", knives,
						p_demo.knife_dog_tags() ? "drawn (--vm-dogtags)" : "hidden", knives_hidden, dogtag_drawn));
	}

	{
		std::string lines;
		std::set<std::string> seen;
		for (const int team : { 1, 2 }) {
			for (const bf3::render::ViewmodelItem &it : p_demo.set(team).items) {
				if (it.kind != bf3::render::ViewmodelItemKind::Gun || !seen.insert(it.unlock).second) {
					continue;
				}
				const bf3::render::ViewmodelClip &r = it.clips[size_t(bf3::render::ViewmodelClipRole::Reload)];
				const bf3::render::ViewmodelClip &e = it.clips[size_t(bf3::render::ViewmodelClipRole::ReloadEmpty)];
				lines += fmt("    %-10s tactical ReloadTimeBulletsLeft %.2f s, clip %.3f s (%s)  empty ReloadTime %.2f s, clip %.3f s (%s)\n",
						short_unlock(it.unlock).c_str(), double(it.reload_time_bullets_left), double(r.seconds), bf3::render::viewmodel_clip_source_name(r.source),
						double(it.reload_time), double(e.seconds), bf3::render::viewmodel_clip_source_name(e.source));
			}
		}
		print("[vm] E5 reload clip lengths (authored rate, §16 Q4: a shot after the finish cuts the clip):\n" + lines);
	}
	print("[vm] E5 reload timings (specific FOV mode passes):\n" + all.e5_lines);
	print("[vm] E19 FOV transitions (specific mode):\n" + per_mode[0].transition_lines);
	print("[vm] E19 FOV transitions (generic mode):\n" + per_mode[1].transition_lines);
	{
		std::string l;
		for (const auto &kv : all.ads_info) {
			l += "    " + kv.first + ": " + kv.second + "\n";
		}
		print("[vm] info at full ADS (look scale, viewmodel FOV, optic vignette; iron sights draw no vignette, §16 Q6):\n" + l);
	}
	if (!all.fov_bad.empty()) {
		print("[vm] FOV misses:" + all.fov_bad);
	}
	print(fmt("[vm] info present cost: mean %.1f us, max %.1f us per frame over %llu frames at 60 fps (noisy on the shared machine)",
			all.present_frames ? all.present_us_sum / double(all.present_frames) : 0.0, all.present_us_max, (unsigned long long)all.present_frames));
	print(fmt("[vm] load %.2f s", p_demo.load_seconds()));

	uint32_t pass = 0;
	for (const Row &r : rows) {
		pass += r.pass ? 1 : 0;
		print(std::string("[vm] ") + (r.pass ? "PASS " : "FAIL ") + r.id + ": " + r.detail);
	}
	const bool ok = pass == rows.size();
	print(fmt("M2A VIEWMODEL CHECK: %s (%u/%zu)", ok ? "PASS" : "FAIL", pass, rows.size()));
	return ok ? 0 : 1;
}

}
