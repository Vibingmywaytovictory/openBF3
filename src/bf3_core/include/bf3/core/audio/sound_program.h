#pragma once
// A SoundPatchAsset compiled into a control program and an audio plan (M2a audio A2; plan D15, §3.10; gap audio
// §1.2-§1.3, §11.1; docs/design/m2a-audio.md §6). Godot-free, immutable once built, shared by every playing instance
// of the patch (bf3_audio PatchInstance holds only the per-instance state).
//
// What the data gives [verified on the M16A3 patch, design §4.4]:
//   SoundPatchAsset { Graph -> SoundGraphData, OutputNodes[], DefaultStartEvent, DefaultStopEvent, Loudness, Radius,
//     IsLooping, DopplerFactor, MasterPitch, MixGroup, Scope, AILoudness }
//   SoundGraphData { Nodes[], ValueCount, InputParameters[], InputEvents[], Public*[], Info: SoundGraphInfo }
//   AudioGraphParameter / AudioGraphEvent { NameHash (djb2-xor of the name, ebx_name_hash), ValueIndex,
//     DefaultValue }
//   SoundGraphInfo { Voices[] {ProcessingStage, Plugins[] {Id FourCC, OutputChannelCount, ConnectionIndex,
//     ConstructParamsIndex, ConstructParamCount, EnableAttributeReadMask}}, Connections[] {ConnectionType
//     Voice|Bus, VoiceIndex, Bus -> sound/master BusNodeData}, ConstructParams[] {Index, Value},
//     LinkedPluginAttributes[] {Node, Plugin {VoiceIndex, PluginIndex}, AttributeIndex, Port, AllowExtremeValues},
//     PluginCount, PluginsParamCount }
// The control graph is a dataflow over one float register file: a port reads register ValueIndex when IsConnected,
// else the constant UnconnectedValue; outputs write registers; events are registers that pulse for one control step.
// LinkedPluginAttributes may name registers at or above ValueCount (the M16A3 uses 131-134 with ValueCount 130),
// so the register file is max(ValueCount, highest index named + 1). Audio-rate nodes (Sampler, Gain, the biquads,
// Delay, the outputs) bind to plugins of compiled voices; their "In"/"Out" ports are register 0 and carry no value.
//
// Evaluation order: nodes are topologically sorted by register dependencies (a node that reads register r runs
// after every node that writes r); a dependency cycle is broken at the edge that closes it (that read sees the
// previous step's value) and counted in CompileStats::feedback_edges. Retail weapon patches have none [to be
// verified by the census gate G4]. The sort is iterative (no recursion) and O(nodes + port reads).

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
// Bump when compile_sound_patch's output changes for the same partitions.
inline constexpr uint32_t kSoundProgramConverterVersion = 1;
inline constexpr const char *kSoundProgramCacheKind = "sound-patch";
inline constexpr const char *kSoundProgramDepsCacheKind = "sound-patch-deps";

// Structural caps (hostile input, design §11). Retail maxima over @all-mp in brackets [verified 2026-10-15].
inline constexpr size_t kMaxProgramNodes = 4096; // [242 nodes in one patch]
inline constexpr size_t kMaxProgramValues = 65535; // registers [ValueCount 266]
inline constexpr size_t kMaxNodeChildren = 1024; // records in one node array
inline constexpr size_t kMaxProgramVoices = 256; // [38]
inline constexpr size_t kMaxVoicePlugins = 64;
inline constexpr size_t kMaxProgramConnections = 4096;
inline constexpr size_t kMaxProgramWaves = 4096; // distinct waves named by one patch
inline constexpr size_t kMaxCurvePoints = 64;
inline constexpr uint16_t kNoRegister = 0xffff;

// A port: a register, or a constant when unconnected.
struct SoundPort {
	uint16_t reg = kNoRegister;
	float constant = 0.0f;
	bool connected() const { return reg != kNoRegister; }
	bool operator==(const SoundPort &) const = default;
};

