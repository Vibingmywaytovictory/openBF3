#include "bf3/audio/movement_foley.h"

#include "patch_state.h"

#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/gameplay/soldier_sound.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace bf3::audio {

namespace ssp = core::gameplay::soldier_sound_port;

namespace {

constexpr uint32_t kJump = uint32_t(core::gameplay::logic_port_id("Jump"));

uint16_t soldier_key(const FootstepSoldier &p_s) {
	return uint16_t((uint16_t(p_s.slot) << 8) | p_s.generation);
}

float speed_of(const Vec3 &p_v) {
	return std::sqrt(p_v.x * p_v.x + p_v.y * p_v.y + p_v.z * p_v.z);
}

}

MovementFoley::MovementFoley(std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning) :
		program_(std::move(p_program)), bank_(std::move(p_bank)), tuning_(p_tuning) {}

bool MovementFoley::push(MixerCore &r_mixer, MixerCommandType p_type, uint32_t p_hash, float p_value) {
	MixerCommand c;
	c.type = p_type;
	c.handle = handle_;
	c.name_hash = p_hash;
	c.value = p_value;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return false;
	}
	return true;
}

bool MovementFoley::start(
		MixerCore &r_mixer, const FootstepSoldier &p_own, float p_speed, const ListenerState &p_listener) {
	core::Result<std::unique_ptr<PatchInstance>> inst = PatchInstance::create(
			program_, bank_, tuning_, core::splitmix64(uint64_t(soldier_key(p_own)) ^ (stats_.starts << 24)), kMixRate);
	if (!inst) {
		++stats_.create_failures;
		return false;
	}
	EmitterState em;
	em.position = p_own.position;
	(*inst)->set_geometry(em, p_listener);
	(*inst)->set_parameter(uint32_t(ssp::kSpeed), p_speed);
	(*inst)->set_parameter(uint32_t(ssp::kIsFirstPerson), p_own.first_person ? 1.0f : 0.0f);
	(*inst)->set_sampler_trace(trace_next_);
	const InstanceHandle handle = r_mixer.next_handle();
	MixerCommand c;
	c.type = MixerCommandType::Start;
	c.handle = handle;
	c.instance = inst->get();
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return false;
	}
	(void)inst->release();
	if (trace_next_ != nullptr) {
		traced_ = handle;
		trace_next_ = nullptr;
	}
	handle_ = handle;
	key_ = soldier_key(p_own);
	speed_ = p_speed;
	first_person_ = p_own.first_person;
	mode_ = p_own.mode;
	if (program_->start_event != 0) {
		(void)push(r_mixer, MixerCommandType::PostEvent, program_->start_event, 0.0f);
	}
	++stats_.starts;
	return true;
}

void MovementFoley::step(MixerCore &r_mixer, const FootstepSoldier *p_own, const ListenerState &p_listener) {
	const bool alive = p_own != nullptr && p_own->mode != sim::MoveMode::Dead && core::finite(p_own->position) &&
			core::finite(p_own->velocity);
	if (handle_ != kNoInstance && (!alive || soldier_key(*p_own) != key_)) {
		stop(r_mixer);
	}
	if (!alive || !program_) {
		return;
	}
	const float speed = speed_of(p_own->velocity);
	stats_.peak_speed = std::max(stats_.peak_speed, speed);
	if (handle_ == kNoInstance) {
		(void)start(r_mixer, *p_own, speed, p_listener);
		return;
	}
	MixerCommand em;
	em.type = MixerCommandType::SetEmitter;
	em.handle = handle_;
	em.emitter.position = p_own->position;
	if (!r_mixer.push(em)) {
		++stats_.push_failures;
	}
	if (std::fabs(speed - speed_) >= kMovementSpeedStep &&
			push(r_mixer, MixerCommandType::SetParameter, uint32_t(ssp::kSpeed), speed)) {
		speed_ = speed;
		++stats_.updates;
	}
	if (p_own->first_person != first_person_ &&
			push(r_mixer, MixerCommandType::SetParameter, uint32_t(ssp::kIsFirstPerson),
					p_own->first_person ? 1.0f : 0.0f)) {
		first_person_ = p_own->first_person;
	}
	if (p_own->mode == sim::MoveMode::Jumping && mode_ != sim::MoveMode::Jumping &&
			push(r_mixer, MixerCommandType::PostEvent, kJump, 0.0f)) {
		++stats_.jumps;
	}
	mode_ = p_own->mode;
}

void MovementFoley::on_cut(const MixerCutEvent &p_event) {
	const MixerCutKind k = p_event.kind;
	if (k != MixerCutKind::Finish && k != MixerCutKind::Refused && k != MixerCutKind::Cull && k != MixerCutKind::Kill &&
			k != MixerCutKind::Drop) {
		return;
	}
	if (traced_ != kNoInstance && p_event.handle == traced_) {
		traced_ = kNoInstance;
		trace_closed_ = true;
	}
	if (handle_ != kNoInstance && p_event.handle == handle_) {
		handle_ = kNoInstance;
		++stats_.ended;
	}
}

