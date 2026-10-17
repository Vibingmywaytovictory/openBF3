// SoundWaveAsset reader (sound_wave.h). Fields by the partitions' own descriptor names; every array element and
// reference is charged to the WorkBudget before it is read.

#include "bf3/core/audio/sound_wave.h"

#include <algorithm>
#include <string>

namespace bf3::core::audio {

namespace {

Error wave_error(const SoundWaveInfo &p_wave, const std::string &p_what) {
	return make_error("SoundWaveAsset " + (p_wave.name.empty() ? p_wave.partition.to_string() : p_wave.name) + ": " + p_what);
}

int64_t int_field(const EbxStruct &p_s, std::string_view p_field, int64_t p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return p_default;
	}
	return v->as_i64().value_or(p_default);
}

bool bool_field(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v && v->as_bool().value_or(false);
}

float float_field(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_f32().value_or(0.0f) : 0.0f;
}

} // namespace

Result<SoundWaveInfo> read_sound_wave(const EbxResolver &p_db, const EbxInstance &p_wave, gameplay::WorkBudget &r_budget) {
	SoundWaveInfo out;
	out.partition = p_wave.partition().guid();
	out.instance = p_wave.guid();
	if (std::optional<std::string_view> name = p_db.name_of(out.partition)) {
		BF3_TRY(r_budget.charge_bytes(name->size()));
		out.name = std::string(*name);
	}
	BF3_TRY(r_budget.charge_type(p_wave.data()));
	if (!p_wave.is_a("SoundWaveAsset")) {
		return wave_error(out, "primary instance is " + std::string(p_wave.type_name()) + ", not a SoundWaveAsset");
	}
	const EbxStruct data = p_wave.data();
	const int64_t channels = int_field(data, "ChannelCount", 0);
	if (channels < 1 || channels > 8) {
		return wave_error(out, "ChannelCount " + std::to_string(channels));
	}
	out.channels = uint8_t(channels);
	out.seekable = bool_field(data, "Seekable");
	out.voice_priority = uint8_t(std::clamp<int64_t>(int_field(data, "VoicePriority", 0), 0, 255));
	if (std::optional<EbxValue> sel = data.get("Selection")) {
		out.selection = std::string(sel->enum_name().value_or(""));
	}
	if (std::optional<EbxValue> pool = data.get("StreamPool"); pool && pool->as_ref() && !pool->as_ref()->is_null()) {
		BF3_TRY(r_budget.charge(1));
		if (std::optional<EbxInstance> target = p_db.resolve(p_wave.partition(), *pool->as_ref())) {
			out.stream_pool = std::string(p_db.name_of(target->partition().guid()).value_or("?"));
		} else {
			out.stream_pool = "?";
		}
	}

	// Chunks[] {ChunkId, ChunkSize}
	const std::optional<EbxValue> chunks = data.get("Chunks");
	if (!chunks || !chunks->as_array()) {
		return wave_error(out, "no Chunks array");
	}
	if (chunks->as_array()->size() > kMaxWaveChunks) {
		return wave_error(out, std::to_string(chunks->as_array()->size()) + " chunks (at most " + std::to_string(kMaxWaveChunks) + ")");
	}
	for (EbxValue c : *chunks->as_array()) {
		BF3_TRY(r_budget.charge(1));
		const std::optional<EbxStruct> s = c.as_struct();
		if (!s) {
			return wave_error(out, "Chunks element is not a struct");
		}
		WaveChunkRef ref;
		const std::optional<EbxValue> id = s->get("ChunkId");
		if (!id || !id->as_guid()) {
			return wave_error(out, "chunk without ChunkId");
		}
		ref.id = *id->as_guid();
		const int64_t size = int_field(*s, "ChunkSize", -1);
		if (size < 0 || size > int64_t(UINT32_MAX)) {
			return wave_error(out, "ChunkSize " + std::to_string(size));
		}
		ref.size = uint32_t(size);
		out.chunks.push_back(ref);
	}

	// Variations[] -> SoundWaveVariation (indices matter: Localization ranges index this array, so an unresolved
	// entry is an error, never skipped).
	const std::optional<EbxValue> variations = data.get("Variations");
	if (!variations || !variations->as_array()) {
		return wave_error(out, "no Variations array");
	}
	const EbxArray vars = *variations->as_array();
	if (vars.size() > kMaxWaveVariations) {
		return wave_error(out, std::to_string(vars.size()) + " variations (at most " + std::to_string(kMaxWaveVariations) + ")");
	}
	out.variations.reserve(vars.size());
	for (size_t i = 0; i < vars.size(); ++i) {
		BF3_TRY(r_budget.charge(1));
		const std::optional<EbxValue> v = vars.get(i);
		const std::optional<EbxRef> ref = v ? v->as_ref() : std::nullopt;
		std::optional<EbxInstance> inst = ref && !ref->is_null() ? p_db.resolve(p_wave.partition(), *ref) : std::nullopt;
		if (!inst) {
			return wave_error(out, "variation " + std::to_string(i) + " does not resolve");
		}
		BF3_TRY(r_budget.charge_type(inst->data()));
		const EbxStruct vd = inst->data();
		WaveVariation wv;
		const int64_t chunk_index = int_field(vd, "ChunkIndex", -1);
		if (chunk_index < 0 || size_t(chunk_index) >= out.chunks.size()) {
			return wave_error(out, "variation " + std::to_string(i) + ": ChunkIndex " + std::to_string(chunk_index) + " of " + std::to_string(out.chunks.size()) +
					" chunks");
		}
		wv.chunk_index = uint16_t(chunk_index);
		wv.first_loop_segment = int16_t(std::clamp<int64_t>(int_field(vd, "FirstLoopSegmentIndex", -1), -1, 32767));
		wv.last_loop_segment = int16_t(std::clamp<int64_t>(int_field(vd, "LastLoopSegmentIndex", -1), -1, 32767));
		const std::optional<EbxValue> segs = vd.get("Segments");
		if (!segs || !segs->as_array()) {
			return wave_error(out, "variation " + std::to_string(i) + " has no Segments array");
		}
		if (segs->as_array()->size() > kMaxVariationSegments) {
			return wave_error(out, "variation " + std::to_string(i) + ": " + std::to_string(segs->as_array()->size()) + " segments (at most " +
					std::to_string(kMaxVariationSegments) + ")");
		}
		wv.segments.reserve(segs->as_array()->size());
		for (EbxValue sv : *segs->as_array()) {
			BF3_TRY(r_budget.charge(1));
			const std::optional<EbxStruct> s = sv.as_struct();
			if (!s) {
				return wave_error(out, "segment is not a struct");
			}
			WaveSegment seg;
			const int64_t offset = int_field(*s, "SamplesOffset", -1);
			if (offset < 0 || offset >= int64_t(out.chunks[wv.chunk_index].size)) {
				return wave_error(out, "variation " + std::to_string(i) + ": SamplesOffset " + std::to_string(offset) + " outside chunk " +
						std::to_string(wv.chunk_index) + " (" + std::to_string(out.chunks[wv.chunk_index].size) + " bytes)");
			}
			seg.samples_offset = uint32_t(offset);
			seg.length_seconds = float_field(*s, "SegmentLength");
			wv.segments.push_back(seg);
		}
		out.variations.push_back(std::move(wv));
	}

	// Localization[] {Language -> AudioLanguage, FirstVariationIndex, VariationCount}
	if (std::optional<EbxValue> loc = data.get("Localization"); loc && loc->as_array()) {
		if (loc->as_array()->size() > 64) {
			return wave_error(out, std::to_string(loc->as_array()->size()) + " localisation entries");
		}
		for (EbxValue lv : *loc->as_array()) {
			BF3_TRY(r_budget.charge(1));
			const std::optional<EbxStruct> s = lv.as_struct();
			if (!s) {
				return wave_error(out, "Localization element is not a struct");
			}
			WaveLocalization wl;
			const int64_t first = int_field(*s, "FirstVariationIndex", -1);
			const int64_t count = int_field(*s, "VariationCount", -1);
			if (first < 0 || count < 0 || uint64_t(first) + uint64_t(count) > out.variations.size()) {
				return wave_error(out, "localisation range [" + std::to_string(first) + ", +" + std::to_string(count) + ") outside " +
						std::to_string(out.variations.size()) + " variations");
			}
			wl.first_variation = uint32_t(first);
			wl.variation_count = uint32_t(count);
			if (std::optional<EbxValue> lang = s->get("Language"); lang && lang->as_ref() && !lang->as_ref()->is_null()) {
				BF3_TRY(r_budget.charge(1));
				if (std::optional<EbxInstance> li = p_db.resolve(p_wave.partition(), *lang->as_ref())) {
					BF3_TRY(r_budget.charge_type(li->data()));
					if (std::optional<EbxValue> n = li->get("Name"); n && n->as_str()) {
						BF3_TRY(r_budget.charge_bytes(n->as_str()->size()));
						wl.language = std::string(*n->as_str());
					}
				}
			}
			out.localization.push_back(std::move(wl));
		}
	}
	BF3_TRY(r_budget.status());
	return out;
}

std::optional<std::pair<uint32_t, uint32_t>> playable_variations(const SoundWaveInfo &p_wave, std::string_view p_language) {
	if (!p_wave.localised()) {
		return std::pair<uint32_t, uint32_t>(0, uint32_t(p_wave.variations.size()));
	}
	for (const WaveLocalization &l : p_wave.localization) {
		if (l.language == p_language) {
			return std::pair<uint32_t, uint32_t>(l.first_variation, l.variation_count);
		}
	}
	return std::nullopt;
}

} // namespace bf3::core::audio
