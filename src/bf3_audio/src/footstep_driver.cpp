#include "bf3/audio/footstep_driver.h"

#include "bf3/core/util/finite.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;
namespace ssp = core::gameplay::soldier_sound_port;

using bf3::core::finite;

namespace {

constexpr uint64_t kFootstepJoinTag = uint64_t(1) << 62;
constexpr uint32_t kFootstepTrackFrames = 30;

float horizontal_speed(const Vec3 &p_v) {
	return std::sqrt(p_v.x * p_v.x + p_v.z * p_v.z);
}

float distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x, dy = p_a.y - p_b.y, dz = p_a.z - p_b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool airborne(sim::MoveMode p_mode) {
	return p_mode == sim::MoveMode::Jumping || p_mode == sim::MoveMode::InAir || p_mode == sim::MoveMode::Falling;
}

uint16_t track_key(const FootstepSoldier &p_s) {
	return uint16_t((uint16_t(p_s.slot) << 8) | p_s.generation);
}

}

float audible_radius(const ca::SoundProgram &p_program) {
	float r = 0.0f;
	for (const ca::SoundOutput &o : p_program.outputs) {
		for (const ca::AudioCurvePoint &p : o.attenuation.points) {
			if (std::isfinite(p.x)) {
				r = std::max(r, p.x);
			}
		}
	}
	return r;
}

FootstepParams footstep_params(const core::gameplay::SoldierSoundDef &p_def, const ca::SoundProgram &p_footsteps) {
	FootstepParams p;
	p.walk_threshold = p_def.walk_velocity_threshold;
	p.run_threshold = p_def.run_velocity_threshold;
	p.walk_period = p_def.walk_period;
	p.run_period = p_def.run_period;
	p.start_above = p_def.start_above;
	p.stop_at_or_below = p_def.stop_at_or_below;
	p.speed_scale = p_def.speed_scale;
	p.speed_input = p_def.speed_input;
	p.radius_m = audible_radius(p_footsteps);
	return p;
}

uint32_t footstep_event_hash(FootstepEvent p_event) {
	switch (p_event) {
		case FootstepEvent::Start:
			return uint32_t(ssp::kOnStart);
		case FootstepEvent::Stop:
			return uint32_t(ssp::kOnStop);
		case FootstepEvent::Jump:
			return uint32_t(ssp::kOnJump);
		case FootstepEvent::Land:
			return uint32_t(ssp::kOnLand);
		case FootstepEvent::Left:
			return uint32_t(ssp::kFootPlantLeft);
		case FootstepEvent::Right:
			return uint32_t(ssp::kFootPlantRight);
	}
	return 0;
}

FootstepScheduler::FootstepScheduler(const FootstepParams &p_params) : params_(p_params) {
	tracks_.reserve(kMaxFootstepSoldiers);
}

void FootstepScheduler::step(std::span<const FootstepSoldier> p_soldiers, const Vec3 &p_listener, double p_dt,
		std::vector<FootstepPost> &r_out) {
	++frame_;
	const double dt = std::isfinite(p_dt) ? std::clamp(p_dt, 0.0, 0.25) : 0.0;
	for (const FootstepSoldier &s : p_soldiers) {
		if (!finite(s.position) || !finite(s.velocity) || s.mode == sim::MoveMode::Dead) {
			continue;
		}
		const float d = distance(s.position, p_listener);
		if (!(d <= params_.radius_m)) {
			continue;
		}
		const uint16_t key = track_key(s);
		auto it = std::find_if(tracks_.begin(), tracks_.end(), [key](const Track &t) { return t.key == key; });
		const float speed = horizontal_speed(s.velocity);
		const float normalized = params_.speed_scale > 0.0f ? speed / params_.speed_scale : 0.0f;
		const auto post = [&](FootstepEvent p_event) { r_out.push_back(FootstepPost{ s.slot, p_event, speed, d }); };
		if (it == tracks_.end()) {
			if (tracks_.size() >= kMaxFootstepSoldiers) {
				continue;
			}
			Track t;
			t.key = key;
			t.seen = frame_;
			t.moving = normalized > params_.start_above;
			t.mode = s.mode;
			tracks_.push_back(t);
			continue;
		}
		Track &t = *it;
		t.seen = frame_;
		if (s.mode == sim::MoveMode::Jumping && t.mode != sim::MoveMode::Jumping) {
			post(FootstepEvent::Jump);
		} else if (s.mode == sim::MoveMode::OnGround && airborne(t.mode)) {
			post(FootstepEvent::Land);
		}
		t.mode = s.mode;
		if (!t.moving && normalized > params_.start_above) {
			t.moving = true;
			post(FootstepEvent::Start);
		} else if (t.moving && normalized <= params_.stop_at_or_below) {
			t.moving = false;
			post(FootstepEvent::Stop);
		}
		if (s.mode != sim::MoveMode::OnGround || !(speed > params_.walk_threshold)) {
			t.phase = 0.0;
			continue;
		}
		const double period = speed > params_.run_threshold ? params_.run_period : params_.walk_period;
		t.phase += dt;
		if (period > 0.0 && t.phase >= period) {
			t.phase = std::min(t.phase - period, period);
			post(t.left_next ? FootstepEvent::Left : FootstepEvent::Right);
			t.left_next = !t.left_next;
		}
	}
	const uint64_t frame = frame_;
	tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
						  [frame](const Track &t) { return frame - t.seen > kFootstepTrackFrames; }),
			tracks_.end());
}

