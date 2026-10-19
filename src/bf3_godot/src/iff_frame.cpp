// PT1 iff: the other soldiers for the friend-or-foe parts (iff_frame.h; docs/design/pt1-iff.md §4).
#include "iff_frame.h"

#include "bf3/host/client_wiring.h"
#include "bf3/sim/soldier_state.h"

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

size_t iff_soldiers(const host::PresentationFrame &p_frame, uint64_t p_now_ns, std::span<IffSoldier> r_out) {
	if (!p_frame.valid || p_frame.other_count == 0 || r_out.empty()) {
		return 0;
	}
	std::array<host::PresentedSoldier, net::kMaxSlots> others{};
	std::array<host::PresentedOtherWeapon, net::kMaxSlots> weapons{};
	const size_t n = host::present_others(p_frame, p_now_ns, others);
	const size_t nw = host::present_other_weapons(p_frame, host::present_render_ticks(p_frame, p_now_ns), weapons);
	const size_t rows = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	size_t out = 0;
	for (size_t i = 0; i < n && out < r_out.size(); ++i) {
		const host::PresentedSoldier &p = others[i];
		IffSoldier s;
		s.slot = p.slot;
		s.feet = p.position;
		s.eye_height = std::isfinite(p.eye_height) ? std::clamp(p.eye_height, 0.0f, 3.0f) : 1.6f;
		s.yaw = p.yaw;
		s.alive = (p.flags & sim::soldier_flags::kAlive) != 0;
		if (i < nw && weapons[i].valid && weapons[i].slot == p.slot) {
			s.team = weapons[i].team <= 2 ? weapons[i].team : uint8_t(0);
			s.firing = weapons[i].firing;
			s.sample_tick = weapons[i].sample_tick;
		}
		// The player row (ascending slot): kit, name, bot; the team when the sample has none.
		const auto row = std::lower_bound(p_frame.players.begin(), p_frame.players.begin() + std::ptrdiff_t(rows), p.slot,
				[](const net::PlayerRow &p_row, uint8_t p_slot) { return p_row.slot < p_slot; });
		if (row != p_frame.players.begin() + std::ptrdiff_t(rows) && row->slot == p.slot) {
			if (s.team == 0 && row->team <= 2) {
				s.team = row->team;
			}
			s.kit = row->kit < 4 ? row->kit : kIffNoKit;
			s.bot = row->bot;
			const size_t len = std::min<size_t>(row->name_bytes, row->name.size());
			s.name = std::string_view(row->name.data(), len);
		}
		r_out[out++] = s;
	}
	return out;
}

void IffReveal::update(std::span<const IffSoldier> p_soldiers, double p_now_s, float p_reveal_seconds) {
	const double hold = std::isfinite(p_reveal_seconds) ? std::clamp(double(p_reveal_seconds), 0.0, 3600.0) : 0.0;
	for (const IffSoldier &s : p_soldiers) {
		if (s.slot >= net::kMaxSlots) {
			continue;
		}
		if (s.firing && s.sample_tick != last_tick_[s.slot]) {
			until_[s.slot] = p_now_s + hold;
			++reveals_;
		}
		if (s.firing) {
			last_tick_[s.slot] = s.sample_tick;
		}
		if (!s.alive) {
			until_[s.slot] = 0.0; // a new life starts unrevealed
		}
	}
}

double IffReveal::revealed_until(uint8_t p_slot) const {
	return p_slot < net::kMaxSlots ? until_[p_slot] : 0.0;
}

void IffReveal::reset() {
	last_tick_ = {};
	until_ = {};
	reveals_ = 0;
}

size_t iff_minimap(std::span<const IffSoldier> p_soldiers, const core::ui::NametagViewer &p_viewer, const IffReveal &p_reveal, double p_now_s,
		std::span<IffMinimapEntry> r_out, IffMinimapCounts *r_counts) {
	IffMinimapCounts c;
	size_t out = 0;
	for (const IffSoldier &s : p_soldiers) {
		const core::ui::TagRelation r = core::ui::minimap_relation(p_viewer, s.team, s.squad, s.alive, p_now_s, p_reveal.revealed_until(s.slot));
		if (r == core::ui::TagRelation::None) {
			if (s.alive && core::ui::tag_relation(p_viewer, s.team, s.squad) == core::ui::TagRelation::Enemy) {
				++c.hidden_enemies;
			}
			continue;
		}
		c.friendly += r == core::ui::TagRelation::Friendly ? 1u : 0u;
		c.squad += r == core::ui::TagRelation::Squad ? 1u : 0u;
		c.enemy += r == core::ui::TagRelation::Enemy ? 1u : 0u;
		if (out < r_out.size()) {
			r_out[out++] = IffMinimapEntry{ s.slot, r, s.feet, s.yaw };
		}
	}
	if (r_counts != nullptr) {
		*r_counts = c;
	}
	return out;
}

} // namespace bf3::godot_side