// SoundGraphPluginRef {VoiceIndex, PluginIndex}; voice = kNoRegister when IsValid is false.
struct SoundPluginRef {
	uint16_t voice = kNoRegister;
	uint16_t plugin = 0;
	bool valid() const { return voice != kNoRegister; }
	bool operator==(const SoundPluginRef &) const = default;
};

// One node or child record. Variable-length parts are ranges into the program's pools, in the field order of
// sound_node_type_info(type) (ports, floats) and of the data (children, waves, plugin slots).
struct SoundNode {
	SoundNodeType type = SoundNodeType::Count;
	uint8_t option = 0; // the type's enum field as the matching enum class (SimpleTransformOp, SamplerLoop, ...)
	uint8_t flags = 0; // bool fields, see SoundNodeFlag
	uint32_t ports_begin = 0;
	uint16_t ports_count = 0;
	uint32_t floats_begin = 0; // plain floats, then (SimpleValueSelector) OutValues[]
	uint16_t floats_count = 0;
	uint32_t children_begin = 0; // into SoundProgram::children (indices of SoundNode records of a Child role)
	uint16_t children_count = 0;
	uint32_t waves_begin = 0; // into SoundProgram::wave_refs (Sampler.Wave, WaveSwitcher.Waves[])
	uint16_t waves_count = 0;
	uint32_t plugins_begin = 0; // into SoundProgram::plugin_refs (Sampler.Plugins[] voice slots x 4, Plugin, outputs' 5)
	uint16_t plugins_count = 0;
	uint16_t output = kNoRegister; // index into SoundProgram::outputs for Cone/Flat/DirectOutput
	uint32_t source_index = 0; // position in SoundGraphData.Nodes (children: in the parent's array)
};

enum SoundNodeFlag : uint8_t {
	kFlagLockControlValue = 1 << 0, // MultiCrossfader
	kFlagShuffleSegments = 1 << 1, // Sampler
	kFlagIsRandom = 1 << 2, // WaveSwitcher
	kFlagRandomStartIndex = 1 << 3, // WaveSwitcher
	kFlagStartAtRandomValue = 1 << 4, // Lfo
	kFlagWrapValue = 1 << 5, // DeMux
	kFlagEnableHdr = 1 << 6, // outputs (A3 reads it)
	kFlagAllowExtreme = 1 << 7, // LinkedPluginAttribute
};

enum class MasterBus : uint8_t {
	GameMixMain, // b735bfcd, 4 ch
	Reverb, // d72517d3, 2 ch
	GameMixAlt, // 4 ch
	DirectOut, // 6 ch
	Dialog, // 1 ch
	Lfe, // fcef9739, 1 ch
	Music, // 4 ch
	Movies, // 6 ch
	LoadingMusic, // 2 ch
	NarrVoice, // 4 ch
	Unknown,
}; // by BusNodeData.BusName of sound/master [verified, design §4.4]

// Plugin FourCCs as stored (big-endian ASCII in a u32: 'SnP1' = 0x536E5031).
constexpr uint32_t fourcc(const char (&p_text)[5]) {
	return (uint32_t(uint8_t(p_text[0])) << 24) | (uint32_t(uint8_t(p_text[1])) << 16) | (uint32_t(uint8_t(p_text[2])) << 8) |
			uint32_t(uint8_t(p_text[3]));
}

struct SoundVoicePlugin {
	uint32_t id = 0; // FourCC
	uint8_t channels = 0; // OutputChannelCount
	uint16_t connection = kNoRegister; // Sen0: index into connections
	uint32_t construct_begin = 0; // into construct_params
	uint16_t construct_count = 0;
	uint32_t attribute_mask = 0; // EnableAttributeReadMask
};

