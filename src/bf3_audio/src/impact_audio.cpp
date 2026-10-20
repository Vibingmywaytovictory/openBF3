#include "bf3/audio/impact_audio.h"

#include "bf3/audio/audio_load.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;

namespace {

constexpr size_t kMaxImpactPatches = 4096;

constexpr size_t kMaxEffectSounds = kMaxOneShotVoices * 2;

constexpr float kMaxImpactDistanceM = 400.0f;

bool finite(const Vec3 &p_v) {
	return std::isfinite(p_v.x) && std::isfinite(p_v.y) && std::isfinite(p_v.z);
}

float distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x, dy = p_a.y - p_b.y, dz = p_a.z - p_b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}

core::Result<std::shared_ptr<const ImpactSoundCache>> ImpactSoundCache::build(const core::EbxResolver &p_db, std::span<const std::string> p_patches) {
	auto cache = std::make_shared<ImpactSoundCache>();
	size_t seen = 0;
	for (const std::string &name : p_patches) {
		if (name.empty() || cache->programs_.count(name) != 0) {
			continue;
		}
		bool dup = false;
		for (const std::string &e : cache->errors_) {
			if (e.size() > name.size() && e.compare(0, name.size(), name) == 0 && e[name.size()] == ':') {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		if (++seen > kMaxImpactPatches) {
			cache->errors_.push_back(name + ": more than " + std::to_string(kMaxImpactPatches) + " impact patches");
			continue;
		}

		core::Result<std::shared_ptr<const ca::SoundProgram>> prog = compile_patch(p_db, name);
		if (!prog) {
			ca::CompileOptions lenient;
			lenient.allow_unimplemented = true;
			core::Result<std::shared_ptr<const ca::SoundProgram>> lax = compile_patch(p_db, name, lenient);
			if (!lax) {
				cache->errors_.push_back(name + ": " + prog.error().message);
				continue;
			}
			cache->errors_.push_back(name + ": (lenient) " + prog.error().message);
			cache->programs_.emplace(name, std::move(*lax));
			continue;
		}
		cache->programs_.emplace(name, std::move(*prog));
	}
	return std::shared_ptr<const ImpactSoundCache>(std::move(cache));
}

std::shared_ptr<const ImpactSoundCache> ImpactSoundCache::from_programs(
		std::span<const std::pair<std::string, std::shared_ptr<const ca::SoundProgram>>> p_programs) {
	auto cache = std::make_shared<ImpactSoundCache>();
	for (const auto &[name, program] : p_programs) {
		if (!name.empty() && program) {
			cache->programs_.emplace(name, program);
		}
	}
	return cache;
}

std::shared_ptr<const ca::SoundProgram> ImpactSoundCache::find(std::string_view p_patch) const {
	const auto it = programs_.find(std::string(p_patch));
	return it == programs_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const ca::SoundProgram>> ImpactSoundCache::programs() const {
	std::vector<std::pair<std::string, std::shared_ptr<const ca::SoundProgram>>> sorted(programs_.begin(), programs_.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	std::vector<std::shared_ptr<const ca::SoundProgram>> out;
	out.reserve(sorted.size());
	for (auto &p : sorted) {
		out.push_back(std::move(p.second));
	}
	return out;
}

size_t ImpactSoundCache::compiled() const {
	return programs_.size();
}

size_t ImpactSoundCache::failed() const {
	size_t n = 0;
	for (const std::string &e : errors_) {
		if (e.find(": (lenient) ") == std::string::npos) {
			++n;
		}
	}
	return n;
}

std::span<const std::string> ImpactSoundCache::errors() const {
	return errors_;
}

struct ImpactAudio::Impl {
	MixerCore &mixer;
	std::shared_ptr<const ImpactSoundCache> cache;
	std::shared_ptr<const ca::WaveBank> bank;
	AudioTuning tuning;
	OneShotPool &pool;
	ListenerState listener;
	ImpactAudioStats stats;
	struct EffectSound {
		uint64_t effect = 0;
		uint32_t component = 0;
		InstanceHandle handle = kNoInstance;
	};
	std::vector<EffectSound> effect_sounds;

	Impl(MixerCore &r_mixer, std::shared_ptr<const ImpactSoundCache> p_cache, std::shared_ptr<const ca::WaveBank> p_bank, const AudioTuning &p_tuning,
			OneShotPool &r_pool) :
			mixer(r_mixer), cache(std::move(p_cache)), bank(std::move(p_bank)), tuning(p_tuning), pool(r_pool) {
		effect_sounds.reserve(kMaxEffectSounds);
	}

	bool start(const std::shared_ptr<const ca::SoundProgram> &p_program, uint64_t p_seed, const Vec3 &p_position, uint64_t p_frame) {
		EmitterState em;
		em.position = p_position;
		if (!pool.start(mixer, p_program, bank, tuning, p_seed, em, listener, p_frame)) {
			++stats.refused;
			return false;
		}
		return true;
	}

	void prune() {
		effect_sounds.erase(std::remove_if(effect_sounds.begin(), effect_sounds.end(), [&](const EffectSound &e) { return !pool.contains(e.handle); }),
				effect_sounds.end());
	}
};

ImpactAudio::ImpactAudio(MixerCore &r_mixer, std::shared_ptr<const ImpactSoundCache> p_cache, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, OneShotPool &r_pool) :
		impl_(std::make_unique<Impl>(r_mixer, std::move(p_cache), std::move(p_bank), p_tuning, r_pool)) {}

ImpactAudio::~ImpactAudio() = default;

void ImpactAudio::set_listener(const ListenerState &p_listener) {
	impl_->listener = p_listener;
}

void ImpactAudio::play_impact(const ImpactSoundRequest &p_request, SimMixerClock &r_clock) {
	Impl &m = *impl_;
	std::shared_ptr<const ca::SoundProgram> program = m.cache ? m.cache->find(p_request.patch) : nullptr;
	if (!program) {
		++m.stats.unknown_patches;
		return;
	}
	if (!finite(p_request.position) || distance(p_request.position, m.listener.position) > kMaxImpactDistanceM) {
		return;
	}
	if (m.start(program, p_request.seed, p_request.position, r_clock.frame_of(p_request.time, p_request.timeline))) {
		++m.stats.impacts;
	}
}

void ImpactAudio::on_effect_sound(const EffectSoundRequest &p_request, SimMixerClock &r_clock) {
	Impl &m = *impl_;
	m.prune();
	if (!p_request.start) {
		for (size_t i = 0; i < m.effect_sounds.size(); ++i) {
			const Impl::EffectSound &e = m.effect_sounds[i];
			if (e.effect == p_request.effect && e.component == p_request.component) {
				(void)m.pool.stop(m.mixer, e.handle);
				m.effect_sounds.erase(m.effect_sounds.begin() + std::ptrdiff_t(i));
				++m.stats.effect_stops;
				return;
			}
		}
		return;
	}
	std::shared_ptr<const ca::SoundProgram> program = m.cache ? m.cache->find(p_request.patch) : nullptr;
	if (!program) {
		++m.stats.unknown_patches;
		return;
	}
	if (!finite(p_request.position) || distance(p_request.position, m.listener.position) > kMaxImpactDistanceM) {
		return;
	}
	const uint64_t seed = sim::weapons::splitmix64(p_request.effect ^ (uint64_t(p_request.component) << 48));
	if (!m.start(program, seed, p_request.position, r_clock.earliest_frame())) {
		return;
	}
	++m.stats.effect_sounds;
	if (m.effect_sounds.size() >= kMaxEffectSounds) {
		m.effect_sounds.erase(m.effect_sounds.begin());
	}
	m.effect_sounds.push_back(Impl::EffectSound{ p_request.effect, p_request.component, m.pool.last_handle() });
}

const ImpactAudioStats &ImpactAudio::stats() const {
	return impl_->stats;
}

}
