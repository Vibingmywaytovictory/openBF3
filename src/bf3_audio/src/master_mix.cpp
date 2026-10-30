#include "bf3/audio/master_mix.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;

namespace {

constexpr uint32_t kMaxFrames = 512;
constexpr float kOpenLowPassShare = 0.45f;

float smoothing(float p_ms, uint32_t p_rate) {
	const double frames = std::max(1.0, double(std::max(0.0f, p_ms)) * double(p_rate) / 1000.0);
	return float(1.0 - std::exp(-1.0 / frames));
}

}

void MasterMixer::configure(std::shared_ptr<const ca::MasterMixData> p_data, uint32_t p_rate, float p_iir2_q) {
	data_ = std::move(p_data);
	rate_ = std::max<uint32_t>(p_rate, 1);
	iir2_q_ = p_iir2_q;
	plan_ = nullptr;
	if (data_) {
		(void)select(data_->default_stereo_mode, data_->default_preset);
	}
}

bool MasterMixer::select(uint32_t p_stereo_mode, uint32_t p_preset) {
	const ca::MasterMixPlan *next = data_ ? data_->plan(p_stereo_mode, p_preset) : nullptr;
	if (next == nullptr) {
		return false;
	}
	if (plan_ == nullptr || plan_->voices.size() != next->voices.size()) {
		buffers_.assign(next->voices.size(), RoleBuffer{});
		live_.assign(next->voices.size(), {});
		states_.assign(next->voices.size(), {});
		for (size_t v = 0; v < next->voices.size(); ++v) {
			states_[v].assign(next->voices[v].steps.size(), StepState{});
		}
	}
	plan_ = next;
	ceiling_ = 0.0f;
	for (const ca::MasterVoice &v : plan_->voices) {
		for (const ca::MasterStep &s : v.steps) {
			ceiling_ = s.kind == ca::MasterStepKind::Clip ? std::max(ceiling_, s.values[0]) : ceiling_;
		}
	}
	return true;
}

