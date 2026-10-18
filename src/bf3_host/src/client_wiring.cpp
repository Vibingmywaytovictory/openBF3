// W3.0 contract (docs/design/m2a-w3.md G2, G5): the others' weapon fields at render time and the client's weapon
// inputs from a SharedLevel. See client_wiring.h.

#include "bf3/host/client_wiring.h"

#include "bf3/host/gunplay.h"
#include "bf3/net/other_sample.h"

#include <algorithm>
#include <array>

namespace bf3::host {

namespace {

constexpr double kMsPerTick = 1000.0 / double(sim::kSimHz); // as client_session.cpp

PresentedOtherWeapon weapon_of_track(const OtherTrack &p_track, double p_render_ticks, double p_max_extra_ticks) {
	PresentedOtherWeapon out;
	out.slot = p_track.slot;
	const size_t used = std::min<size_t>(p_track.count, kOtherSamples);
	if (used == 0) {
		return out;
	}
	std::array<net::TimedOther, kOtherSamples> window;
	for (size_t k = 0; k < used; ++k) {
		window[k] = p_track.samples[k];
	}
	const std::span<const net::TimedOther> samples(window.data(), used);
	const net::SampleSelection sel = net::select_sample(samples, p_render_ticks, p_max_extra_ticks);
	if (!sel.valid) {
		return out;
	}
	// sample_other's discrete-field rule (other_sample.cpp): an interpolation switches at u >= 1 (u as a float, as
	// there); before the oldest sample, a hold, an extrapolation: the selected sample a.
	const net::TimedOther *src = &samples[sel.index_a];
	if (p_render_ticks > double(src->server_tick) && sel.kind == net::SampleKind::Interpolate) {
		const net::TimedOther &b = samples[sel.index_b];
		const double span = double(b.server_tick - src->server_tick);
		const float u = float(std::clamp((p_render_ticks - double(src->server_tick)) / span, 0.0, 1.0));
		if (u >= 1.0f) {
			src = &b;
		}
	}
	const net::OtherSoldier &o = src->soldier;
	out.valid = true;
	out.generation = o.generation;
	out.def = o.weapon == 0xFF ? sim::weapons::kNoWeapon : uint16_t(o.weapon);
	out.phase = sim::weapons::WeaponPhase(o.weapon_phase);
	out.firing = o.firing;
	out.ads = o.ads;
	out.reload_seq = o.reload_seq;
	out.burst_seq = o.burst_seq;
	out.team = o.team;
	out.sample_tick = src->server_tick;
	return out;
}

double max_extra_ticks(const PresentationFrame &p_frame) {
	return std::max(0.0, double(p_frame.extrapolate_max_ms)) / kMsPerTick;
}

} // namespace

PresentedOtherWeapon present_other_weapon(const PresentationFrame &p_frame, uint8_t p_slot, double p_render_ticks) {
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	for (uint32_t i = 0; i < count; ++i) {
		if (p_frame.others[i].slot == p_slot && p_frame.others[i].count > 0) {
			return weapon_of_track(p_frame.others[i], p_render_ticks, max_extra_ticks(p_frame));
		}
	}
	PresentedOtherWeapon none;
	none.slot = p_slot;
	return none;
}

size_t present_other_weapons(const PresentationFrame &p_frame, double p_render_ticks, std::span<PresentedOtherWeapon> r_out) {
	// present_others' loop: tracks in frame order, empty tracks skipped.
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	const double extra = max_extra_ticks(p_frame);
	size_t n = 0;
	for (uint32_t i = 0; i < count && n < r_out.size(); ++i) {
		const OtherTrack &t = p_frame.others[i];
		if (std::min<size_t>(t.count, kOtherSamples) == 0) {
			continue;
		}
		r_out[n++] = weapon_of_track(t, p_render_ticks, extra);
	}
	return n;
}

core::Result<std::shared_ptr<const ClientGunplayInputs>> make_client_gunplay_inputs(const SharedLevel &p_level) {
	if (p_level.gunplay == nullptr) {
		return core::make_error("client gunplay inputs: " + p_level.level + " was loaded without gunplay tables (set SharedLoadOptions::gunplay)");
	}
	core::Result<std::shared_ptr<const ClientGunplayInputs>> in = client_gunplay_inputs(p_level);
	if (!in) {
		return core::error_at("make_client_gunplay_inputs", in.error());
	}
	return *in;
}

} // namespace bf3::host
