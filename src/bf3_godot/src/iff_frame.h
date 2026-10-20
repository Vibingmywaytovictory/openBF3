#pragma once

#include "bf3/core/ui/nametags.h"
#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace bf3::godot_side {

inline constexpr uint8_t kIffNoKit = 0xFF;

struct IffSoldier {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint8_t kit = kIffNoKit;
	uint8_t squad = 0;
	bool alive = false;
	bool bot = false;
	bool firing = false;
	uint32_t sample_tick = 0;
	sim::Vec3 feet{};
	float eye_height = 1.6f;
	float yaw = 0.0f;
	std::string_view name;
};

size_t iff_soldiers(const host::PresentationFrame &p_frame, uint64_t p_now_ns, std::span<IffSoldier> r_out);

class IffReveal {
public:
	void update(std::span<const IffSoldier> p_soldiers, double p_now_s, float p_reveal_seconds);
	double revealed_until(uint8_t p_slot) const;
	uint64_t reveals() const { return reveals_; }
	void reset();

private:
	std::array<uint32_t, net::kMaxSlots> last_tick_{};
	std::array<double, net::kMaxSlots> until_{};
	uint64_t reveals_ = 0;
};

struct IffMinimapEntry {
	uint8_t slot = 0;
	core::ui::TagRelation relation = core::ui::TagRelation::None;
	sim::Vec3 feet{};
	float yaw = 0.0f;
};
struct IffMinimapCounts {
	uint32_t friendly = 0;
	uint32_t squad = 0;
	uint32_t enemy = 0;
	uint32_t hidden_enemies = 0;
};

size_t iff_minimap(std::span<const IffSoldier> p_soldiers, const core::ui::NametagViewer &p_viewer, const IffReveal &p_reveal, double p_now_s,
		std::span<IffMinimapEntry> r_out, IffMinimapCounts *r_counts = nullptr);

}
