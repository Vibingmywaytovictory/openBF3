#pragma once
// PT1 iff (docs/design/pt1-iff.md §4): the other soldiers as the friend-or-foe parts see them in one display frame.
// Godot-free (bf3_host only), so test_pt1_iff runs it on recorded PresentationFrames without Godot.
//   iff_soldiers   present_others + present_other_weapons at the frame's draw time, with each slot's team (the
//                  weapon sample's 2-bit team, else the PlayerList row), kit, name and bot flag from the row, and alive
//                  from the sample's soldier_flags::kAlive
//   IffReveal      an enemy's minimap reveal by its own fire: a firing sample with a newer sample tick than the last
//                  one seen for the slot sets revealed_until = now + SpottingRules::fire_reveal_seconds()
//   iff_minimap    the minimap's soldiers (ui::minimap_relation) for the local team

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
	uint8_t team = 0; // 0 unknown, 1, 2
	uint8_t kit = kIffNoKit; // host::KitRole from the player row
	uint8_t squad = 0; // none in M2b
	bool alive = false;
	bool bot = false;
	bool firing = false; // the weapon sample fired since the previous snapshot tick
	uint32_t sample_tick = 0; // of the weapon sample
	sim::Vec3 feet{};
	float eye_height = 1.6f;
	float yaw = 0.0f; // radians
	std::string_view name; // the player row's name (into the frame; empty without a row)
};

// At most r_out.size() soldiers, in present_others' order; returns the count.
size_t iff_soldiers(const host::PresentationFrame &p_frame, uint64_t p_now_ns, std::span<IffSoldier> r_out);

class IffReveal {
public:
	void update(std::span<const IffSoldier> p_soldiers, double p_now_s, float p_reveal_seconds);
	double revealed_until(uint8_t p_slot) const;
	uint64_t reveals() const { return reveals_; } // shots that (re)started a reveal
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
	uint32_t enemy = 0; // revealed enemies drawn
	uint32_t hidden_enemies = 0; // alive enemies not revealed
};
// The soldiers the minimap draws (relation != None), at most r_out.size().
size_t iff_minimap(std::span<const IffSoldier> p_soldiers, const core::ui::NametagViewer &p_viewer, const IffReveal &p_reveal, double p_now_s,
		std::span<IffMinimapEntry> r_out, IffMinimapCounts *r_counts = nullptr);

} // namespace bf3::godot_side
