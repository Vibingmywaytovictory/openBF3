// The Sampler model of a PatchInstance (patch_instance.h "Sampler model", design §6.3; [inferred], open question 4):
// Trigger / Step / Release handling at control rate, and the per-slot playback (StreamDecoder -> Catmull-Rom
// resampler) at audio rate. Audio thread: no allocation; the decoders read the WaveBank's resident chunk bytes.

#include "patch_state.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

using namespace detail;
using ca::SoundNode;

namespace {

bool on(float p_v) {
	return p_v >= 0.5f;
}

float catmull_rom(float p0, float p1, float p2, float p3, float t) {
	return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}

struct NextSegment {
	bool play = false;
	uint16_t segment = 0;
	SamplerTraceKind kind = SamplerTraceKind::Advance;
};

NextSegment tail_or_end(const SamplerState &s) {
	if (s.last_loop >= 0 && s.last_loop + 1 < int32_t(s.segments)) {
		return { true, uint16_t(s.last_loop + 1), SamplerTraceKind::Tail };
	}
	return {};
}

} // namespace

void PatchInstance::State::trace_entry(uint64_t p_frame, uint32_t p_node, uint8_t p_slot, uint16_t p_segment, SamplerTraceKind p_kind, float p_pitch,
		uint64_t p_prev_end) {
	if (trace == nullptr) {
		return;
	}
	if (trace->count >= trace->entries.size()) {
		++trace->dropped;
		return;
	}
	SamplerTraceEntry &e = trace->entries[trace->count++];
	e.frame = p_frame;
	e.node = p_node;
	e.slot = p_slot;
	e.segment = p_segment;
	e.kind = p_kind;
	e.pitch = p_pitch;
	e.prev_end = p_prev_end;
	const SamplerState &s = samplers[nodes[p_node].index];
	e.wave = s.wave;
	e.variation = s.variation;
}

bool PatchInstance::State::sampler_busy(const SamplerState &p_sampler) const {
	for (uint32_t k = 0; k < p_sampler.slots_count; ++k) {
		const SlotState &slot = slots[p_sampler.slots_begin + k];
		if (slot.play.active || slot.pending.armed) {
			return true;
		}
	}
	return false;
}

namespace {

// What follows segment p_segment on slot p_slot when it ends.
NextSegment next_segment(const SamplerState &s, uint32_t p_slot, uint16_t p_segment) {
	const int32_t seg = p_segment, fl = s.first_loop, ll = s.last_loop, count = s.segments;
	if (s.loop == ca::SamplerLoop::None || fl < 0) {
		if (s.released && s.loop == ca::SamplerLoop::None) {
			return {}; // LtNone: a Release ends the sampler with its segment
		}
		if (seg + 1 < count) {
			return { true, uint16_t(seg + 1), SamplerTraceKind::Advance };
		}
		return {};
	}
	if (seg > ll) { // in the tail
		if (seg + 1 < count) {
			return { true, uint16_t(seg + 1), SamplerTraceKind::Tail };
		}
		return {};
	}
	if (s.stepped) {
		// Steps drive the rhythm: a slot whose segment ends goes idle; the tail follows the latest segment.
		if (s.released && p_slot == s.last_slot) {
			return tail_or_end(s);
		}
		return {};
	}
	if (s.released) {
		if (s.loop == ca::SamplerLoop::Forward && seg < ll) {
			return { true, uint16_t(seg + 1), SamplerTraceKind::Advance }; // LtForward finishes its pass
		}
		return tail_or_end(s);
	}
	if (seg < fl) {
		return { true, uint16_t(seg + 1), SamplerTraceKind::Advance }; // the intro
	}
	return { true, kNoSegment, SamplerTraceKind::Advance }; // the next loop segment (chosen by the caller)
}

uint16_t pick_loop_segment(PatchInstance::State &st, SamplerState &s) {
	const int32_t fl = s.first_loop, ll = s.last_loop;
	const int32_t size = ll - fl + 1;
	const int32_t last = s.last_segment;
	if (size <= 1) {
		return uint16_t(fl);
	}
	const bool in_loop = last >= fl && last <= ll;
	if (s.shuffle) {
		if (!in_loop) {
			return uint16_t(fl + int32_t(uniform01(st.seed, s.node | 0x80000000u, s.draws++) * double(size)) % size);
		}
		int32_t r = int32_t(uniform01(st.seed, s.node | 0x80000000u, s.draws++) * double(size - 1)) % (size - 1);
		if (fl + r >= last) {
			++r;
		}
		return uint16_t(fl + r);
	}
	if (!in_loop || last >= ll) {
		return uint16_t(fl);
	}
	return uint16_t(last + 1);
}

} // namespace

