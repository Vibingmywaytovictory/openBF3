#include "bf3/audio/movement_foley.h"

#include "bf3/core/gameplay/soldier_sound.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <cmath>

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
	if (handle_ != kNoInstance && p_event.handle == handle_) {
		handle_ = kNoInstance;
		++stats_.ended;
	}
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
