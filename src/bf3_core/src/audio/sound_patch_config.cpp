#include "bf3/core/audio/sound_patch_config.h"

#include <cmath>
#include <optional>

namespace bf3::core::audio {

namespace {

Error config_error(const SoundPatchConfiguration &p_config, const std::string &p_what) {
	return make_error("sound patch configuration " + p_config.name + ": " + p_what);
}

}

Result<SoundPatchConfiguration> read_sound_patch_configuration(
		const EbxResolver &p_db, const EbxPartition &p_partition) {
	const EbxInstance primary = p_partition.primary();
	SoundPatchConfiguration out;
	out.partition = p_partition.guid();
	out.name = std::string(p_db.name_of(p_partition.guid()).value_or(""));
	if (!primary.is_a("SoundPatchConfigurationAsset")) {
		return config_error(out, "primary instance is " + std::string(primary.type_name()));
	}
	const std::optional<EbxValue> loudness = primary.get("Loudness");
	out.loudness_db = loudness ? loudness->as_f32().value_or(0.0f) : 0.0f;
	if (!std::isfinite(out.loudness_db)) {
		return config_error(out, "non-finite Loudness");
	}
	const std::optional<EbxInstance> patch = p_db.follow(primary.data(), "Sound");
	if (!patch || !patch->is_a("SoundPatchAsset")) {
		return config_error(out, "Sound is not a SoundPatchAsset");
	}
	out.patch = patch->partition().guid();
	out.patch_name = std::string(p_db.name_of(out.patch).value_or(""));
	for (const EbxInstance &entry : p_db.follow_all(primary.data(), "Entries")) {
		const std::optional<EbxValue> hash = entry.get("NameHash");
		const uint32_t name_hash = hash ? uint32_t(hash->as_i64().value_or(0)) : 0u;
		if (entry.is_a("SoundPatchConfigurationAssetEntry")) {
			const std::optional<EbxInstance> wave = p_db.follow(entry.data(), "Value");
			if (!wave || !wave->is_a("SoundWaveAsset")) {
				return config_error(out, "asset entry " + std::to_string(name_hash) + " is not a SoundWaveAsset");
			}
			SoundPatchConfigAsset a;
			a.name_hash = name_hash;
			a.wave.partition = wave->partition().guid();
			a.wave.name = std::string(p_db.name_of(a.wave.partition).value_or(""));
			out.assets.push_back(std::move(a));
		} else if (entry.is_a("SoundPatchConfigurationParameterEntry")) {
			const std::optional<EbxValue> value = entry.get("Value");
			const float v = value ? value->as_f32().value_or(0.0f) : 0.0f;
			if (!std::isfinite(v)) {
				return config_error(out, "parameter entry " + std::to_string(name_hash) + " is not finite");
			}
			out.parameters.push_back(SoundPatchConfigParameter{ name_hash, v });
		} else {
			return config_error(out, "entry of type " + std::string(entry.type_name()));
		}
	}
	return out;
}

Result<SoundProgram> configure_sound_program(SoundProgram p_patch, const SoundPatchConfiguration &p_config) {
	if (p_patch.partition != p_config.patch) {
		return config_error(p_config, "configures " + p_config.patch_name + ", not " + p_patch.name);
	}
	const auto input_of = [&](uint32_t p_hash) -> SoundInput * {
		for (SoundInput &in : p_patch.inputs) {
			if (in.name_hash == p_hash && !in.event) {
				return &in;
			}
		}
		return nullptr;
	};
	for (const SoundPatchConfigAsset &a : p_config.assets) {
		SoundInput *in = input_of(a.name_hash);
		if (in == nullptr) {
			return config_error(p_config, "no input parameter " + std::to_string(a.name_hash) + " for its wave");
		}
		size_t index = 0;
		while (index < p_patch.waves.size() && p_patch.waves[index].partition != a.wave.partition) {
			++index;
		}
		if (index == p_patch.waves.size()) {
			if (p_patch.waves.size() >= kMaxProgramWaves) {
				return config_error(p_config, "more than " + std::to_string(kMaxProgramWaves) + " waves");
			}
			p_patch.waves.push_back(a.wave);
		}
		in->default_value = float(index + 1);
	}
	for (const SoundPatchConfigParameter &prm : p_config.parameters) {
		SoundInput *in = input_of(prm.name_hash);
		if (in == nullptr) {
			return config_error(p_config, "no input parameter " + std::to_string(prm.name_hash));
		}
		in->default_value = prm.value;
	}
	p_patch.name = p_config.name;
	p_patch.loudness_db = p_config.loudness_db;
	BF3_TRY(validate_sound_program(p_patch));
	return p_patch;
}

}