void PatchInstance::State::sampler_events(uint32_t p_node_index, const SoundNode &n, uint64_t p_frame) {
	SamplerState &s = samplers[nodes[p_node_index].index];
	const float pitch = s.pitch = [&] {
		const double v = double(flt(n, cp::SmpBasePitch)) * read(port(n, cp::SmpPitch)) * program->master_pitch;
		return std::isfinite(v) && v > 0.0 ? float(std::clamp(v, 1.0 / 64.0, 64.0)) : 1.0f;
	}();
	const float amp = read(port(n, cp::SmpAmplitude));
	s.amplitude = std::isfinite(amp) ? std::clamp(amp, 0.0f, 16.0f) : 0.0f;
	const bool trigger = on(read(port(n, cp::SmpTrigger)));
	const bool step = on(read(port(n, cp::SmpStep)));
	const bool release = on(read(port(n, cp::SmpRelease)));
	const auto delay_frames = [&](float p_seconds) { return uint64_t(std::llround(double(p_seconds) * double(mix_rate))); };

	if (trigger && s.slots_count > 0) {
		++stats.sampler_triggers;
		uint32_t wave_index = UINT32_MAX;
		const ca::SoundPort &external = port(n, cp::SmpExternalWave);
		if (external.connected()) {
			const double h = std::floor(double(read(external)) + 0.5);
			if (h >= 1.0 && h <= double(program->waves.size())) {
				wave_index = uint32_t(h) - 1;
			}
		} else if (n.waves_count > 0 && program->wave_refs[n.waves_begin] != ca::kNoRegister) {
			wave_index = program->wave_refs[n.waves_begin];
		}
		const ca::WaveId id = wave_index == UINT32_MAX ? ca::kNoWave : wave_ids[wave_index];
		const std::pair<uint32_t, uint32_t> range = wave_index == UINT32_MAX ? std::pair<uint32_t, uint32_t>{ 0, 0 } : wave_variations[wave_index];
		if (id == ca::kNoWave || range.second == 0) {
			++stats.starved_slots;
		} else {
			const ca::SoundWaveInfo &wave = bank->waves()[id];
			const ca::SoundPort &variation_port = port(n, cp::SmpVariation);
			uint32_t v = 0;
			if (variation_port.connected()) {
				const float f = std::floor(read(variation_port));
				v = f <= 0.0f ? 0u : std::min(uint32_t(f), range.second - 1);
			} else if (range.second > 1) {
				v = std::min(range.second - 1, uint32_t(uniform01(seed, s.node, s.draws++) * double(range.second)));
			}
			const ca::WaveVariation &var = wave.variations[range.first + v];
			s.wave = id;
			s.variation = range.first + v;
			s.segments = uint16_t(std::min<size_t>(var.segments.size(), 0xfffe));
			s.first_loop = var.first_loop_segment;
			s.last_loop = var.last_loop_segment;
			if (s.first_loop < 0 || s.last_loop < s.first_loop || s.last_loop >= int32_t(s.segments)) {
				s.first_loop = s.last_loop = -1;
			}
			s.delay_seconds = std::clamp(read(port(n, cp::SmpDelay)), 0.0f, 10.0f);
			s.stepped = s.step_connected && on(read(port(n, cp::SmpEnableStep)));
			s.released = false;
			s.instant_release = false;
			if (s.segments > 0) {
				uint32_t slot = (s.last_slot + 1) % s.slots_count;
				for (uint32_t k = 0; k < s.slots_count; ++k) {
					const uint32_t c = (s.last_slot + 1 + k) % s.slots_count;
					if (!slots[s.slots_begin + c].play.active && !slots[s.slots_begin + c].pending.armed) {
						slot = c;
						break;
					}
				}
				PendingStart &pend = slots[s.slots_begin + slot].pending;
				pend.armed = true;
				pend.frame = p_frame + delay_frames(s.delay_seconds);
				pend.segment = 0;
				pend.kind = SamplerTraceKind::Trigger;
				pend.offset_seconds = std::clamp(read(port(n, cp::SmpOffset)), 0.0f, 3600.0f);
				s.last_slot = uint8_t(slot);
				s.last_segment = 0;
				s.active = true;
			}
		}
	}
	if (step && s.active && s.stepped && !s.released && s.first_loop >= 0) {
		const uint16_t seg = pick_loop_segment(*this, s);
		const uint32_t slot = (s.last_slot + 1) % s.slots_count;
		PendingStart &pend = slots[s.slots_begin + slot].pending;
		pend.armed = true;
		pend.frame = p_frame + delay_frames(s.delay_seconds);
		pend.segment = seg;
		pend.kind = SamplerTraceKind::Step;
		pend.offset_seconds = 0.0f;
		s.last_slot = uint8_t(slot);
		s.last_segment = seg;
	}
	if (release && s.active && !s.released) {
		s.released = true;
		if (s.loop == ca::SamplerLoop::InstantRelease) {
			s.instant_release = true;
			const float frames = std::max(1.0f, tuning.release_fade_ms * 0.001f * float(mix_rate));
			for (uint32_t k = 0; k < s.slots_count; ++k) {
				SlotState &slot = slots[s.slots_begin + k];
				slot.pending.armed = false;
				if (slot.play.active) {
					slot.fade_step = -1.0f / frames;
					slot.fade_kill = true;
				}
			}
			if (!sampler_busy(s)) {
				s.active = false;
				s.finished_pulse = true;
			}
		} else if (!sampler_busy(s)) {
			const NextSegment tail = s.loop == ca::SamplerLoop::None ? NextSegment{} : tail_or_end(s);
			if (tail.play) {
				PendingStart &pend = slots[s.slots_begin + s.last_slot].pending;
				pend.armed = true;
				pend.frame = p_frame + delay_frames(s.delay_seconds);
				pend.segment = tail.segment;
				pend.kind = SamplerTraceKind::Tail;
				pend.offset_seconds = 0.0f;
				s.last_segment = tail.segment;
			} else {
				s.active = false;
				s.finished_pulse = true;
			}
		}
	}
	write(port(n, cp::SmpFinished), s.finished_pulse ? 1.0f : 0.0f);
	s.finished_pulse = false;
	write(port(n, cp::SmpBuffered), 1.0f);
	const SlotState &latest = slots[s.slots_begin + std::min<uint32_t>(s.last_slot, s.slots_count ? s.slots_count - 1 : 0)];
	write(port(n, cp::SmpPosition), s.slots_count && latest.play.active ? float(latest.play.frames_out) / float(mix_rate) : 0.0f);
	(void)pitch;
}