void MovementFoley::trace_next(SamplerTrace *r_trace) {
	trace_next_ = r_trace;
	trace_closed_ = false;
}

std::optional<SpeedLoop> speed_triggered_loop(const core::audio::SoundProgram &p_program) {
	namespace ca = core::audio;
	namespace cp = core::audio::port;
	const std::optional<uint16_t> speed = p_program.input_register(uint32_t(ssp::kSpeed));
	if (!speed) {
		return std::nullopt;
	}
	const auto port = [&](const ca::SoundNode &p_node, uint32_t p_index) -> const ca::SoundPort & {
		return p_program.ports[p_node.ports_begin + p_index];
	};
	const auto children = [&](const ca::SoundNode &p_node, auto &&p_visit) {
		for (uint16_t c = 0; c < p_node.children_count; ++c) {
			p_visit(p_program.child_nodes[p_program.children[p_node.children_begin + c]]);
		}
	};
	const auto from_speed = [&](uint16_t p_reg) {
		bool fed = p_reg == *speed;
		for (const ca::SoundNode &n : p_program.nodes) {
			if (n.type == ca::SoundNodeType::Receive) {
				children(n, [&](const ca::SoundNode &e) {
					fed = fed || (port(e, cp::ReceiveOut).reg == p_reg && port(e, cp::ReceiveSource).reg == *speed);
				});
			}
		}
		return fed;
	};
	for (uint32_t i = 0; i < p_program.nodes.size(); ++i) {
		const ca::SoundNode &s = p_program.nodes[i];
		if (s.type != ca::SoundNodeType::Sampler || ca::SamplerLoop(s.option) == ca::SamplerLoop::None ||
				!port(s, cp::SmpTrigger).connected()) {
			continue;
		}
		const uint16_t trigger = port(s, cp::SmpTrigger).reg;
		std::optional<SpeedLoop> found;
		for (const ca::SoundNode &n : p_program.nodes) {
			if (n.type != ca::SoundNodeType::Condition) {
				continue;
			}
			children(n, [&](const ca::SoundNode &e) {
				if (!found && port(e, cp::CondTrue).reg == trigger && !port(e, cp::CondY).connected() &&
						port(e, cp::CondX).connected() && from_speed(port(e, cp::CondX).reg)) {
					found = SpeedLoop{ i, uint32_t(s.plugins_count / cp::kSamplerSlotPlugins),
						ca::ConditionOp(e.option), port(e, cp::CondY).constant };
				}
			});
		}
		if (found) {
			return found;
		}
	}
	return std::nullopt;
}

bool speed_loop_on(const SpeedLoop &p_loop, float p_speed) {
	return detail::condition(p_loop.op, p_speed, p_loop.threshold);
}

LoopVoices loop_voices(const SamplerTrace &p_trace, uint32_t p_node, uint64_t p_end_frame) {
	std::vector<SamplerTraceEntry> e;
	for (uint32_t i = 0; i < std::min<size_t>(p_trace.count, p_trace.entries.size()); ++i) {
		if (p_trace.entries[i].node == p_node) {
			e.push_back(p_trace.entries[i]);
		}
	}
	std::stable_sort(e.begin(), e.end(),
			[](const SamplerTraceEntry &a, const SamplerTraceEntry &b) { return a.frame < b.frame; });
	LoopVoices out;
	std::array<bool, 256> busy{};
	uint32_t count = 0;
	uint64_t at = e.empty() ? 0 : e.front().frame, stack_since = 0;
	const auto close_stack = [&](uint64_t p_frame) {
		if (count >= 2) {
			out.stacked_frames += p_frame - at;
			out.longest_stack_frames = std::max(out.longest_stack_frames, p_frame - stack_since);
		}
	};
	for (const SamplerTraceEntry &x : e) {
		close_stack(x.frame);
		const uint32_t before = count;
		if (x.kind == SamplerTraceKind::Trigger || x.kind == SamplerTraceKind::Step) {
			out.starts += x.kind == SamplerTraceKind::Trigger ? 1u : 0u;
			count += busy[x.slot] ? 0u : 1u;
			busy[x.slot] = true;
		} else if (x.kind == SamplerTraceKind::End && busy[x.slot]) {
			busy[x.slot] = false;
			--count;
		}
		if (before < 2 && count >= 2) {
			stack_since = x.frame;
		}
		at = x.frame;
		out.max_busy = std::max(out.max_busy, count);
	}
	close_stack(std::max(p_end_frame, at));
	return out;
}

void MovementFoley::stop(MixerCore &r_mixer) {
	if (handle_ == kNoInstance) {
		return;
	}
	if (speed_ != 0.0f && push(r_mixer, MixerCommandType::SetParameter, uint32_t(ssp::kSpeed), 0.0f)) {
		speed_ = 0.0f;
	}
	MixerCommand c;
	c.type = MixerCommandType::Stop;
	c.handle = handle_;
	if (!r_mixer.push(c)) {
		++stats_.push_failures;
		return;
	}
	handle_ = kNoInstance;
	++stats_.stops;
}

}
