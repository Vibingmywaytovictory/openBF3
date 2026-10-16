// Loading helpers (audio_load.h).

#include "bf3/audio/audio_load.h"

#include "bf3/core/gameplay/work_budget.h"

#include <set>

namespace bf3::audio {

namespace ca = core::audio;

core::Result<std::shared_ptr<const ca::SoundProgram>> compile_patch(const core::EbxResolver &p_db, std::string_view p_patch, const ca::CompileOptions &p_options) {
	const core::EbxPartition *partition = p_db.partition_by_name(p_patch);
	if (partition == nullptr) {
		return core::make_error("sound patch " + std::string(p_patch) + ": no such partition");
	}
	core::gameplay::WorkBudget budget;
	core::Result<ca::SoundProgram> prog = ca::compile_sound_patch(p_db, *partition, p_options, budget);
	if (!prog) {
		return prog.error();
	}
	return std::shared_ptr<const ca::SoundProgram>(std::make_shared<ca::SoundProgram>(std::move(*prog)));
}

core::Result<std::vector<ca::SoundWaveInfo>> read_level_waves(const core::EbxDb &p_db) {
	std::vector<ca::SoundWaveInfo> out;
	for (size_t i = 0; i < p_db.size(); ++i) {
		const core::EbxPartition &p = p_db.partition_at(i);
		const core::EbxInstance primary = p.primary();
		if (primary.type_name() != "SoundWaveAsset") {
			continue;
		}
		core::gameplay::WorkBudget budget;
		core::Result<ca::SoundWaveInfo> wave = ca::read_sound_wave(p_db, primary, budget);
		if (!wave) {
			return core::make_error(p_db.name_at(i) + ": " + wave.error().message);
		}
		out.push_back(std::move(*wave));
	}
	return out;
}

core::Result<std::vector<ca::SoundWaveInfo>> read_program_waves(const core::EbxResolver &p_db, std::span<const std::shared_ptr<const ca::SoundProgram>> p_programs,
		size_t *r_missing) {
	std::vector<ca::SoundWaveInfo> out;
	std::set<core::Guid> seen;
	size_t missing = 0;
	for (const std::shared_ptr<const ca::SoundProgram> &prog : p_programs) {
		if (!prog) {
			continue;
		}
		for (const ca::SoundWaveRef &w : prog->waves) {
			if (!seen.insert(w.partition).second) {
				continue;
			}
			const core::EbxPartition *part = p_db.partition(w.partition);
			if (part == nullptr) {
				++missing;
				continue;
			}
			core::gameplay::WorkBudget budget;
			core::Result<ca::SoundWaveInfo> wave = ca::read_sound_wave(p_db, part->primary(), budget);
			if (!wave) {
				return core::make_error(w.name + ": " + wave.error().message);
			}
			out.push_back(std::move(*wave));
		}
	}
	if (r_missing) {
		*r_missing = missing;
	}
	return out;
}

} // namespace bf3::audio
