#include "bot_vehicles_check.h"

#include "level_session.h"

#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
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
constexpr float kSeatHipsMetres = 0.001f;
constexpr float kSideBack = 4.0f;
constexpr float kSideUp = 1.0f;
constexpr double kSideSeconds = 0.5;
constexpr float kQuarterTurn = 1.5707964f;
constexpr double kCrewTimeout = 120.0;

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
	const float heading = heading_ + side_;
	const float back = side_ != 0.0f ? kSideBack : kCameraBack, up = side_ != 0.0f ? kSideUp : kCameraUp;
	const float sy = std::sin(heading), cy = std::cos(heading);
	const float pitch = -std::atan2(up, back);
	r_session.mapper().script_view(heading, pitch);
	r_session.cameras().set_free(true);
	r_session.cameras().place_free(Vector3(at_[0] - sy * back, at_[1] + up, at_[2] - cy * back), heading, pitch);
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

void BotVehiclesCheck::see_drawn(const host::PresentationFrame &p_frame, const BotVehiclesInputs &p_in, int p_only) {
	for (const SeatedSoldierRoot &r : last_roots_) {
		sim::Vec3 at{};
		if ((p_only >= 0 && r.slot != p_only) || !p_in.drawn_seated || !bot_slot(p_frame, r.slot) ||
				!p_in.drawn_seated(r.slot, at)) {
			continue;
		}
		++drawn_;
		drawn_off_ = float(r.root.origin.distance_to(Vector3(at[0], at[1], at[2])));
		worst_off_ = std::max(worst_off_, drawn_off_);
		on_entry_ += drawn_off_ < kSeatHipsMetres ? 1u : 0u;
	}
	last_roots_.assign(p_in.seat_roots.begin(), p_in.seat_roots.end());
}

bool BotVehiclesCheck::find_crew(const host::PresentationFrame &p_frame, const BotVehiclesInputs &p_in) {
	for (size_t i = 0; i < p_frame.vehicle_count && i < p_frame.vehicles.size(); ++i) {
		const host::PresentedVehicle &v = p_frame.vehicles[i];
		for (size_t k = 1; k < v.seat_count && k < v.occupants.size(); ++k) {
			const uint8_t slot = v.occupants[k];
			if (slot == net::kNoVehicleOccupant || !bot_slot(p_frame, slot) ||
					std::none_of(p_in.seat_roots.begin(), p_in.seat_roots.end(),
							[&](const SeatedSoldierRoot &p_r) { return p_r.slot == slot; })) {
				continue;
			}
			crew_slot_ = slot;
			crew_seat_ = k;
			spawner_ = v.spawner;
			return true;
		}
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
			see_drawn(f, p_in);
			framed_ += p_delta;
			if (framed_ < kFrameSeconds) {
				return false;
			}
			row("bots_seated", true,
					fmt("%u bots in %u vehicles after %.1f s; framed spawner %u at %.1f %.1f %.1f", seated_, vehicles_,
							t_ - framed_, unsigned(spawner_), double(at_[0]), double(at_[1]), double(at_[2])));
			row("bot_drawn_seated", on_entry_ > 0 && drawn_off_ >= 0.0f && drawn_off_ < kSeatHipsMetres,
					fmt("%u of %u bot-frames seated with the hips on the entry; last %.4f m, worst %.4f m off",
							on_entry_, drawn_, double(drawn_off_), double(worst_off_)));
			if (!shots_.empty() && p_in.shot) {
				row("shot", p_in.shot(shots_ + "/bots_in_vehicle.png"), shots_ + "/bots_in_vehicle.png");
			}
			side_ = kQuarterTurn;
			framed_ = 0.0;
			phase_ = Phase::Sides;
			return false;
		case Phase::Sides: {
			follow(f);
			aim(r_session);
			framed_ += p_delta;
			if (framed_ < kSideSeconds) {
				return false;
			}
			const std::string name = side_ > 0.0f ? "/bot_seated_side_a.png" : "/bot_seated_side_b.png";
			if (!shots_.empty() && p_in.shot) {
				row("side_shot", p_in.shot(shots_ + name), shots_ + name);
			}
			framed_ = 0.0;
			if (side_ > 0.0f) {
				side_ = -kQuarterTurn;
				return false;
			}
			side_ = 0.0f;
			boarded_ = at_;
			driving_ = 0.0;
			phase_ = Phase::Drive;
			return false;
		}
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
				phase_ = Phase::Crew;
				crew_wait_ = 0.0;
				return false;
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
		case Phase::Crew: {
			crew_wait_ += p_delta;
			if (crew_slot_ < 0) {
				if (find_crew(f, p_in)) {
					follow(f);
					framed_ = 0.0;
					drawn_ = 0;
					on_entry_ = 0;
					drawn_off_ = -1.0f;
					worst_off_ = 0.0f;
					side_ = kQuarterTurn;
					last_roots_.assign(p_in.seat_roots.begin(), p_in.seat_roots.end());
				} else if (crew_wait_ > kCrewTimeout) {
					row("bot_drawn_crew", false,
							fmt("no bot sat in a shown gunner or passenger entry within %.0f s", kCrewTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			const bool live = follow(f);
			aim(r_session);
			see_drawn(f, p_in, crew_slot_);
			framed_ += p_delta;
			if (live && framed_ < kFrameSeconds) {
				return false;
			}
			row("bot_drawn_crew", live && on_entry_ > 0 && drawn_off_ >= 0.0f && drawn_off_ < kSeatHipsMetres,
					fmt("slot %d in seat %zu of spawner %u: %u of %u bot-frames seated with the hips on the entry; "
						"last %.4f m, worst %.4f m off",
							crew_slot_, crew_seat_, unsigned(spawner_), on_entry_, drawn_, double(drawn_off_),
							double(worst_off_)));
			framed_ = 0.0;
			phase_ = Phase::CrewSides;
			return false;
		}
		case Phase::CrewSides: {
			follow(f);
			aim(r_session);
			framed_ += p_delta;
			if (framed_ < kSideSeconds) {
				return false;
			}
			const std::string name = side_ > 0.0f ? "/bot_crew_side_a.png" : "/bot_crew_side_b.png";
			if (!shots_.empty() && p_in.shot) {
				row("crew_shot", p_in.shot(shots_ + name), shots_ + name);
			}
			framed_ = 0.0;
			if (side_ > 0.0f) {
				side_ = -kQuarterTurn;
				return false;
			}
			phase_ = Phase::Done;
			return true;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
