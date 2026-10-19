// The control program of a PatchInstance (patch_instance.h, design §6.3): one evaluation of every node in the
// program's topological order over the register file, and the time advance of the stateful nodes. Audio thread:
// no allocation, every index was checked by validate_sound_program.

#include "patch_state.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

using namespace detail;
using ca::SoundNode;
using ca::SoundNodeType;

namespace detail {

uint64_t splitmix64(uint64_t p_x) {
	uint64_t z = p_x + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

double uniform01(uint64_t p_seed, uint32_t p_node, uint32_t p_draw) {
	const uint64_t h = splitmix64(splitmix64(p_seed ^ (uint64_t(p_node) << 32)) ^ uint64_t(p_draw));
	return double(h >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace detail

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kCaseTolerance = 0.001f;

bool on(float p_v) {
	return p_v >= 0.5f;
}

float finite_or_zero(double p_v) {
	return std::isfinite(p_v) ? float(p_v) : 0.0f;
}

float simple_transform(uint8_t p_option, float x, float y) {
	const bool degrees = (p_option & cp::kSimpleTransformDegrees) != 0;
	const auto op = ca::SimpleTransformOp(p_option & 0x7f);
	const double to_rad = degrees ? double(kPi) / 180.0 : 1.0;
	const double from_rad = degrees ? 180.0 / double(kPi) : 1.0;
	const double X = x, Y = y;
	switch (op) {
		case ca::SimpleTransformOp::Add:
			return finite_or_zero(X + Y);
		case ca::SimpleTransformOp::Subtract:
			return finite_or_zero(X - Y);
		case ca::SimpleTransformOp::Multiply:
			return finite_or_zero(X * Y);
		case ca::SimpleTransformOp::Divide:
			return Y == 0.0 ? 0.0f : finite_or_zero(X / Y);
		case ca::SimpleTransformOp::Sine:
			return finite_or_zero(std::sin(X * to_rad));
		case ca::SimpleTransformOp::Cosine:
			return finite_or_zero(std::cos(X * to_rad));
		case ca::SimpleTransformOp::Tangent:
			return finite_or_zero(std::tan(X * to_rad));
		case ca::SimpleTransformOp::ArcSine:
			return (X < -1.0 || X > 1.0) ? 0.0f : finite_or_zero(std::asin(X) * from_rad);
		case ca::SimpleTransformOp::ArcCosine:
			return (X < -1.0 || X > 1.0) ? 0.0f : finite_or_zero(std::acos(X) * from_rad);
		case ca::SimpleTransformOp::ArcTangent:
			return finite_or_zero(std::atan(X) * from_rad);
		case ca::SimpleTransformOp::Modulus:
			return Y == 0.0 ? 0.0f : finite_or_zero(std::fmod(X, Y));
		case ca::SimpleTransformOp::Round:
			return finite_or_zero(std::round(X));
		case ca::SimpleTransformOp::Floor:
			return finite_or_zero(std::floor(X));
		case ca::SimpleTransformOp::Ceiling:
			return finite_or_zero(std::ceil(X));
		case ca::SimpleTransformOp::Power:
			if (X < 0.0 && Y != std::floor(Y)) {
				return 0.0f;
			}
			if (X == 0.0 && Y < 0.0) {
				return 0.0f;
			}
			return finite_or_zero(std::pow(X, Y));
		case ca::SimpleTransformOp::SquareRoot:
			return X < 0.0 ? 0.0f : finite_or_zero(std::sqrt(X));
		case ca::SimpleTransformOp::Absolute:
			return finite_or_zero(std::fabs(X));
		case ca::SimpleTransformOp::Exp:
			return finite_or_zero(std::exp(X));
		case ca::SimpleTransformOp::Log:
			return X <= 0.0 ? 0.0f : finite_or_zero(std::log(X));
		case ca::SimpleTransformOp::Log10:
			return X <= 0.0 ? 0.0f : finite_or_zero(std::log10(X));
		case ca::SimpleTransformOp::Unknown:
			return 0.0f;
	}
	return 0.0f;
}

float scale_clamp(float p_in, float p_in_min, float p_in_max, float p_out_min, float p_out_max) {
	if (p_in_min == p_in_max) {
		return p_in < p_in_min ? p_out_min : p_out_max; // a step
	}
	const double t = std::clamp((double(p_in) - p_in_min) / (double(p_in_max) - p_in_min), 0.0, 1.0);
	return finite_or_zero(p_out_min + t * (double(p_out_max) - p_out_min));
}

// Fade shape over a band: t in [0, 1] -> the fading-out group's level.
float fade_out(ca::FaderType p_type, float p_t) {
	const float t = std::clamp(p_t, 0.0f, 1.0f);
	if (p_type == ca::FaderType::Cosine) {
		return 0.5f + 0.5f * std::cos(kPi * t);
	}
	return 1.0f - t;
}

float band_t(float p_control, float p_begin, float p_end) {
	if (p_end <= p_begin) {
		return p_control < p_begin ? 0.0f : 1.0f;
	}
	return (p_control - p_begin) / (p_end - p_begin);
}

bool condition(ca::ConditionOp p_op, float x, float y) {
	switch (p_op) {
		case ca::ConditionOp::And:
			return on(x) && on(y);
		case ca::ConditionOp::Or:
			return on(x) || on(y);
		case ca::ConditionOp::XOr:
			return on(x) != on(y);
		case ca::ConditionOp::Equal:
			return std::fabs(x - y) <= kCaseTolerance;
		case ca::ConditionOp::Less:
			return x < y;
		case ca::ConditionOp::Greater:
			return x > y;
		case ca::ConditionOp::LessOrEqual:
			return x <= y;
		case ca::ConditionOp::GreaterOrEqual:
			return x >= y;
		case ca::ConditionOp::Unknown:
			return false;
	}
	return false;
}

} // namespace

void PatchInstance::State::evaluate_program(uint64_t p_frame) {
	const ca::SoundProgram &p = *program;
	haptics = PatchHaptics{};
	if (stop_register != ca::kNoRegister && on(registers[stop_register])) {
		stop_seen = true;
	}
	for (uint32_t ni = 0; ni < p.nodes.size(); ++ni) {
		const SoundNode &n = p.nodes[ni];
		NodeState &s = nodes[ni];
		switch (n.type) {
			case SoundNodeType::Receive:
				for (uint16_t c = 0; c < n.children_count; ++c) {
					const SoundNode &e = child(n, c);
					write(port(e, cp::ReceiveOut), read(port(e, cp::ReceiveSource)));
				}
				break;
			case SoundNodeType::MixerReceive:
				for (uint16_t c = 0; c < n.children_count; ++c) {
					const SoundNode &e = child(n, c);
					write(port(e, cp::MixerEntryOut), mixer_values[e.aux]);
				}
				break;
			case SoundNodeType::Physics:
				write(port(n, cp::PhysDistance), geometry.distance);
				write(port(n, cp::PhysAzimuth), geometry.azimuth);
				write(port(n, cp::PhysElevation), geometry.elevation);
				write(port(n, cp::PhysVelocity), geometry.velocity);
				write(port(n, cp::PhysRelativeVelocity), geometry.relative_velocity);
				break;
			case SoundNodeType::MultiCrossfader: {
				const bool start = on(read(port(n, cp::McfStart)));
				const bool stop = on(read(port(n, cp::McfStop)));
				const float live = read(port(n, cp::McfControl));
				const bool lock = (n.flags & ca::kFlagLockControlValue) != 0;
				if (lock && start) {
					s.latched = live;
					s.mode = 1;
				}
				const float control = (lock && s.mode) ? s.latched : live;
				// Reading R (design §6.3): group i fades in over band i-1 and out over band i.
				for (uint16_t g = 0; g < n.children_count; ++g) {
					const SoundNode &e = child(n, g);
					const ca::FaderType type = ca::FaderType(e.option);
					float amp = fade_out(type, band_t(control, flt(e, cp::GroupFadeBegin), flt(e, cp::GroupFadeEnd)));
					if (g > 0) {
						const SoundNode &prev = child(n, g - 1);
						const float t = band_t(control, flt(prev, cp::GroupFadeBegin), flt(prev, cp::GroupFadeEnd));
						amp *= 1.0f - fade_out(ca::FaderType(prev.option), t);
					}
					amp *= flt(e, cp::GroupFadeAmplitude);
					GroupState &gs = groups[child_index(n, g)];
					gs.amplitude = finite_or_zero(amp);
					write(port(e, cp::GroupAmplitude), gs.amplitude);
					bool start_out = false, stop_out = false;
					if (start && gs.amplitude > 0.0f) {
						start_out = true;
						gs.started = true;
					}
					if (stop && (gs.amplitude > 0.0f || gs.started)) {
						stop_out = true;
						gs.started = false;
					}
					write(port(e, cp::GroupStart), start_out ? 1.0f : 0.0f);
					write(port(e, cp::GroupStop), stop_out ? 1.0f : 0.0f);
				}
				break;
			}
			case SoundNodeType::Adsr: {
				const float a = read(port(n, cp::AdsrA)), d = read(port(n, cp::AdsrD)), sus = std::clamp(read(port(n, cp::AdsrS)), 0.0f, 1.0f);
				const float r = read(port(n, cp::AdsrR));
				float finished = s.pulse ? 1.0f : 0.0f;
				s.pulse = 0;
				if (on(read(port(n, cp::AdsrTrigger)))) {
					s.mode = uint8_t(AdsrPhase::Attack);
					if (!(a > 0.0f)) {
						s.value = 1.0f;
						s.mode = uint8_t(AdsrPhase::Decay);
					}
				}
				if (s.mode == uint8_t(AdsrPhase::Decay) && !(d > 0.0f)) {
					s.value = sus;
					s.mode = uint8_t(AdsrPhase::Sustain);
				}
				if (on(read(port(n, cp::AdsrRelease))) && s.mode != uint8_t(AdsrPhase::Idle)) {
					if (r > 0.0f) {
						s.mode = uint8_t(AdsrPhase::Release);
						s.release_rate = s.value / r;
					} else {
						s.value = 0.0f;
						s.mode = uint8_t(AdsrPhase::Idle);
						finished = 1.0f;
					}
				}
				write(port(n, cp::AdsrValue), s.value);
				write(port(n, cp::AdsrFinished), finished);
				break;
			}
			case SoundNodeType::Lfo: {
				const float mn = flt(n, cp::LfoMin), mx = flt(n, cp::LfoMax);
				const double mid = 0.5 * (double(mn) + mx), half = 0.5 * (double(mx) - mn);
				const double amp = read(port(n, cp::LfoAmplitude));
				write(port(n, cp::LfoOut), finite_or_zero(mid + amp * half * std::sin(2.0 * double(kPi) * s.phase)));
				break;
			}
			case SoundNodeType::NumberGenerator:
				if (on(read(port(n, cp::NumTrigger)))) {
					const float mn = flt(n, cp::NumMin), mx = flt(n, cp::NumMax);
					s.value = finite_or_zero(mn + (double(mx) - mn) * uniform01(seed, ni, s.draws++));
				}
				write(port(n, cp::NumY), s.value);
				break;
			case SoundNodeType::ParameterFilterLinear: {
				const float in = read(port(n, cp::PflIn));
				if (!s.mode) {
					s.value = in;
					s.mode = 1;
				}
				write(port(n, cp::PflOut), s.value);
				break;
			}
			case SoundNodeType::Timer: {
				float tick = s.pulse ? 1.0f : 0.0f;
				s.pulse = 0;
				if (on(read(port(n, cp::TimerStart)))) {
					s.mode = 1;
					s.value = 0.0f;
				}
				if (on(read(port(n, cp::TimerStop)))) {
					s.mode = 0;
				}
				const float period = read(port(n, cp::TimerPeriod));
				write(port(n, cp::TimerTick), tick);
				write(port(n, cp::TimerProgress), period > 0.0f ? std::clamp(s.value / period, 0.0f, 1.0f) : 0.0f);
				break;
			}
			case SoundNodeType::EventGate: {
				bool out = false;
				if (on(read(port(n, cp::GateIn))) && on(read(port(n, cp::GateEnable))) && s.value <= 0.0f) {
					out = true;
					s.value = std::max(0.0f, read(port(n, cp::GateCoolDown)));
				}
				write(port(n, cp::GateOut), out ? 1.0f : 0.0f);
				break;
			}
			case SoundNodeType::ScaleClamp:
				write(port(n, cp::ScOut), scale_clamp(read(port(n, cp::ScIn)), read(port(n, cp::ScInMin)), read(port(n, cp::ScInMax)), read(port(n, cp::ScOutMin)),
												read(port(n, cp::ScOutMax))));
				break;
			case SoundNodeType::SimpleTransform:
				write(port(n, cp::StZ), simple_transform(n.option, read(port(n, cp::StX)), read(port(n, cp::StY))));
				break;
			case SoundNodeType::Crossfader2: {
				const float c = std::clamp(read(port(n, cp::Cf2Ctrl)), 0.0f, 1.0f);
				write(port(n, cp::Cf2Out1), 1.0f - c);
				write(port(n, cp::Cf2Out2), c);
				break;
			}
			case SoundNodeType::SimpleValueSelector: {
				const uint32_t count = n.floats_count;
				float out = 0.0f;
				if (count > 0) {
					const float idx = std::floor(read(port(n, cp::SvsIndex)));
					const uint32_t i = idx <= 0.0f ? 0u : (idx >= float(count - 1) ? count - 1 : uint32_t(idx));
					out = flt(n, i);
				}
				write(port(n, cp::SvsOut), out);
				break;
			}
			case SoundNodeType::Switcher: {
				const bool trigger = on(read(port(n, cp::SwTrigger)));
				const float value = read(port(n, cp::SwValue));
				int32_t hit = -1;
				if (trigger) {
					for (int pass = 0; pass < 2 && hit < 0; ++pass) {
						const float v = pass == 0 ? value : flt(n, cp::SwDefault);
						for (uint16_t c = 0; c < n.children_count; ++c) {
							if (std::fabs(flt(child(n, c), cp::CaseValue) - v) <= kCaseTolerance) {
								hit = c;
								break;
							}
						}
					}
				}
				for (uint16_t c = 0; c < n.children_count; ++c) {
					write(port(child(n, c), cp::SwitchCaseTrigger), int32_t(c) == hit ? 1.0f : 0.0f);
				}
				break;
			}
			case SoundNodeType::ValueSelector: {
				const float value = read(port(n, cp::VsValue));
				float out = 0.0f;
				bool found = false;
				for (int pass = 0; pass < 2 && !found; ++pass) {
					const float v = pass == 0 ? value : flt(n, cp::VsDefault);
					for (uint16_t c = 0; c < n.children_count; ++c) {
						const SoundNode &e = child(n, c);
						if (std::fabs(flt(e, cp::CaseValue) - v) <= kCaseTolerance) {
							out = read(port(e, cp::VsEntryInput));
							found = true;
							break;
						}
					}
				}
				write(port(n, cp::VsOut), out);
				break;
			}
			case SoundNodeType::DeMux: {
				const bool trigger = on(read(port(n, cp::DeMuxTrigger)));
				int64_t hit = -1;
				if (trigger && n.children_count > 0) {
					const double v = std::floor(double(read(port(n, cp::DeMuxValue))));
					const int64_t count = n.children_count;
					if (n.flags & ca::kFlagWrapValue) {
						hit = int64_t(std::fmod(std::fmod(v, double(count)) + double(count), double(count)));
					} else {
						hit = v <= 0.0 ? 0 : (v >= double(count - 1) ? count - 1 : int64_t(v));
					}
				}
				for (uint16_t c = 0; c < n.children_count; ++c) {
					write(port(child(n, c), cp::DeMuxOutTrigger), int64_t(c) == hit ? 1.0f : 0.0f);
				}
				break;
			}
			case SoundNodeType::LogicalExpression: {
				const ca::LogicalOp op = ca::LogicalOp(n.option);
				bool all = n.children_count > 0, any = false;
				for (uint16_t c = 0; c < n.children_count; ++c) {
					const bool v = on(read(port(child(n, c), cp::LogicEntryTrigger)));
					all = all && v;
					any = any || v;
				}
				bool out = false;
				switch (op) {
					case ca::LogicalOp::And:
						out = all;
						break;
					case ca::LogicalOp::Or:
						out = any;
						break;
					case ca::LogicalOp::Nand:
						out = !all;
						break;
					case ca::LogicalOp::Nor:
						out = !any;
						break;
					case ca::LogicalOp::Unknown:
						break;
				}
				write(port(n, cp::LogicTrigger), out ? 1.0f : 0.0f);
				break;
			}
			case SoundNodeType::Condition:
				for (uint16_t c = 0; c < n.children_count; ++c) {
					const SoundNode &e = child(n, c);
					write(port(e, cp::CondTrue), condition(ca::ConditionOp(e.option), read(port(e, cp::CondX)), read(port(e, cp::CondY))) ? 1.0f : 0.0f);
				}
				break;
			case SoundNodeType::WaveSwitcher: {
				const uint32_t count = n.waves_count;
				bool changed = false;
				if (count > 0) {
					const ca::SoundPort &index_port = port(n, cp::WsIndex);
					uint32_t idx = s.index;
					if (index_port.connected()) {
						const float v = std::floor(read(index_port));
						idx = v <= 0.0f ? 0u : (v >= float(count - 1) ? count - 1 : uint32_t(v));
					} else if (on(read(port(n, cp::WsAdvance)))) {
						if ((n.flags & ca::kFlagIsRandom) && count > 1) {
							idx = (idx + 1 + uint32_t(uniform01(seed, ni, s.draws++) * double(count - 1))) % count;
						} else {
							idx = (idx + 1) % count;
						}
					}
					changed = idx != s.index;
					s.index = idx;
					const uint16_t w = p.wave_refs[n.waves_begin + s.index];
					write(port(n, cp::WsWave), w == ca::kNoRegister ? 0.0f : float(w) + 1.0f);
				} else {
					write(port(n, cp::WsWave), 0.0f);
				}
				write(port(n, cp::WsIndexChanged), changed ? 1.0f : 0.0f);
				break;
			}
			case SoundNodeType::CameraShake:
				haptics.shake_pitch += read(port(n, cp::ShakePitch));
				haptics.shake_roll += read(port(n, cp::ShakeRoll));
				haptics.shake_yaw += read(port(n, cp::ShakeYaw));
				break;
			case SoundNodeType::PadRumble:
				haptics.rumble_low += read(port(n, cp::RumbleLow));
				haptics.rumble_high += read(port(n, cp::RumbleHigh));
				break;
			case SoundNodeType::Sampler:
				sampler_events(ni, n, p_frame);
				break;
			default:
				break; // audio-rate nodes act through their plugins; unimplemented types are no-ops
		}
	}
	for (const AttributeDrive &d : drives) {
		plugins[d.plugin].attributes[d.attribute] = read(d.port);
	}
	// Output attenuation: curve(distance) + Gain, and the cone (design §6.3).
	for (size_t o = 0; o < p.outputs.size(); ++o) {
		const ca::SoundOutput &out = p.outputs[o];
		const float metres = geometry.distance;
		float d = metres;
		if (metres < out.min_distance) {
			d = metres + (out.min_distance - metres) * tuning.min_distance_clamp;
		}
		outputs[o].curve_gain_db = out.attenuation.evaluate(d * tuning.curve_x_metres) + out.gain_db;
		float cone = 0.0f;
		if (out.kind == ca::OutputKind::Cone) {
			const SoundNode &n = p.nodes[out.node];
			const float inner = std::clamp(read(port(n, cp::ConeInnerAngle)), 0.0f, 360.0f) * 0.5f;
			const float outer = std::clamp(read(port(n, cp::ConeOuterAngle)), 0.0f, 360.0f) * 0.5f;
			const float a = geometry.cone_angle_deg;
			if (a > inner) {
				cone = a >= outer || outer <= inner ? out.outside_gain_db : out.outside_gain_db * (a - inner) / (outer - inner);
			}
		}
		outputs[o].cone_db = cone;
		// PT1 rld [bypass headroom]: port 0 of the Cone, Flat and Direct outputs (the weapons' Handling: IsFirstPerson).
		static_assert(cp::ConeBypassHeadroom == 0); // PT1 rld [bypass headroom]
		outputs[o].bypass_headroom = out.node < p.nodes.size() && p.nodes[out.node].ports_count > 0 && p.nodes[out.node].ports_begin < p.ports.size() && // PT1 rld [bypass headroom]
				read(port(p.nodes[out.node], 0)) > 0.5f; // PT1 rld [bypass headroom]
	}
}

void PatchInstance::State::advance(double p_seconds) {
	const ca::SoundProgram &p = *program;
	const float dt = float(p_seconds);
	if (!(dt > 0.0f)) {
		return;
	}
	for (uint32_t ni = 0; ni < p.nodes.size(); ++ni) {
		const SoundNode &n = p.nodes[ni];
		NodeState &s = nodes[ni];
		switch (n.type) {
			case SoundNodeType::Adsr: {
				const float a = read(port(n, cp::AdsrA)), d = read(port(n, cp::AdsrD)), sus = std::clamp(read(port(n, cp::AdsrS)), 0.0f, 1.0f);
				switch (AdsrPhase(s.mode)) {
					case AdsrPhase::Attack:
						s.value = a > 0.0f ? s.value + dt / a : 1.0f;
						if (s.value >= 1.0f) {
							s.value = 1.0f;
							s.mode = uint8_t(AdsrPhase::Decay);
						}
						break;
					case AdsrPhase::Decay:
						s.value = d > 0.0f ? s.value - (1.0f - sus) * dt / d : sus;
						if (s.value <= sus) {
							s.value = sus;
							s.mode = uint8_t(AdsrPhase::Sustain);
						}
						break;
					case AdsrPhase::Release:
						s.value -= s.release_rate * dt;
						if (!(s.value > 0.0f)) {
							s.value = 0.0f;
							s.mode = uint8_t(AdsrPhase::Idle);
							s.pulse = 1;
						}
						break;
					default:
						break;
				}
				break;
			}
			case SoundNodeType::Lfo: {
				const double hz = read(port(n, cp::LfoHz));
				if (std::isfinite(hz)) {
					s.phase += hz * p_seconds;
					s.phase -= std::floor(s.phase);
				}
				break;
			}
			case SoundNodeType::ParameterFilterLinear: {
				const float in = read(port(n, cp::PflIn));
				if (in > s.value) {
					const float up = std::max(0.0f, read(port(n, cp::PflAttack)));
					s.value = std::min(in, s.value + up * dt);
				} else if (in < s.value) {
					const float down = std::max(0.0f, read(port(n, cp::PflRelease)));
					s.value = std::max(in, s.value - down * dt);
				}
				break;
			}
			case SoundNodeType::Timer:
				if (s.mode) {
					const float period = read(port(n, cp::TimerPeriod));
					s.value += dt;
					if (period > 0.0f && s.value >= period) {
						s.pulse = 1;
						if (ca::TimerMode(n.option) == ca::TimerMode::SingleShot) {
							s.mode = 0;
							s.value = period;
						} else {
							s.value = std::fmod(s.value, period);
						}
					}
				}
				break;
			case SoundNodeType::EventGate:
				if (s.value > 0.0f) {
					s.value = std::max(0.0f, s.value - dt);
				}
				break;
			default:
				break;
		}
	}
}

} // namespace bf3::audio
