#pragma once

#include "bf3/core/audio/sound_types.h"
#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::audio {

inline constexpr uint32_t kSoundProgramFormatVersion = 1;

inline constexpr uint32_t kSoundProgramConverterVersion = 1;
inline constexpr const char *kSoundProgramCacheKind = "sound-patch";
inline constexpr const char *kSoundProgramDepsCacheKind = "sound-patch-deps";

inline constexpr size_t kMaxProgramNodes = 4096;
inline constexpr size_t kMaxProgramValues = 65535;
inline constexpr size_t kMaxNodeChildren = 1024;
inline constexpr size_t kMaxProgramVoices = 256;
inline constexpr size_t kMaxVoicePlugins = 64;
inline constexpr size_t kMaxProgramConnections = 4096;
inline constexpr size_t kMaxProgramWaves = 4096;
inline constexpr size_t kMaxCurvePoints = 64;
inline constexpr uint16_t kNoRegister = 0xffff;

struct SoundPort {
	uint16_t reg = kNoRegister;
	float constant = 0.0f;
	bool connected() const { return reg != kNoRegister; }
	bool operator==(const SoundPort &) const = default;
};

struct SoundPluginRef {
	uint16_t voice = kNoRegister;
	uint16_t plugin = 0;
	bool valid() const { return voice != kNoRegister; }
	bool operator==(const SoundPluginRef &) const = default;
};

struct SoundNode {
	SoundNodeType type = SoundNodeType::Count;
	uint8_t option = 0;
	uint8_t flags = 0;
	uint32_t ports_begin = 0;
	uint16_t ports_count = 0;
	uint32_t floats_begin = 0;
	uint16_t floats_count = 0;
	uint32_t children_begin = 0;
	uint16_t children_count = 0;
	uint32_t waves_begin = 0;
	uint16_t waves_count = 0;
	uint32_t plugins_begin = 0;
	uint16_t plugins_count = 0;
	uint16_t output = kNoRegister;
	uint32_t source_index = 0;
	uint32_t aux = 0;
};

enum SoundNodeFlag : uint8_t {
	kFlagLockControlValue = 1 << 0,
	kFlagShuffleSegments = 1 << 1,
	kFlagIsRandom = 1 << 2,
	kFlagRandomStartIndex = 1 << 3,
	kFlagStartAtRandomValue = 1 << 4,
	kFlagWrapValue = 1 << 5,
	kFlagEnableHdr = 1 << 6,
	kFlagAllowExtreme = 1 << 7,
};

enum class MasterBus : uint8_t {
	GameMixMain,
	Reverb,
	GameMixAlt,
	DirectOut,
	Dialog,
	Lfe,
	Music,
	Movies,
	LoadingMusic,
	NarrVoice,
	Unknown,
};

constexpr uint32_t fourcc(const char (&p_text)[5]) {
	return (uint32_t(uint8_t(p_text[0])) << 24) | (uint32_t(uint8_t(p_text[1])) << 16) | (uint32_t(uint8_t(p_text[2])) << 8) |
			uint32_t(uint8_t(p_text[3]));
}

inline constexpr uint32_t kStoredConnectionNone = 0;

struct SoundVoicePlugin {
	uint32_t id = 0;
	uint8_t channels = 0;
	uint16_t connection = kNoRegister;
	uint32_t construct_begin = 0;
	uint16_t construct_count = 0;
	uint32_t attribute_mask = 0;
};

struct SoundVoice {
	int8_t stage = 0;
	uint32_t plugins_begin = 0;
	uint16_t plugins_count = 0;
};

struct SoundConnection {
	bool to_bus = false;
	uint16_t voice = 0;
	MasterBus bus = MasterBus::Unknown;
};

struct SoundPluginLink {
	uint32_t node = 0;
	SoundPluginRef plugin;
	uint8_t attribute = 0;
	bool allow_extreme = false;
	SoundPort port;
};

struct AudioCurvePoint {
	float x = 0.0f, y = 0.0f, k = 0.0f;
};
struct AudioCurve {
	std::vector<AudioCurvePoint> points;
	float evaluate(float p_x) const;
};

enum class OutputKind : uint8_t { Cone, Flat, Direct };

