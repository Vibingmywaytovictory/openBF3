#pragma once

#include "bf3/core/ui/commo_rose.h"
#include "bf3/host/client_session.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/spotter.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/commo.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bf3::godot_side {

inline constexpr float kCommoRoseHoldSeconds = 0.25f;

struct CommoFrameInput {
	const host::PresentationFrame *frame = nullptr;
	uint64_t now_ns = 0;
	double now_s = 0.0;
	float dt = 0.0f;
	bool held = false;
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;
	bool alive = false;
	sim::Vec3 eye{};
	sim::Vec3 view{};
	const host::GunplayLevel *gunplay = nullptr;
	const sim::CollisionWorld *world = nullptr;
};

struct CommoFrameOutput {
	std::optional<net::SpotRequest> spot;
	std::optional<net::OrderRequest> order;
};

struct CommoStats {
	uint64_t presses = 0;
	uint64_t spots_sent = 0;
	uint64_t orders_sent = 0;
	uint64_t refused = 0;
	uint64_t scans = 0;
	uint64_t rose_opens = 0;
	uint64_t rose_picks = 0;
	int32_t last_pick = -1;
};

std::optional<sim::commo::OrderType> rose_command(const core::ui::CommoRoseEntry *p_entry, bool p_in_vehicle);

size_t spot_subjects(const host::PresentationFrame &p_frame, uint64_t p_now_ns, const host::GunplayLevel *p_gunplay,
		std::span<host::SpotSubject> r_out);
sim::commo::Supplies own_supplies(const host::PresentationFrame &p_frame, const host::GunplayLevel *p_gunplay);

class CommoFrame {
public:
	CommoFrameOutput update(const CommoFrameInput &p_in);
	bool rose_open() const { return rose_; }
	void select(std::optional<sim::commo::OrderType> p_command) { selected_ = p_command; }
	std::optional<sim::commo::OrderType> selected() const { return selected_; }
	const CommoStats &stats() const { return stats_; }

private:
	std::optional<host::Spotter> spotter_;
	bool was_held_ = false;
	double pressed_at_ = 0.0;
	bool rose_ = false;
	std::optional<sim::commo::OrderType> selected_;
	CommoStats stats_;
};

}