struct SoundVoice {
	int8_t stage = 0; // ProcessingStage: 0 sampler voices, then submix stages (the plan renders by ascending stage)
	uint32_t plugins_begin = 0;
	uint16_t plugins_count = 0;
};

struct SoundConnection {
	bool to_bus = false;
	uint16_t voice = 0; // to_bus false: the voice this send feeds (its Sub0 input)
	MasterBus bus = MasterBus::Unknown;
};

// LinkedPluginAttributes: a port that drives a plugin attribute (filter frequency, Q, gain, Gai0 amplitude).
struct SoundPluginLink {
	uint32_t node = 0; // index into nodes
	SoundPluginRef plugin;
	uint8_t attribute = 0; // AttributeIndex
	bool allow_extreme = false;
	SoundPort port;
};

// AudioCurve {CurveType (AudioCurveType_Spline only), Points[] {X, Y, K}}. Evaluated as a cubic Hermite through the
// points with slopes K (dY/dX), clamped to the end points' Y outside [X0, Xn] [inferred, design §6.3; the oracle
// checks the curve passes through every point].
struct AudioCurvePoint {
	float x = 0.0f, y = 0.0f, k = 0.0f;
};
struct AudioCurve {
	std::vector<AudioCurvePoint> points; // sorted by x
	float evaluate(float p_x) const;
};

enum class OutputKind : uint8_t { Cone, Flat, Direct };

struct SoundOutput {
	uint32_t node = 0;
	OutputKind kind = OutputKind::Cone;
	uint32_t name_hash = 0; // OutputNameHash ("Cone", "Flat", "Handling", "LFE", ...)
	AudioCurve attenuation; // dB over distance (X in metres [inferred], design §6.3)
	float gain_db = 0.0f; // Gain
	float min_distance = 0.0f; // MinDistance
	float hf_damping_distance = 0.0f, hf_damping_obstruction = 0.0f, hf_damping_occlusion = 0.0f, hf_damping_angle = 0.0f;
	float reverb_gain_db = -96.0f; // ReverbGain (Cone/Flat); the Reverb bus is dropped until A4
	float pan_size = 0.0f; // PanSize (Cone)
	float angle = 0.0f; // Angle (Flat)
	float outside_gain_db = 0.0f; // OutsideGain (Cone); InnerAngle/OuterAngle are ports of the node
	float direction[3] = { 0.0f, 0.0f, 1.0f }; // Cone Direction
	MasterBus main_send = MasterBus::Unknown;
	MasterBus reverb_send = MasterBus::Unknown;
	SoundPluginRef low_pass, pan, main_send_plugin, reverb_send_plugin, vu;
	bool enable_hdr = false;
	OutputTransformSource transform = OutputTransformSource::Sound;
};

// A game-facing input: AudioGraphParameter (value) or AudioGraphEvent (pulse). Bound by NameHash.
struct SoundInput {
	uint32_t name_hash = 0;
	uint16_t reg = kNoRegister; // the AudioGraphParameter/Event's own ValueIndex
	float default_value = 0.0f;
	bool event = false;
};

// ReceiveEntry {Source -> parameter/event, Out}: a Receive node copies the input's register into Out. Kept as
// children of the Receive node; `inputs` indexes them for the game side.

// MixerReceiveEntry {Mixer -> MixerAsset, Source -> its AudioGraphParameter, Out}: a global mixer output (AreaType).
struct SoundMixerInput {
	Guid mixer; // MixerAsset partition (sound/mixers/areatype_mixer)
	uint32_t name_hash = 0; // the mixer parameter's NameHash (AreaType = ebx_name_hash("AreaType") = 3772263626)
	uint16_t out = kNoRegister;
};

// A wave the program can play: the SoundWaveAsset partition (bound to a WaveBank id at instantiation).
struct SoundWaveRef {
	Guid partition;
	std::string name;
};

