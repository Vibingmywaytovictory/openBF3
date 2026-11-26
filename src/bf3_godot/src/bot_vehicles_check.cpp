#include "bot_vehicles_check.h"

#include "level_session.h"

#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;

namespace {

constexpr double kSeatedTimeout = 120.0;
constexpr double kFrameSeconds = 1.5;
constexpr float kCameraBack = 9.0f;
constexpr float kCameraUp = 4.0f;
constexpr double kDriveTimeout = 90.0;
constexpr float kMovedMetres = 4.0f;

bool bot_slot(const host::PresentationFrame &p_frame, uint8_t p_slot) {
	for (size_t i = 0; i < p_frame.player_count && i < p_frame.players.size(); ++i) {
		if (p_frame.players[i].slot == p_slot) {
			return p_frame.players[i].bot;
		}
	}
	return false;
}

}

BotVehiclesCheck::BotVehiclesCheck(std::string p_shots) : shots_(std::move(p_shots)) {}

void BotVehiclesCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("BOT VEHICLES ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void BotVehiclesCheck::aim(Bf3LevelSession &r_session) const {
	const float sy = std::sin(heading_), cy = std::cos(heading_);
	const float pitch = -std::atan2(kCameraUp, kCameraBack);
	r_session.mapper().script_view(heading_, pitch);
	r_session.cameras().set_free(true);
	r_session.cameras().place_free(
			Vector3(at_[0] - sy * kCameraBack, at_[1] + kCameraUp, at_[2] - cy * kCameraBack), heading_, pitch);
}

bool BotVehiclesCheck::follow(const host::PresentationFrame &p_frame) {
	for (size_t i = 0; i < p_frame.vehicle_count && i < p_frame.vehicles.size(); ++i) {
		const host::PresentedVehicle &v = p_frame.vehicles[i];
		if (v.spawner != spawner_) {
			continue;
		}
		at_ = v.com_position;
		const core::Transform hull = sim::vehicles::hull_transform(v.rotation, {});
		heading_ = std::atan2(hull.forward[0], hull.forward[2]);
		return true;
	}
	return false;
}

bool BotVehiclesCheck::step(Bf3LevelSession &r_session, const BotVehiclesInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	t_ += p_delta;
	switch (phase_) {
		case Phase::WaitSeated: {
			seated_ = 0;
			vehicles_ = 0;
			for (size_t i = 0; i < f.vehicle_count && i < f.vehicles.size(); ++i) {
				const host::PresentedVehicle &v = f.vehicles[i];
				uint32_t bots = 0;
				for (size_t k = 0; k < v.seat_count && k < v.occupants.size(); ++k) {
					bots += v.occupants[k] != net::kNoVehicleOccupant && bot_slot(f, v.occupants[k]) ? 1u : 0u;
				}
				if (bots == 0) {
					continue;
				}
				if (vehicles_ == 0) {
					spawner_ = v.spawner;
					at_ = v.com_position;
					const core::Transform hull = sim::vehicles::hull_transform(v.rotation, {});
					heading_ = std::atan2(hull.forward[0], hull.forward[2]);
				}
				seated_ += bots;
				++vehicles_;
			}
			if (vehicles_ > 0) {
				phase_ = Phase::Frame;
				framed_ = 0.0;
				aim(r_session);
				return false;
			}
			if (t_ > kSeatedTimeout) {
				row("bots_seated", false, fmt("no bot sat in a vehicle within %.0f s", kSeatedTimeout));
				phase_ = Phase::Done;
				return true;
			}
			return false;
		}
		case Phase::Frame:
			follow(f);
			aim(r_session);
			framed_ += p_delta;
			if (framed_ < kFrameSeconds) {
				return false;
			}
			row("bots_seated", true,
					fmt("%u bots in %u vehicles after %.1f s; framed spawner %u at %.1f %.1f %.1f", seated_, vehicles_,
							t_ - framed_, unsigned(spawner_), double(at_[0]), double(at_[1]), double(at_[2])));
			if (!shots_.empty() && p_in.shot) {
				row("shot", p_in.shot(shots_ + "/bots_in_vehicle.png"), shots_ + "/bots_in_vehicle.png");
			}
			boarded_ = at_;
			driving_ = 0.0;
			phase_ = Phase::Drive;
			return false;
		case Phase::Drive: {
			driving_ += p_delta;
			const bool live = follow(f);
			aim(r_session);
			const float moved = std::hypot(at_[0] - boarded_[0], at_[2] - boarded_[2]);
			if (live && moved > kMovedMetres) {
				row("bot_driving", true,
						fmt("spawner %u moved %.1f m in %.1f s, now at %.1f %.1f %.1f", unsigned(spawner_),
								double(moved), driving_, double(at_[0]), double(at_[1]), double(at_[2])));
				if (!shots_.empty() && p_in.shot) {
					row("drive_shot", p_in.shot(shots_ + "/bot_driving.png"), shots_ + "/bot_driving.png");
				}
				phase_ = Phase::Done;
				return true;
			}
			if (!live || driving_ > kDriveTimeout) {
				row("bot_driving", false,
						fmt("spawner %u %s, moved %.1f m in %.1f s", unsigned(spawner_), live ? "live" : "gone",
								double(moved), driving_));
				phase_ = Phase::Done;
				return true;
			}
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