struct SoundOutput {
	uint32_t node = 0;
	OutputKind kind = OutputKind::Cone;
	uint32_t name_hash = 0;
	AudioCurve attenuation;
	float gain_db = 0.0f;
	float min_distance = 0.0f;
	float hf_damping_distance = 0.0f, hf_damping_obstruction = 0.0f, hf_damping_occlusion = 0.0f, hf_damping_angle = 0.0f;
	float reverb_gain_db = -96.0f;
	float pan_size = 0.0f;
	float angle = 0.0f;
	float outside_gain_db = 0.0f;
	float direction[3] = { 0.0f, 0.0f, 1.0f };
	MasterBus main_send = MasterBus::Unknown;
	MasterBus reverb_send = MasterBus::Unknown;
	SoundPluginRef low_pass, pan, main_send_plugin, reverb_send_plugin, vu;
	bool enable_hdr = false;
	OutputTransformSource transform = OutputTransformSource::Sound;
};

struct SoundInput {
	uint32_t name_hash = 0;
	uint16_t reg = kNoRegister;
	float default_value = 0.0f;
	bool event = false;
};

struct SoundMixerInput {
	Guid mixer;
	uint32_t name_hash = 0;
	uint16_t out = kNoRegister;
};

struct SoundWaveRef {
	Guid partition;
	std::string name;
};

struct SoundProgram {
	std::string name;
	Guid partition;

	float loudness_db = 0.0f;
	float radius = 0.0f;
	float master_pitch = 1.0f;
	float doppler_factor = 0.0f;
	float ai_loudness = 0.0f;
	bool is_looping = false;
	uint32_t start_event = 0;
	uint32_t stop_event = 0;

	uint32_t registers = 0;
	std::vector<SoundInput> inputs;
	std::vector<SoundNode> nodes;
	std::vector<SoundNode> child_nodes;
	std::vector<uint32_t> children;
	std::vector<SoundPort> ports;
	std::vector<float> floats;
	std::vector<uint16_t> wave_refs;
	std::vector<SoundPluginRef> plugin_refs;
	std::vector<SoundWaveRef> waves;
	std::vector<SoundMixerInput> mixer_inputs;
	std::vector<SoundOutput> outputs;

	std::vector<SoundVoice> voices;
	std::vector<uint16_t> voice_order;
	std::vector<SoundVoicePlugin> plugins;
	std::vector<float> construct_params;
	std::vector<SoundConnection> connections;
	std::vector<SoundPluginLink> links;

	std::optional<uint16_t> input_register(uint32_t p_name_hash) const;
};

struct CompileOptions {

	bool allow_unimplemented = false;
};

struct CompileStats {
	size_t nodes = 0;
	size_t child_records = 0;
	size_t unknown_types = 0;
	size_t unimplemented_nodes = 0;
	size_t feedback_edges = 0;
	size_t registers = 0;
	size_t voices = 0;
	size_t plugins = 0;
	size_t connections = 0;
	std::vector<std::string> unknown_type_names;
	std::vector<std::string> dependencies;
};

Result<SoundProgram> compile_sound_patch(const EbxResolver &p_db, const EbxPartition &p_partition, const CompileOptions &p_options,
		gameplay::WorkBudget &r_budget, CompileStats *r_stats = nullptr);

Result<std::vector<uint32_t>> order_by_register_dependencies(std::span<const std::vector<uint16_t>> p_reads,
		std::span<const std::vector<uint16_t>> p_writes, uint32_t p_registers, gameplay::WorkBudget &r_budget, size_t &r_feedback_edges);

Status validate_sound_program(const SoundProgram &p_program);
std::vector<uint8_t> encode_sound_program(const SoundProgram &p_program);
Result<SoundProgram> decode_sound_program(std::span<const uint8_t> p_blob);

struct SoundProgramSources {
	const EbxResolver *db = nullptr;
	gameplay::PartitionDigest digest;
};
struct CachedSoundProgram {
	SoundProgram program;
	CachedItem item;
};
Result<CachedSoundProgram> compile_sound_patch_cached(Cache *p_cache, const SoundProgramSources &p_sources, std::string_view p_patch,
		const CompileOptions &p_options);

}