struct SoundProgram {
	std::string name; // lowercase partition name
	Guid partition;
	// SoundPatchAsset
	float loudness_db = 0.0f;
	float radius = 0.0f;
	float master_pitch = 1.0f;
	float doppler_factor = 0.0f;
	float ai_loudness = 0.0f;
	bool is_looping = false;
	uint32_t start_event = 0; // NameHash of DefaultStartEvent (0 = none)
	uint32_t stop_event = 0; // NameHash of DefaultStopEvent
	// Control program
	uint32_t registers = 0;
	std::vector<SoundInput> inputs; // parameters then events, data order
	std::vector<SoundNode> nodes; // evaluation order (topological), Patch role only
	std::vector<SoundNode> child_nodes; // child records; SoundNode::children_begin indexes `children`
	std::vector<uint32_t> children; // indices into child_nodes
	std::vector<SoundPort> ports;
	std::vector<float> floats;
	std::vector<uint16_t> wave_refs; // indices into waves (kNoRegister = null wave)
	std::vector<SoundPluginRef> plugin_refs;
	std::vector<SoundWaveRef> waves;
	std::vector<SoundMixerInput> mixer_inputs;
	std::vector<SoundOutput> outputs;
	// Audio plan
	std::vector<SoundVoice> voices; // data order (VoiceIndex)
	std::vector<uint16_t> voice_order; // render order: ascending stage, then index
	std::vector<SoundVoicePlugin> plugins;
	std::vector<float> construct_params; // Value by ConstructParams order (Pn21: 45, 135, 2 on the M16A3)
	std::vector<SoundConnection> connections;
	std::vector<SoundPluginLink> links;

	std::optional<uint16_t> input_register(uint32_t p_name_hash) const;
};

struct CompileOptions {
	// A2: the 32 weapon types are evaluated; any other type fails the compile unless this is set, in which case
	// the node is kept as a no-op that writes 0 to its outputs (census gate G4 compiles every patch this way).
	bool allow_unimplemented = false;
};

struct CompileStats {
	size_t nodes = 0;
	size_t child_records = 0;
	size_t unknown_types = 0; // a node type outside the table (an error unless allow_unimplemented)
	size_t unimplemented_nodes = 0; // known, not a2
	size_t feedback_edges = 0;
	size_t registers = 0;
	size_t voices = 0;
	size_t plugins = 0;
	std::vector<std::string> unknown_type_names;
	std::vector<std::string> dependencies; // partitions read besides the patch (master, mixers, waves by GUID only)
};

// Compiles the primary SoundPatchAsset (or SoundMasterPatchAsset) of p_partition. Reads the patch partition, the
// partitions its references name for bus names (sound/master) and mixer parameter hashes (MixerAssets); waves are
// referenced by GUID, not read. Every array element, reference and node is charged to r_budget before it is read.
Result<SoundProgram> compile_sound_patch(const EbxResolver &p_db, const EbxPartition &p_partition, const CompileOptions &p_options,
		gameplay::WorkBudget &r_budget, CompileStats *r_stats = nullptr);

// Blob "OBF3SNDP" (kSoundProgramFormatVersion): every field in declaration order, little endian, vectors as u64
// count + elements, strings as u32 length + bytes (<= 64 KiB). decode_sound_program checks every count against the
// bytes left, every register < registers, every range inside its pool, every plugin and connection reference in
// range, the node order topological, and refuses trailing bytes, so an accepted program can be evaluated without
// further checks. encode(decode(b)) == b.
std::vector<uint8_t> encode_sound_program(const SoundProgram &p_program);
Result<SoundProgram> decode_sound_program(std::span<const uint8_t> p_blob);

// Cache (design §8), the WeaponDef scheme (gameplay/weapon_blob.h): "sound-patch-deps" keyed by the patch
// partition's SHA1 -> the sorted list of partitions the compile read; "sound-patch" keyed by their SHA1s (zero for
// an absent one) and the list's SHA-256 -> the blob.
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

} // namespace bf3::core::audio
