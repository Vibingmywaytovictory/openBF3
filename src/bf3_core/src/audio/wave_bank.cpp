#include "bf3/core/audio/wave_bank.h"

#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace bf3::core::audio {

namespace {

constexpr uint32_t kNotResident = 0xffffffffu;

std::string loc_superbundle(std::string_view p_language) {
	static constexpr std::pair<std::string_view, std::string_view> kCodes[] = {
		{ "English", "en" },
		{ "French", "fr" },
		{ "German", "de" },
		{ "Italian", "it" },
		{ "Spanish", "es" },
		{ "Polish", "pl" },
		{ "Russian", "ru" },
		{ "Japanese", "ja" },
	};
	for (const auto &[name, code] : kCodes) {
		if (name == p_language) {
			return "Win32/Loc/" + std::string(code);
		}
	}
	const std::string code = to_lower_ascii(p_language.substr(0, 2));
	return "Win32/Loc/" + code;
}

}

Result<std::shared_ptr<const WaveBank>> WaveBank::load(const Install &p_install, const LevelMount &p_mount, std::vector<SoundWaveInfo> p_waves,
		const WaveBankOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	std::shared_ptr<WaveBank> bank(new WaveBank());
	bank->waves_ = std::move(p_waves);
	bank->wave_chunks_.resize(bank->waves_.size());
	bank->stats_.waves = bank->waves_.size();

	struct ChunkJob {
		Guid id;
		uint32_t size = 0;
		bool localised = false;
		std::string loc_superbundle;
		std::string first_wave;
	};
	std::vector<ChunkJob> jobs;
	std::unordered_map<Guid, uint32_t, GuidHash> job_of;
	uint64_t total = 0;
	for (size_t w = 0; w < bank->waves_.size(); ++w) {
		const SoundWaveInfo &wave = bank->waves_[w];
		bank->wave_chunks_[w].assign(wave.chunks.size(), kNotResident);
		if (!bank->by_partition_.emplace(wave.partition, WaveId(w)).second) {
			continue;
		}
		if (wave.localised() && !p_options.include_localised) {
			++bank->stats_.skipped_localised;
			continue;
		}

		const std::optional<std::pair<uint32_t, uint32_t>> range = playable_variations(wave, p_options.language);
		if (!range) {
			++bank->stats_.skipped_localised;
			continue;
		}
		std::vector<bool> used(wave.chunks.size(), !wave.localised());
		for (uint32_t v = range->first; v < range->first + range->second; ++v) {
			used[wave.variations[v].chunk_index] = true;
		}
		for (size_t c = 0; c < wave.chunks.size(); ++c) {
			if (!used[c]) {
				continue;
			}
			auto [it, fresh] = job_of.emplace(wave.chunks[c].id, uint32_t(jobs.size()));
			if (fresh) {
				ChunkJob job;
				job.id = wave.chunks[c].id;
				job.size = wave.chunks[c].size;
				job.localised = wave.localised();
				job.loc_superbundle = loc_superbundle(p_options.language);
				job.first_wave = wave.name;
				jobs.push_back(std::move(job));
				total += wave.chunks[c].size;
			} else if (jobs[it->second].size != wave.chunks[c].size) {
				return make_error("wave bank: chunk " + wave.chunks[c].id.to_string() + " is listed with " + std::to_string(jobs[it->second].size) + " bytes by " +
						jobs[it->second].first_wave + " and " + std::to_string(wave.chunks[c].size) + " by " + wave.name);
			}
			bank->wave_chunks_[w][c] = it->second;
		}
		++bank->stats_.resident_waves;
	}
	if (total > p_options.resident_budget_bytes) {
		return make_error("wave bank: " + std::to_string(jobs.size()) + " chunks need " + std::to_string(total) + " bytes, over the resident budget of " +
				std::to_string(p_options.resident_budget_bytes) + " bytes");
	}

	bank->chunk_bytes_.resize(jobs.size());
	std::vector<std::string> errors(jobs.size());
	parallel_for(
			jobs.size(),
			[&](size_t i) {
				const ChunkJob &job = jobs[i];
				Result<std::vector<uint8_t>> bytes = p_mount.read_chunk(job.id);
				if (!bytes && job.localised) {
					Result<std::optional<std::vector<uint8_t>>> loc = ui::read_toc_chunk(p_install, job.loc_superbundle, job.id);
					if (loc && loc->has_value()) {
						bytes = std::move(**loc);
					} else if (!loc) {
						errors[i] = job.first_wave + ": chunk " + job.id.to_string() + ": " + loc.error().message;
						return;
					}
				}
				if (!bytes) {
					errors[i] = job.first_wave + ": chunk " + job.id.to_string() + ": " + bytes.error().message;
					return;
				}
				if (bytes->size() != job.size) {
					errors[i] = job.first_wave + ": chunk " + job.id.to_string() + " decodes to " + std::to_string(bytes->size()) + " bytes, ChunkSize is " +
							std::to_string(job.size);
					return;
				}
				bank->chunk_bytes_[i] = std::move(*bytes);
			},
			p_options.threads);
	for (const std::string &e : errors) {
		if (!e.empty()) {
			return make_error("wave bank: " + e);
		}
	}

	for (size_t w = 0; w < bank->waves_.size(); ++w) {
		const SoundWaveInfo &wave = bank->waves_[w];
		const std::optional<std::pair<uint32_t, uint32_t>> range = playable_variations(wave, p_options.language);
		if (!range) {
			continue;
		}
		for (uint32_t v = range->first; v < range->first + range->second; ++v) {
			const WaveVariation &var = wave.variations[v];
			const uint32_t job = bank->wave_chunks_[w][var.chunk_index];
			if (job == kNotResident) {
				continue;
			}
			const std::vector<uint8_t> &bytes = bank->chunk_bytes_[job];
			for (size_t s = 0; s < var.segments.size(); ++s) {
				Result<SoundStreamHeader> h = parse_stream_header(bytes, var.segments[s].samples_offset);
				if (!h) {
					return make_error("wave bank: " + wave.name + " variation " + std::to_string(v) + " segment " + std::to_string(s) + ": " + h.error().message);
				}
			}
		}
	}
	bank->stats_.chunks = jobs.size();
	bank->stats_.resident_bytes = total;
	bank->stats_.read_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return std::shared_ptr<const WaveBank>(std::move(bank));
}