namespace {

// Opens p_segment of the sampler's current variation on p_play; false (counted by the caller) when the chunk is not
// resident or the stream does not open.
bool open_segment(const PatchInstance::State &st, const SamplerState &s, SlotPlay &r_play, uint16_t p_segment) {
	const ca::SoundWaveInfo &wave = st.bank->waves()[s.wave];
	const ca::WaveVariation &var = wave.variations[s.variation];
	if (p_segment >= var.segments.size()) {
		return false;
	}
	const std::span<const uint8_t> chunk = st.bank->chunk(s.wave, var.chunk_index);
	if (chunk.empty()) {
		return false;
	}
	if (!r_play.decoder.open(chunk, var.segments[p_segment].samples_offset)) {
		return false;
	}
	r_play.wave = s.wave;
	r_play.variation = s.variation;
	r_play.segment = p_segment;
	r_play.channels = r_play.decoder.header().channels;
	r_play.rate = r_play.decoder.header().sample_rate;
	r_play.input_count = r_play.input_pos = 0;
	r_play.source_done = false;
	r_play.flush = 0;
	r_play.frames_out = 0;
	return true;
}

} // namespace

void PatchInstance::State::render_slot(SamplerState &s, uint32_t p_slot, float *r_left, float *r_right, uint64_t p_first_frame, uint32_t p_frames) {
	SlotState &slot = slots[s.slots_begin + p_slot];
	SlotPlay &play = slot.play;
	const uint32_t node = s.node;

	// The slot ends: idle, and the sampler ends with its last busy slot.
	const auto end_slot = [&](uint64_t p_frame) {
		play.active = false;
		trace_entry(p_frame, node, uint8_t(p_slot), play.segment, SamplerTraceKind::End, s.pitch, 0);
		if (!sampler_busy(s)) {
			s.active = false;
			s.finished_pulse = true;
		}
	};
	// One input frame into history[3]; handles segment ends (continuation on this slot, or the flush to idle).
	const auto pull = [&](uint64_t p_frame) {
		float l = 0.0f, r = 0.0f;
		for (;;) {
			if (play.source_done) {
				++play.flush;
				break;
			}
			if (play.input_pos < play.input_count) {
				const float *x = play.input.data() + size_t(play.input_pos) * play.channels;
				++play.input_pos;
				if (play.channels == 1) {
					l = r = x[0];
				} else {
					l = x[0];
					r = x[1];
					for (uint32_t c = 2; c < play.channels; ++c) {
						(c & 1 ? r : l) += 0.5f * x[c];
					}
				}
				break;
			}
			core::Result<uint32_t> got = play.decoder.read(play.input.data(), kSlotInputFrames);
			if (!got) {
				++stats.decode_errors;
				play.source_done = true;
				continue;
			}
			if (*got > 0) {
				play.input_count = *got;
				play.input_pos = 0;
				continue;
			}
			// The segment's stream is exhausted.
			NextSegment next = next_segment(s, p_slot, play.segment);
			if (next.play && next.segment == kNoSegment) {
				next.segment = pick_loop_segment(*this, s);
			}
			if (next.play) {
				// The new segment's first sample enters history[3], two input positions ahead of history[1] where
				// the next output frame (p_frame) reads at play.frac: it is heard ceil((2 - frac) / ratio) output
				// frames later (2 at ratio 1), the frame the trace reports as its start.
				const double ratio_now = std::max(1e-9, double(play.rate) / double(mix_rate) * double(s.pitch));
				const uint64_t heard = p_frame + uint64_t(std::max(0.0, std::ceil((2.0 - play.frac) / ratio_now - 1e-9)));
				if (open_segment(*this, s, play, next.segment)) {
					s.last_segment = next.segment;
					trace_entry(heard, node, uint8_t(p_slot), next.segment, next.kind, s.pitch, heard);
					continue;
				}
				++stats.decode_errors;
			}
			play.source_done = true;
		}
		play.history[0][0] = play.history[1][0];
		play.history[0][1] = play.history[1][1];
		play.history[1][0] = play.history[2][0];
		play.history[1][1] = play.history[2][1];
		play.history[2][0] = play.history[3][0];
		play.history[2][1] = play.history[3][1];
		play.history[3][0] = l;
		play.history[3][1] = r;
	};
	const auto start_pending = [&](uint64_t p_frame) {
		PendingStart pend = slot.pending;
		slot.pending.armed = false;
		const bool takeover = play.active;
		play.active = false;
		if (!open_segment(*this, s, play, pend.segment)) {
			++stats.starved_slots;
			if (!sampler_busy(s)) {
				s.active = false;
				s.finished_pulse = true;
			}
			return;
		}
		if (pend.offset_seconds > 0.0f) {
			const uint64_t skip = uint64_t(double(pend.offset_seconds) * double(play.rate));
			if (skip >= play.decoder.header().total_frames || !play.decoder.skip_to(uint32_t(skip))) {
				play.source_done = true;
			}
		}
		play.active = true;
		play.frac = 0.0;
		for (auto &h : play.history) {
			h[0] = h[1] = 0.0f;
		}
		for (int i = 0; i < 3; ++i) {
			pull(p_frame);
		}
		if (takeover && tuning.step_crossfade_ms > 0.0f) {
			slot.fade = 0.0f;
			slot.fade_step = 1.0f / std::max(1.0f, tuning.step_crossfade_ms * 0.001f * float(mix_rate));
			slot.fade_kill = false;
		} else {
			slot.fade = 1.0f;
			slot.fade_step = 0.0f;
			slot.fade_kill = false;
		}
		trace_entry(p_frame, node, uint8_t(p_slot), pend.segment, pend.kind, s.pitch, 0);
	};

	uint32_t i = 0;
	while (i < p_frames) {
		const uint64_t f = p_first_frame + i;
		if (slot.pending.armed && slot.pending.frame <= f) {
			start_pending(f);
			continue;
		}
		uint32_t limit = p_frames;
		if (slot.pending.armed && slot.pending.frame < p_first_frame + p_frames) {
			limit = uint32_t(slot.pending.frame - p_first_frame);
		}
		if (!play.active) {
			i = limit; // silence until the pending start (buffers are zeroed per sub-block)
			continue;
		}
		const double ratio = double(play.rate) / double(mix_rate) * double(s.pitch);
		for (; i < limit; ++i) {
			const float t = float(play.frac);
			float l = catmull_rom(play.history[0][0], play.history[1][0], play.history[2][0], play.history[3][0], t);
			float r = catmull_rom(play.history[0][1], play.history[1][1], play.history[2][1], play.history[3][1], t);
			if (slot.fade_step != 0.0f || slot.fade != 1.0f) {
				l *= slot.fade;
				r *= slot.fade;
				slot.fade += slot.fade_step;
				if (slot.fade >= 1.0f) {
					slot.fade = 1.0f;
					slot.fade_step = 0.0f;
				} else if (slot.fade <= 0.0f) {
					slot.fade = 0.0f;
					slot.fade_step = 0.0f;
					if (slot.fade_kill) {
						slot.fade_kill = false;
						r_left[i] += l;
						r_right[i] += r;
						++i;
						end_slot(p_first_frame + i);
						break;
					}
				}
			}
			r_left[i] += l;
			r_right[i] += r;
			++play.frames_out;
			play.frac += ratio;
			while (play.frac >= 1.0 && play.active) {
				play.frac -= 1.0;
				pull(p_first_frame + i + 1);
				if (play.flush >= 3) {
					end_slot(p_first_frame + i + 1);
				}
			}
			if (!play.active) {
				++i;
				break;
			}
		}
	}
}

} // namespace bf3::audio
