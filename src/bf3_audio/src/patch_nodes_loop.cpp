#include "patch_state.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace bf3::audio {

using namespace detail;
using ca::SoundNode;

namespace {

constexpr uint32_t kLoopDrawNode = 0x40000000u;
constexpr float kMaxCrossFadeSeconds = 10.0f;
constexpr std::string_view kSequentialSelection = "SwvsSequential";

double piece_seconds(const ca::WaveVariation &p_variation) {
	double seconds = 0.0;
	for (const ca::WaveSegment &seg : p_variation.segments) {
		seconds += std::isfinite(seg.length_seconds) ? std::max(0.0f, seg.length_seconds) : 0.0f;
	}
	return seconds;
}

}

bool PatchInstance::State::start_loop_piece(
		LoopPlayerState &r_loop, uint64_t p_frame, float p_offset_seconds, uint32_t p_fade_frames) {
	const ca::WaveId id = wave_ids[r_loop.wave_index];
	const uint32_t variation = wave_variations[r_loop.wave_index].first + r_loop.piece;
	const ca::WaveVariation &var = bank->waves()[id].variations[variation];
	const uint32_t next = r_loop.current == UINT32_MAX ? 0u : (r_loop.current + 1) % r_loop.samplers_count;
	SamplerState &s = samplers[r_loop.samplers_begin + next];
	s.wave = id;
	s.variation = variation;
	s.segments = uint16_t(std::min<size_t>(var.segments.size(), 0xfffe));
	s.first_loop = s.last_loop = -1;
	s.released = false;
	s.stepped = false;
	if (s.segments == 0) {
		++stats.starved_slots;
		return false;
	}
	PendingStart &pend = slots[s.slots_begin].pending;
	pend.armed = true;
	pend.frame = p_frame;
	pend.segment = 0;
	pend.kind = r_loop.current == UINT32_MAX ? SamplerTraceKind::Trigger : SamplerTraceKind::Advance;
	pend.offset_seconds = p_offset_seconds;
	pend.fade_in_frames = p_fade_frames;
	s.last_slot = 0;
	s.last_segment = 0;
	s.active = true;
	r_loop.current = next;
	const double heard = std::max(0.0, piece_seconds(var) - double(p_offset_seconds)) / double(s.pitch);
	r_loop.next_frame = p_frame + uint64_t(std::llround(heard * double(mix_rate)));
	return true;
}

void PatchInstance::State::loop_player_events(uint32_t p_node_index, const SoundNode &n, uint64_t p_frame) {
	LoopPlayerState &l = loops[nodes[p_node_index].index];
	if (l.samplers_count == 0) {
		return;
	}
	const float amp = read(port(n, cp::DlpAmplitude));
	const float amplitude = std::isfinite(amp) ? std::clamp(amp, 0.0f, 16.0f) : 0.0f;
	const float pitch = std::clamp(program->master_pitch, 1.0f / 64.0f, 64.0f);
	for (uint32_t k = 0; k < l.samplers_count; ++k) {
		samplers[l.samplers_begin + k].amplitude = amplitude;
		samplers[l.samplers_begin + k].pitch = pitch;
	}
	const float crossfade_seconds = std::clamp(flt(n, cp::DlpCrossFadeLength), 0.0f, kMaxCrossFadeSeconds);
	const uint32_t crossfade = uint32_t(std::llround(double(crossfade_seconds) * double(mix_rate)));

	if (rose(port(n, cp::DlpStart)) && !l.running) {
		++stats.sampler_triggers;
		uint32_t wave_index = UINT32_MAX;
		const ca::SoundPort &external = port(n, cp::DlpExternalWave);
		if (external.connected()) {
			const double h = std::floor(double(read(external)) + 0.5);
			if (h >= 1.0 && h <= double(program->waves.size())) {
				wave_index = uint32_t(h) - 1;
			}
		} else if (n.waves_count > 0 && program->wave_refs[n.waves_begin] != ca::kNoRegister) {
			wave_index = program->wave_refs[n.waves_begin];
		}
		if (wave_index == UINT32_MAX || wave_ids[wave_index] == ca::kNoWave ||
				wave_variations[wave_index].second == 0) {
			++stats.starved_slots;
		} else {
			const uint32_t pieces = wave_variations[wave_index].second;
			l.wave_index = wave_index;
			l.current = UINT32_MAX;
			l.piece = 0;
			float offset = 0.0f;
			if (n.flags & ca::kFlagStartAtRandomPosition) {
				l.piece = std::min(pieces - 1,
						uint32_t(uniform01(seed, p_node_index | kLoopDrawNode, l.draws++) * double(pieces)));
				const ca::WaveVariation &var =
						bank->waves()[wave_ids[wave_index]].variations[wave_variations[wave_index].first + l.piece];
				const double room = piece_seconds(var) - 2.0 * double(crossfade_seconds);
				offset = room > 0.0 ? float(uniform01(seed, p_node_index | kLoopDrawNode, l.draws++) * room) : 0.0f;
			}
			l.running = start_loop_piece(l, p_frame, offset, 0);
		}
	}

	if (on(read(port(n, cp::DlpStop))) && l.running) {
		l.running = false;
		const float frames = float(std::max<uint32_t>(crossfade, 1));
		for (uint32_t k = 0; k < l.samplers_count; ++k) {
			SamplerState &s = samplers[l.samplers_begin + k];
			SlotState &slot = slots[s.slots_begin];
			slot.pending.armed = false;
			if (slot.play.active) {
				slot.fade_step = -std::max(slot.fade, 1.0f / frames) / frames;
				slot.fade_kill = true;
			} else if (!sampler_busy(s)) {
				s.active = false;
			}
		}
	}

	if (!l.running || l.current == UINT32_MAX) {
		return;
	}
	const SamplerState &playing = samplers[l.samplers_begin + l.current];
	const uint32_t piece_frames = uint32_t(std::min<double>(
			piece_seconds(bank->waves()[playing.wave].variations[playing.variation]) * double(mix_rate) / double(pitch),
			double(UINT32_MAX)));
	const uint32_t fade = std::min(crossfade, piece_frames / 2);
	if (p_frame + kMaxSubBlock + fade < l.next_frame) {
		return;
	}
	const uint32_t pieces = wave_variations[l.wave_index].second;
	if (!on(read(port(n, cp::DlpFreezeSegment))) && pieces > 1) {
		if (bank->waves()[playing.wave].selection == kSequentialSelection) {
			l.piece = (l.piece + 1) % pieces;
		} else {
			l.piece = (l.piece + 1 +
							  uint32_t(uniform01(seed, p_node_index | kLoopDrawNode, l.draws++) * double(pieces - 1))) %
					pieces;
		}
	}
	const uint32_t outgoing = l.current;
	const uint64_t at = fade > 0 ? p_frame : std::max(p_frame, l.next_frame);
	if (!start_loop_piece(l, at, 0.0f, fade)) {
		l.running = false;
		return;
	}
	if (fade > 0 && l.current != outgoing) {
		SlotState &old = slots[samplers[l.samplers_begin + outgoing].slots_begin];
		if (old.play.active) {
			old.fade_step = -std::max(old.fade, 1.0f / float(fade)) / float(fade);
			old.fade_kill = true;
		}
	}
}

}