std::optional<WaveId> WaveBank::find(const Guid &p_partition) const {
	const auto it = by_partition_.find(p_partition);
	if (it == by_partition_.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::span<const uint8_t> WaveBank::chunk(WaveId p_wave, uint16_t p_chunk_index) const {
	if (p_wave >= wave_chunks_.size() || p_chunk_index >= wave_chunks_[p_wave].size()) {
		return {};
	}
	const uint32_t job = wave_chunks_[p_wave][p_chunk_index];
	if (job == kNotResident) {
		return {};
	}
	return chunk_bytes_[job];
}

bool WaveBank::resident(WaveId p_wave) const {
	if (p_wave >= wave_chunks_.size() || wave_chunks_[p_wave].empty()) {
		return false;
	}
	for (uint32_t job : wave_chunks_[p_wave]) {
		if (job != kNotResident) {
			return true;
		}
	}
	return false;
}

Result<std::shared_ptr<const WaveBank>> WaveBank::from_memory(std::vector<SoundWaveInfo> p_waves, std::vector<std::vector<std::vector<uint8_t>>> p_chunks,
		std::string_view p_language) {
	if (p_chunks.size() != p_waves.size()) {
		return make_error("wave bank: " + std::to_string(p_chunks.size()) + " chunk lists for " + std::to_string(p_waves.size()) + " waves");
	}
	std::shared_ptr<WaveBank> bank(new WaveBank());
	bank->waves_ = std::move(p_waves);
	bank->wave_chunks_.resize(bank->waves_.size());
	bank->stats_.waves = bank->waves_.size();
	uint64_t total = 0;
	for (size_t w = 0; w < bank->waves_.size(); ++w) {
		const SoundWaveInfo &wave = bank->waves_[w];
		if (p_chunks[w].size() != wave.chunks.size()) {
			return make_error("wave bank: " + wave.name + ": chunk count");
		}
		if (!bank->by_partition_.emplace(wave.partition, WaveId(w)).second) {
			return make_error("wave bank: " + wave.name + ": duplicate partition");
		}
		for (size_t c = 0; c < wave.chunks.size(); ++c) {
			if (p_chunks[w][c].size() != wave.chunks[c].size) {
				return make_error("wave bank: " + wave.name + ": chunk " + std::to_string(c) + " size");
			}
			bank->wave_chunks_[w].push_back(uint32_t(bank->chunk_bytes_.size()));
			total += p_chunks[w][c].size();
			bank->chunk_bytes_.push_back(std::move(p_chunks[w][c]));
		}
		++bank->stats_.resident_waves;
		const std::optional<std::pair<uint32_t, uint32_t>> range = playable_variations(wave, p_language);
		if (!range) {
			continue;
		}
		for (uint32_t v = range->first; v < range->first + range->second; ++v) {
			const WaveVariation &var = wave.variations[v];
			if (var.chunk_index >= bank->wave_chunks_[w].size()) {
				return make_error("wave bank: " + wave.name + ": chunk index");
			}
			const std::vector<uint8_t> &bytes = bank->chunk_bytes_[bank->wave_chunks_[w][var.chunk_index]];
			for (size_t s = 0; s < var.segments.size(); ++s) {
				Result<SoundStreamHeader> h = parse_stream_header(bytes, var.segments[s].samples_offset);
				if (!h) {
					return make_error("wave bank: " + wave.name + " segment " + std::to_string(s) + ": " + h.error().message);
				}
			}
		}
	}
	bank->stats_.chunks = bank->chunk_bytes_.size();
	bank->stats_.resident_bytes = total;
	return std::shared_ptr<const WaveBank>(std::move(bank));
}

}
