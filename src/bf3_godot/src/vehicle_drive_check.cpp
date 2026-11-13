#include "vehicle_drive_check.h"

#include "game_vehicles.h"
#include "level_session.h"
#include "native_hud.h"
#include "viewmodel.h"

#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;

namespace {

constexpr double kScreenTimeout = 60.0;
constexpr double kDeployTimeout = 30.0;
constexpr double kDeployPressEvery = 2.0;
constexpr double kPickTimeout = 60.0;
constexpr double kPickSettle = 3.0;
constexpr double kWalkTimeout = 5.0;
constexpr double kKeyHold = 0.3;
constexpr double kSeatTimeout = 3.0;
constexpr double kSettle = 1.0;
constexpr double kThrottle = 1.5;
constexpr double kBrakeTimeout = 6.0;
constexpr float kDriveMetres = 4.0f;
constexpr float kOffLine = 1.5f;
constexpr const char *kJeep = "vehicles/growleritv/growleritv";
constexpr const char *kTank = "vehicles/m1a2/m1abrams";
constexpr double kZoomSettle = 0.5;

float up_y(const std::array<float, 4> &p_q) {
	return 1.0f - 2.0f * (p_q[0] * p_q[0] + p_q[2] * p_q[2]);
}

float speed(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

const host::PresentedVehicle *presented(const host::PresentationFrame &p_frame, uint16_t p_spawner) {
	for (size_t i = 0; i < p_frame.vehicle_count && i < p_frame.vehicles.size(); ++i) {
		if (p_frame.vehicles[i].spawner == p_spawner) {
			return &p_frame.vehicles[i];
		}
	}
	return nullptr;
}

const host::PresentedVehicle *parked(
		const host::PresentationFrame &p_frame, const host::LevelVehicles &p_lv, const char *p_blueprint) {
	const host::PresentedVehicle *best = nullptr;
	for (size_t i = 0; i < p_frame.vehicle_count && i < p_frame.vehicles.size(); ++i) {
		const host::PresentedVehicle &v = p_frame.vehicles[i];
		if (v.spawner >= p_lv.spawners.size() || p_lv.kinds[p_lv.spawners[v.spawner].kind].blueprint != p_blueprint) {
			continue;
		}
		if (up_y(v.rotation) > 0.98f && speed(v.linear_velocity) < 0.05f &&
				(best == nullptr || v.spawner < best->spawner)) {
			best = &v;
		}
	}
	return best;
}

std::optional<float> first_person_zoom(const host::LevelVehicleKind &p_kind, uint8_t p_seat) {
	if (p_seat >= p_kind.seat_defs.size()) {
		return std::nullopt;
	}
	for (const core::gameplay::VehicleCameraDef &c : p_kind.seat_defs[p_seat].cameras) {
		if (c.first_person) {
			return c.zoom_field_of_view;
		}
	}
	return std::nullopt;
}

}

void VehicleDriveRun::hud_row(const VehicleDriveInputs &p_in, uint8_t p_seat) {
	if (p_in.hud == nullptr) {
		row("hud", false, "no native HUD");
		return;
	}
	const core::ui::HudVehicle &v = p_in.hud->model().vehicle;
	const auto shown = [&](const char *p_widget) {
		const std::optional<NativeHudClipState> s = p_in.hud->clip_state(0, std::string("instance1.") + p_widget);
		return s && s->visible;
	};
	const core::ui::HudPassenger *own =
			p_seat < v.passenger_list.count ? &v.passenger_list.passengers[p_seat] : nullptr;
	const bool vehicle_health = shown("VehicleHealth_01"), passengers = shown("PassangerList_01");
	const bool health = shown("Health_01"), ammo = shown("Ammo_01");
	row("hud",
			v.in_vehicle && v.health == 100.0f && !v.damage_state_disabled && own != nullptr && own->is_player &&
					own->seat == core::ui::HudPassengerSeat::Driver && vehicle_health && passengers && !health && !ammo,
			fmt("vehicle health %.0f %%, %u passenger rows, own row %s; VehicleHealth %s, PassangerList %s, Health %s, "
				"Ammo %s",
					double(v.health), unsigned(v.passenger_list.count),
					own != nullptr && own->is_player ? "bold" : "missing", vehicle_health ? "shown" : "hidden",
					passengers ? "shown" : "hidden", health ? "shown" : "hidden", ammo ? "shown" : "hidden"));
}

void VehicleDriveRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("VEHICLE DRIVE ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void VehicleDriveRun::enter(Phase p_phase) {
	phase_ = p_phase;
	t_ = 0.0;
	next_press_ = 0.0;
}

bool VehicleDriveRun::fail(const std::string &p_name, const std::string &p_detail) {
	row(p_name, false, p_detail);
	phase_ = Phase::Done;
	return true;
}

void VehicleDriveRun::shoot(const VehicleDriveInputs &p_in, const char *p_name) {
	if (!shots_.empty() && p_in.shot) {
		(void)p_in.shot(shots_ + "/" + p_name + ".png");
	}
}

bool VehicleDriveRun::step(Bf3LevelSession &r_session, const VehicleDriveInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	InputMapper &mapper = r_session.mapper();
	t_ += p_delta;
	if (p_in.vehicles == nullptr || !p_in.vehicles->built()) {
		return t_ > kScreenTimeout ? fail("built", "the vehicles were not loaded or built") : false;
	}
	const host::LevelVehicles &lv = *p_in.vehicles->level();
	const std::optional<OwnVehicleSeat> seat = p_in.vehicles->own_seat();
	switch (phase_) {
		case Phase::WaitScreen: {
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			if (in_round && p_in.alive) {
				mapper.set_scripted(true);
				mapper.release_all();
				enter(Phase::Pick);
				return false;
			}
			if (!in_round || !p_in.spawn_screen) {
				return t_ > kScreenTimeout ? fail("spawn_screen", fmt("no spawn screen after %.0f s", kScreenTimeout))
										   : false;
			}
			mapper.set_scripted(true);
			mapper.release_all();
			enter(Phase::Deploying);
			return false;
		}
		case Phase::Deploying:
			if (!p_in.alive) {
				if (p_in.spawn_screen && t_ >= next_press_) {
					p_in.deploy();
					next_press_ = t_ + kDeployPressEvery;
				}
				return t_ > kDeployTimeout
						? fail("deploy", fmt("not alive %.0f s after the spawn screen", kDeployTimeout))
						: false;
			}
			enter(Phase::Pick);
			return false;
		case Phase::Pick: {
			const host::PresentedVehicle *best = parked(f, lv, kJeep);
			if (best == nullptr || t_ < kPickSettle) {
				return t_ > kPickTimeout ? fail("pick", fmt("no parked upright Growler after %.0f s", kPickTimeout))
										 : false;
			}
			spawner_ = best->spawner;
			const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[spawner_].kind];
			host::ClientSession *client = r_session.client();
			if (client == nullptr) {
				return fail("pick", "no client session");
			}
			net::DebugRequest q;
			q.kind = net::DebugKind::Teleport;
			q.id = ++request_id_;
			q.position = { best->com_position[0] + 0.6f * kind.entry_radius, best->com_position[1] + 0.5f,
				best->com_position[2] };
			q.yaw = sim::quantize_yaw(-0.5f * sim::kPi);
			client->send_debug(q);
			row("pick", true,
					fmt("Growler of spawner %u at (%.1f, %.1f, %.1f), %u parts drawn", unsigned(spawner_),
							double(best->com_position[0]), double(best->com_position[1]), double(best->com_position[2]),
							unsigned(p_in.vehicles->view(spawner_) ? p_in.vehicles->view(spawner_)->parts : 0)));
			enter(Phase::Walk);
			return false;
		}
		case Phase::Walk: {
			const host::PresentedVehicle *v = presented(f, spawner_);
			if (v == nullptr) {
				return fail("walk", "the Growler went away");
			}
			const float dx = f.own_curr.position[0] - v->com_position[0],
						dz = f.own_curr.position[2] - v->com_position[2];
			const float reach = lv.kinds[lv.spawners[spawner_].kind].entry_radius;
			if (std::sqrt(dx * dx + dz * dz) > reach || t_ < 0.5) {
				return t_ > kWalkTimeout
						? fail("walk",
								  fmt("not within %.2f m of the Growler after %.0f s", double(reach), kWalkTimeout))
						: false;
			}
			mapper.script_hold(InputMapper::VehicleAction::Use, true);
			enter(Phase::Enter);
			return false;
		}
		case Phase::Enter:
			if (t_ >= kKeyHold) {
				mapper.script_hold(InputMapper::VehicleAction::Use, false);
			}
			if (!seat || seat->spawner != spawner_) {
				return t_ > kSeatTimeout ? fail("enter", "the use key did not seat the soldier") : false;
			}
			mapper.script_hold(InputMapper::VehicleAction::Use, false);
			row("enter", seat->seat == 0, fmt("seat %u of spawner %u", unsigned(seat->seat), unsigned(seat->spawner)));
			enter(Phase::Settle);
			return false;
		case Phase::Settle: {
			if (t_ < kSettle) {
				return false;
			}
			const std::optional<GameVehicleView> view = p_in.vehicles->view(spawner_);
			const Vector3 eye = p_in.camera != nullptr ? p_in.camera->get_global_position() : Vector3();
			const float inside = view ? eye.distance_to(view->body.origin) : 1e9f;
			const uint32_t glass = p_in.vehicles->stats().glass_surfaces;
			row("first_person",
					seat.has_value() && inside < 3.0f && (p_in.own_vm == nullptr || !p_in.own_vm->is_visible()) &&
							glass > 0 && !mapper.weapons_allowed(),
					fmt("camera %.2f m from the body origin, viewmodel %s, infantry weapon %s, %u glass surfaces "
						"drawn see-through",
							double(inside), p_in.own_vm != nullptr && p_in.own_vm->is_visible() ? "shown" : "hidden",
							mapper.weapons_allowed() ? "live" : "off", unsigned(glass)));
			hud_row(p_in, seat->seat);
			shoot(p_in, "vehicle_1p");
			const host::PresentedVehicle *v = presented(f, spawner_);
			if (v == nullptr) {
				return fail("settle", "the Growler went away");
			}
			start_x_ = v->com_position[0];
			start_z_ = v->com_position[2];
			const Vector3 forward = view ? view->body.basis.get_column(2) : Vector3(0, 0, 1);
			const float n = std::sqrt(forward.x * forward.x + forward.z * forward.z);
			heading_x_ = forward.x / n;
			heading_z_ = forward.z / n;
			first_wheel_angle_ = v->wheel_count > 0 ? v->wheel_angle[0] : 0.0f;
			last_wheel_ = view && !view->wheels.empty() ? view->wheels[0] : Transform3D();
			wheel_frames_ = wheel_turns_ = 0;
			mapper.script_hold(InputMapper::VehicleAction::Throttle, true);
			enter(Phase::Drive);
			return false;
		}
		case Phase::Drive: {
			if (t_ < kThrottle) {
				if (const std::optional<GameVehicleView> turning = p_in.vehicles->view(spawner_);
						turning && !turning->wheels.empty() && t_ > 0.5 * kThrottle) {
					++wheel_frames_;
					wheel_turns_ += turning->wheels[0].basis.is_equal_approx(last_wheel_.basis) ? 0u : 1u;
					last_wheel_ = turning->wheels[0];
				}
				return false;
			}
			const host::PresentedVehicle *v = presented(f, spawner_);
			const std::optional<GameVehicleView> view = p_in.vehicles->view(spawner_);
			if (v == nullptr || !view) {
				return fail("drive", "the Growler went away");
			}
			const float dx = v->com_position[0] - start_x_, dz = v->com_position[2] - start_z_;
			const float along = dx * heading_x_ + dz * heading_z_;
			const float off = std::fabs(dx * heading_z_ - dz * heading_x_);
			const float spun = v->wheel_count > 0
					? std::fabs(std::remainder(v->wheel_angle[0] - first_wheel_angle_, sim::kTwoPi))
					: 0.0f;
			const Vector3 eye = p_in.camera != nullptr ? p_in.camera->get_global_position() : Vector3();
			row("drive",
					seat.has_value() && along > kDriveMetres && off < kOffLine &&
							view->wheels.size() == v->wheel_count && wheel_frames_ > 0 && wheel_turns_ == wheel_frames_,
					fmt("%.1f m along its heading, %.2f m off it, %.1f km/h, gear %d, %zu wheel parts drawn, wheel 0 "
						"turned %.2f rad and its drawn part turned on %u of %u frames, camera %.2f m from the body",
							double(along), double(off), double(3.6f * speed(v->linear_velocity)), int(v->gear),
							view->wheels.size(), double(spun), unsigned(wheel_turns_), unsigned(wheel_frames_),
							double(eye.distance_to(view->body.origin))));
			const host::ClientStats &st = f.stats;
			const int64_t lead = int64_t(v->tick) - int64_t(f.newest_tick);
			const int64_t need = std::max<int64_t>(1, int64_t(st.rtt_ms * double(sim::kSimHz) / 1000.0));
			row("predicted",
					v->predicted == 1 && v->tick == f.tick && lead >= need && st.vehicle_prediction_starts > 0 &&
							st.vehicle_predicted_ticks > 0 && st.vehicle_prediction_failures == 0,
					fmt("drawn at tick %u, own tick %u, %lld ticks ahead of the newest snapshot (%lld needed at %.0f "
						"ms RTT), predicted %u; %llu starts, %llu predicted ticks, %llu replayed, %llu corrections "
						"(largest %.3f m), %llu failures",
							unsigned(v->tick), unsigned(f.tick), (long long)lead, (long long)need, st.rtt_ms,
							unsigned(v->predicted), (unsigned long long)st.vehicle_prediction_starts,
							(unsigned long long)st.vehicle_predicted_ticks,
							(unsigned long long)st.vehicle_replayed_ticks, (unsigned long long)st.vehicle_corrections,
							double(st.vehicle_max_correction_m), (unsigned long long)st.vehicle_prediction_failures));
			shoot(p_in, "vehicle_drive");
			mapper.script_press(InputMapper::VehicleAction::Camera);
			enter(Phase::ThirdPerson);
			return false;
		}
		case Phase::ThirdPerson: {
			if (t_ < 0.5) {
				return false;
			}
			const std::optional<GameVehicleView> view = p_in.vehicles->view(spawner_);
			if (!view || p_in.camera == nullptr) {
				return fail("third_person", "no Growler or camera");
			}
			const Vector3 off = p_in.camera->get_global_position() - view->body.origin;
			const float behind = -off.dot(view->body.basis.get_column(2));
			row("third_person", p_in.camera_counter % 2 == 1 && off.length() > 3.0f && behind > 0.0f,
					fmt("camera %.2f m from the body, %.2f m behind it", double(off.length()), double(behind)));
			shoot(p_in, "vehicle_3p");
			mapper.script_hold(InputMapper::VehicleAction::Throttle, false);
			mapper.script_hold(InputMapper::VehicleAction::Brake, true);
			enter(Phase::Stop);
			return false;
		}
		case Phase::Stop: {
			const host::PresentedVehicle *v = presented(f, spawner_);
			if (v == nullptr) {
				return fail("stop", "the Growler went away");
			}
			const float forward = v->linear_velocity[0] * heading_x_ + v->linear_velocity[2] * heading_z_;
			if (forward > 0.3f) {
				return t_ > kBrakeTimeout ? fail("stop",
													fmt("still %.1f km/h after %.0f s of the back pedal",
															double(3.6f * forward), kBrakeTimeout))
										  : false;
			}
			mapper.script_hold(InputMapper::VehicleAction::Brake, false);
			row("stop", true, fmt("stopped in %.2f s", t_));
			mapper.script_hold(InputMapper::VehicleAction::Seat2, true);
			enter(Phase::Seat);
			return false;
		}
		case Phase::Seat:
			if (t_ >= kKeyHold) {
				mapper.script_hold(InputMapper::VehicleAction::Seat2, false);
			}
			if (!seat || seat->seat != 1) {
				return t_ > kSeatTimeout ? fail("seat", "the second seat key did not move the soldier to seat 2")
										 : false;
			}
			mapper.script_hold(InputMapper::VehicleAction::Seat2, false);
			row("seat", true, "the second seat key moved the driver to seat 2");
			mapper.script_hold(InputMapper::VehicleAction::Exit, true);
			enter(Phase::Exit);
			return false;
		case Phase::Exit: {
			if (t_ >= kKeyHold) {
				mapper.script_hold(InputMapper::VehicleAction::Exit, false);
			}
			if (seat || t_ < 1.0) {
				return t_ > kSeatTimeout + 1.0 ? fail("exit", "the use key did not take the soldier out") : false;
			}
			const host::PresentedVehicle *v = presented(f, spawner_);
			const float dx = f.own_curr.position[0] - (v != nullptr ? v->com_position[0] : 0.0f);
			const float dz = f.own_curr.position[2] - (v != nullptr ? v->com_position[2] : 0.0f);
			const float away = std::sqrt(dx * dx + dz * dz);
			row("exit", p_in.alive && away > 1.0f && away < 6.0f && p_in.camera_counter == 0,
					fmt("out %.2f m from the Growler's centre, camera counter %u", double(away),
							unsigned(p_in.camera_counter)));
			if (!zoom_) {
				phase_ = Phase::Done;
				return true;
			}
			enter(Phase::TankPick);
			return false;
		}
		case Phase::TankPick: {
			const host::PresentedVehicle *best = parked(f, lv, kTank);
			if (best == nullptr) {
				return t_ > kPickTimeout ? fail("tank_pick", fmt("no parked upright M1A2 after %.0f s", kPickTimeout))
										 : false;
			}
			host::ClientSession *client = r_session.client();
			if (client == nullptr) {
				return fail("tank_pick", "no client session");
			}
			spawner_ = best->spawner;
			const float reach = 0.6f * lv.kinds[lv.spawners[spawner_].kind].entry_radius;
			const std::optional<GameVehicleView> view = p_in.vehicles->view(spawner_);
			const Vector3 side = view ? view->body.basis.get_column(0) : Vector3(1, 0, 0);
			const float n = std::max(1e-3f, std::sqrt(side.x * side.x + side.z * side.z));
			net::DebugRequest q;
			q.kind = net::DebugKind::Teleport;
			q.id = ++request_id_;
			q.position = { best->com_position[0] + reach * side.x / n, best->com_position[1] + 0.5f,
				best->com_position[2] + reach * side.z / n };
			q.yaw = sim::quantize_yaw(-0.5f * sim::kPi);
			client->send_debug(q);
			row("tank_pick", true,
					fmt("M1A2 of spawner %u at (%.1f, %.1f, %.1f)", unsigned(spawner_), double(best->com_position[0]),
							double(best->com_position[1]), double(best->com_position[2])));
			enter(Phase::TankWalk);
			return false;
		}
		case Phase::TankWalk: {
			const host::PresentedVehicle *v = presented(f, spawner_);
			if (v == nullptr) {
				return fail("tank_walk", "the M1A2 went away");
			}
			const float dx = f.own_curr.position[0] - v->com_position[0],
						dz = f.own_curr.position[2] - v->com_position[2];
			const float reach = lv.kinds[lv.spawners[spawner_].kind].entry_radius;
			if (std::sqrt(dx * dx + dz * dz) > reach || t_ < 0.5) {
				return t_ > kWalkTimeout
						? fail("tank_walk",
								  fmt("not within %.2f m of the M1A2 after %.0f s", double(reach), kWalkTimeout))
						: false;
			}
			mapper.script_hold(InputMapper::VehicleAction::Use, true);
			enter(Phase::TankEnter);
			return false;
		}
		case Phase::TankEnter:
			if (t_ >= kKeyHold) {
				mapper.script_hold(InputMapper::VehicleAction::Use, false);
			}
			if (!seat || seat->spawner != spawner_) {
				return t_ > kSeatTimeout ? fail("tank_enter", "the use key did not seat the soldier in the M1A2")
										 : false;
			}
			if (t_ < kSettle) {
				return false;
			}
			if (p_in.camera == nullptr) {
				return fail("tank_enter", "no camera");
			}
			mapper.script_hold(InputMapper::VehicleAction::Use, false);
			regular_fov_ = float(p_in.camera->get_fov());
			row("tank_enter", seat->seat == 0 && p_in.camera_counter == 0,
					fmt("seat %u of spawner %u, camera counter %u, FOV %.2f deg", unsigned(seat->seat),
							unsigned(seat->spawner), unsigned(p_in.camera_counter), double(regular_fov_)));
			shoot(p_in, "tank_1p");
			mapper.script_hold(InputMapper::WeaponAction::Ads, true);
			enter(Phase::Zoom);
			return false;
		case Phase::Zoom: {
			if (t_ < kZoomSettle) {
				return false;
			}
			const std::optional<float> want = first_person_zoom(lv.kinds[lv.spawners[spawner_].kind], 0);
			const float fov = float(p_in.camera->get_fov());
			row("zoom",
					seat.has_value() && want && std::fabs(fov - *want) < 0.01f &&
							std::fabs(regular_fov_ - *want) > 1.0f,
					fmt("camera FOV %.2f deg %.1f s into the held zoom key, the seat camera's AlternateView %s deg, "
						"regular %.2f deg",
							double(fov), kZoomSettle, want ? fmt("%.2f", double(*want)).c_str() : "-",
							double(regular_fov_)));
			shoot(p_in, "vehicle_zoom");
			mapper.script_hold(InputMapper::WeaponAction::Ads, false);
			enter(Phase::Unzoom);
			return false;
		}
		case Phase::Unzoom: {
			if (t_ < kZoomSettle) {
				return false;
			}
			const float fov = float(p_in.camera->get_fov());
			row("unzoom", seat.has_value() && std::fabs(fov - regular_fov_) < 0.01f,
					fmt("camera FOV %.2f deg %.1f s after the zoom key's release, regular %.2f deg", double(fov),
							kZoomSettle, double(regular_fov_)));
			phase_ = Phase::Done;
			return true;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