void MasterMixer::process(
		const BusBuffers &p_buses, const float *p_bus_gain, float *r_interleaved_stereo, uint32_t p_frames) {
	const uint32_t frames = std::min(p_frames, kMaxFrames);
	if (plan_ == nullptr) {
		return;
	}
	for (size_t v = 0; v < buffers_.size(); ++v) {
		for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
			if (live_[v][r]) {
				std::fill_n(buffers_[v][r].data(), frames, 0.0f);
				live_[v][r] = 0;
			}
		}
	}
	for (size_t v = 0; v < plan_->voices.size(); ++v) {
		const ca::MasterBus bus = plan_->voices[v].bus;
		if (bus == ca::MasterBus::Unknown || size_t(bus) >= size_t(ca::MasterBus::Unknown)) {
			continue;
		}
		const float *l = p_buses.channels[size_t(bus)][0];
		const float *r = p_buses.channels[size_t(bus)][1];
		const float g = p_bus_gain[size_t(bus)];
		RoleBuffer &b = buffers_[v];
		const bool mono = plan_->voices[v].channels == 1;
		if (mono) {
			float *m = b[size_t(ca::SpeakerRole::Mono)].data();
			for (uint32_t i = 0; i < frames; ++i) {
				m[i] = 0.5f * (l[i] + r[i]) * g;
			}
			live_[v][size_t(ca::SpeakerRole::Mono)] = 1;
		} else {
			float *bl = b[size_t(ca::SpeakerRole::L)].data();
			float *br = b[size_t(ca::SpeakerRole::R)].data();
			for (uint32_t i = 0; i < frames; ++i) {
				bl[i] = l[i] * g;
				br[i] = r[i] * g;
			}
			live_[v][size_t(ca::SpeakerRole::L)] = 1;
			live_[v][size_t(ca::SpeakerRole::R)] = 1;
		}
	}
	for (const uint16_t v : plan_->order) {
		const ca::MasterVoice &mv = plan_->voices[v];
		RoleBuffer &b = buffers_[v];
		std::array<uint8_t, ca::kSpeakerRoles> &live = live_[v];
		for (size_t k = 0; k < mv.steps.size(); ++k) {
			const ca::MasterStep &s = mv.steps[k];
			StepState &st = states_[v][k];
			switch (s.kind) {
				case ca::MasterStepKind::Pass:
					break;
				case ca::MasterStepKind::Gain: {
					const float to = std::isfinite(s.values[0]) ? std::max(0.0f, s.values[0]) : 0.0f;
					const float from = st.gain_prev < 0.0f ? to : st.gain_prev;
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (live[r]) {
							apply_gain_ramp(b[r].data(), frames, from, to);
						}
					}
					st.gain_prev = to;
					break;
				}
				case ca::MasterStepKind::HighPass:
				case ca::MasterStepKind::LowPass:
				case ca::MasterStepKind::HighShelf:
				case ca::MasterStepKind::LowShelf:
				case ca::MasterStepKind::Peaking: {
					BiquadCoefficients c;
					const float f = s.values[0];
					switch (s.kind) {
						case ca::MasterStepKind::HighPass:
							c = BiquadCoefficients::design(BiquadShape::HighPass, float(rate_), f, iir2_q_, 0.0f);
							break;
						case ca::MasterStepKind::LowPass:
							c = f >= kOpenLowPassShare * float(rate_)
									? BiquadCoefficients{}
									: BiquadCoefficients::design(BiquadShape::LowPass, float(rate_), f, iir2_q_, 0.0f);
							break;
						case ca::MasterStepKind::HighShelf:
							c = BiquadCoefficients::design(BiquadShape::HighShelf, float(rate_), f, iir2_q_,
									gain_to_db(std::max(0.0f, s.values[1])));
							break;
						case ca::MasterStepKind::LowShelf:
							c = BiquadCoefficients::design(BiquadShape::LowShelf, float(rate_), f, iir2_q_,
									gain_to_db(std::max(0.0f, s.values[1])));
							break;
						default:
							c = BiquadCoefficients::design(BiquadShape::Peaking, float(rate_), f, s.values[2],
									gain_to_db(std::max(0.0f, s.values[1])));
							break;
					}
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (live[r]) {
							st.biquads[r].process(b[r].data(), frames, c);
						}
					}
					break;
				}
				case ca::MasterStepKind::Compressor: {
					const float threshold = s.values[0];
					const float slope = 1.0f - 1.0f / std::max(1.0f, s.values[1]);
					const float attack = smoothing(s.values[2], rate_), release = smoothing(s.values[3], rate_);
					const float threshold_gain = std::pow(10.0f, threshold / 20.0f);
					float gr = st.compressor_db;
					for (uint32_t i = 0; i < frames; ++i) {
						float peak = 0.0f;
						for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
							peak = live[r] ? std::max(peak, std::fabs(b[r][i])) : peak;
						}
						const float target =
								peak > threshold_gain ? (threshold - 20.0f * std::log10(peak)) * slope : 0.0f;
						gr += (target - gr) * (target < gr ? attack : release);
						gr = target == 0.0f && gr > -1.0e-5f ? 0.0f : gr;
						if (gr == 0.0f) {
							continue;
						}
						const float g = std::pow(10.0f, gr / 20.0f);
						for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
							if (live[r]) {
								b[r][i] *= g;
							}
						}
						stats_.compressor_min_db = std::min(stats_.compressor_min_db, gr);
					}
					st.compressor_db = gr;
					break;
				}
				case ca::MasterStepKind::Clip: {
					const float level = std::max(0.0f, s.values[0]);
					uint64_t clipped = 0;
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (!live[r]) {
							continue;
						}
						for (uint32_t i = 0; i < frames; ++i) {
							const float x = b[r][i];
							clipped += std::fabs(x) > level ? 1u : 0u;
							b[r][i] = std::clamp(x, -level, level);
						}
					}
					(s.values[0] >= ceiling_ ? stats_.ceiling_clipped_samples : stats_.other_clipped_samples) +=
							clipped;
					break;
				}
				case ca::MasterStepKind::Send:
				case ca::MasterStepKind::Route: {
					if (s.to_voice >= buffers_.size()) {
						break;
					}
					const float amp = std::isfinite(s.values[0]) ? s.values[0] : 0.0f;
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (!live[r] || s.map[r] < 0 || amp == 0.0f) {
							continue;
						}
						float *dst = buffers_[s.to_voice][size_t(s.map[r])].data();
						const float *src = b[r].data();
						for (uint32_t i = 0; i < frames; ++i) {
							dst[i] += src[i] * amp;
						}
						live_[s.to_voice][size_t(s.map[r])] = 1;
					}
					break;
				}
				case ca::MasterStepKind::MatrixPan: {
					std::array<float, kMaxFrames> m{};
					uint32_t inputs = 0;
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (live[r]) {
							for (uint32_t i = 0; i < frames; ++i) {
								m[i] += b[r][i];
							}
							++inputs;
							std::fill_n(b[r].data(), frames, 0.0f);
							live[r] = 0;
						}
					}
					if (inputs > 1) {
						for (uint32_t i = 0; i < frames; ++i) {
							m[i] /= float(inputs);
						}
					}
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						const float g = s.map[r] >= 0 ? s.values[size_t(s.map[r])] : 0.0f;
						if (inputs == 0 || !(g != 0.0f) || !std::isfinite(g)) {
							continue;
						}
						for (uint32_t i = 0; i < frames; ++i) {
							b[r][i] = m[i] * g;
						}
						live[r] = 1;
					}
					break;
				}
				case ca::MasterStepKind::Silence:
					for (size_t r = 0; r < ca::kSpeakerRoles; ++r) {
						if (live[r]) {
							std::fill_n(b[r].data(), frames, 0.0f);
							live[r] = 0;
						}
					}
					break;
			}
		}
	}
	const RoleBuffer &out = buffers_[plan_->output_voice];
	const std::array<uint8_t, ca::kSpeakerRoles> &live = live_[plan_->output_voice];
	const auto role = [&](ca::SpeakerRole p_role, uint32_t p_i) {
		return live[size_t(p_role)] ? out[size_t(p_role)][p_i] : 0.0f;
	};
	const bool stereo = plan_->speakers <= 2;
	for (uint32_t i = 0; i < frames; ++i) {
		float l = role(ca::SpeakerRole::L, i), r = role(ca::SpeakerRole::R, i);
		if (!stereo) {
			const float c = role(ca::SpeakerRole::C, i);
			l += kStereoDownmix * (c + role(ca::SpeakerRole::Ls, i));
			r += kStereoDownmix * (c + role(ca::SpeakerRole::Rs, i));
		}
		r_interleaved_stereo[size_t(i) * 2] = l;
		r_interleaved_stereo[size_t(i) * 2 + 1] = r;
	}
}

}