struct FootstepDriver::Impl {
	MixerCore &mixer;
	std::shared_ptr<const ca::SoundProgram> program;
	std::shared_ptr<const ca::WaveBank> bank;
	AudioTuning tuning;
	OneShotPool &pool;
	FootstepGround ground;
	FootstepScheduler scheduler;
	FootstepStats stats;
	std::vector<FootstepPost> posts;
	uint64_t seed_counter = 0;

	Impl(MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program,
			std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool,
			const FootstepParams &p_params, FootstepGround p_ground) :
			mixer(r_mixer), program(std::move(p_program)), bank(std::move(p_bank)), tuning(p_tuning), pool(r_pool),
			ground(std::move(p_ground)), scheduler(p_params) {
		posts.reserve(kMaxFootstepSoldiers * 4);
	}

	void play(const FootstepPost &p_post, const FootstepSoldier &p_soldier, const ListenerState &p_listener,
			uint64_t p_frame) {
		const std::optional<float> material = ground ? ground(p_soldier.position) : std::nullopt;
		if (!material) {
			++stats.no_ground;
			return;
		}
		const float id = *material;
		const FootstepParams &params = scheduler.params();
		const float pose = float(p_soldier.pose);
		const float speed = p_post.speed;
		OneShotOptions o;
		o.event = footstep_event_hash(p_post.event);
		o.set_first_person = true;
		o.first_person = p_soldier.first_person;
		const uint64_t material_key = uint64_t(std::clamp(std::lround(id), 0l, 255l));
		o.join_key = kFootstepJoinTag | (uint64_t(p_soldier.slot) << 16) | (material_key << 8) |
				(uint64_t(p_soldier.pose) << 1) | uint64_t(p_soldier.first_person);
		o.before_start = [id, pose, speed, input = params.speed_input](PatchInstance &r_instance, InstanceHandle) {
			r_instance.set_parameter(uint32_t(ssp::kSoundMaterialId), id);
			r_instance.set_parameter(uint32_t(ssp::kPose), pose);
			r_instance.set_parameter(input, speed);
		};
		EmitterState em;
		em.position = p_soldier.position;
		const uint64_t seed = core::splitmix64(
				(uint64_t(p_soldier.slot) << 56) ^ (uint64_t(p_soldier.generation) << 48) ^ ++seed_counter);
		const uint64_t joins = pool.join_requests();
		if (!pool.start(mixer, program, bank, tuning, seed, em, p_listener, p_frame, o)) {
			++stats.refused;
			return;
		}
		if (pool.join_requests() != joins) {
			++stats.joined;
			MixerCommand c;
			c.type = MixerCommandType::SetParameter;
			c.handle = pool.last_handle();
			c.name_hash = params.speed_input;
			c.value = speed;
			c.at_frame = p_frame;
			(void)mixer.push(c);
		}
		++stats.posts[size_t(p_post.event)];
		++stats.by_material[std::min<size_t>(size_t(material_key), kFootstepMaterialIds)];
		stats.farthest_m = std::max(stats.farthest_m, p_post.distance_m);
	}
};

FootstepDriver::FootstepDriver(MixerCore &r_mixer, std::shared_ptr<const ca::SoundProgram> p_program,
		std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning, OneShotPool &r_pool,
		const FootstepParams &p_params, FootstepGround p_ground) :
		impl_(std::make_unique<Impl>(
				r_mixer, std::move(p_program), std::move(p_bank), p_tuning, r_pool, p_params, std::move(p_ground))) {}

FootstepDriver::~FootstepDriver() = default;

void FootstepDriver::step(
		std::span<const FootstepSoldier> p_soldiers, const ListenerState &p_listener, double p_dt, uint64_t p_frame) {
	Impl &m = *impl_;
	++m.stats.frames;
	m.posts.clear();
	if (!m.program) {
		return;
	}
	m.scheduler.step(p_soldiers, p_listener.position, p_dt, m.posts);
	for (const FootstepPost &p : m.posts) {
		for (const FootstepSoldier &s : p_soldiers) {
			if (s.slot == p.slot) {
				m.play(p, s, p_listener, p_frame);
				break;
			}
		}
	}
	m.stats.tracked = uint32_t(m.scheduler.tracked());
	m.stats.peak_tracked = std::max(m.stats.peak_tracked, m.stats.tracked);
}

const FootstepStats &FootstepDriver::stats() const {
	return impl_->stats;
}

std::span<const FootstepPost> FootstepDriver::last_posts() const {
	return impl_->posts;
}

}
