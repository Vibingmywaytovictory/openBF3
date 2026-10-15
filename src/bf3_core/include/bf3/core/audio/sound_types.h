#pragma once
// The sound-graph node type table (M2a audio A2; docs/design/m2a-audio.md §6.1). Implemented now (sound_types.cpp):
// it is data, and it pins the census gate's type sets.
//
// Census over @all-mp [verified 2026-10-15, scratch census on this tree's bf3_core, design §4.3]: 834
// SoundPatchAsset partitions (833 under sound/ plus levels/mp_012/sound/sfx_poppertarget_raise) hold 32,117
// instances of 46 node types; with the master patch and the 19 MixerAssets, 32,221 instances of 58 types. The 164
// handheld weapon patches use exactly the 32 types flagged `a2`. Child records (crossfader groups, receive entries,
// ...) are not nodes: they are listed with role Child and belong to the node whose array names them.
//
// Field names are the EBX descriptors' (PascalCase). `ports` are AudioGraphNodePort fields {UnconnectedValue,
// ValueIndex, IsConnected} in the order the evaluator reads them; for the A2 types they are taken from the 164
// weapon patches (every field of every instance, design §4.3); the types A2 does not implement list no ports yet
// (A6/A7 fill them in).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf3::core::audio {

enum class SoundNodeType : uint8_t {
	// The 32 types of the handheld weapon patches (A2).
	Adsr,
	BandPassIir2,
	CameraShake,
	Condition,
	ConeOutput,
	Crossfader2,
	DeMux,
	Delay,
	DirectOutput,
	EventGate,
	FlatOutput,
	Gain,
	HighPassIir2,
	Lfo,
	LogicalExpression,
	LowPassIir2,
	MixerReceive,
	MultiCrossfader,
	NumberGenerator,
	PadRumble,
	ParameterFilterLinear,
	PeakingIir2,
	Physics,
	Receive,
	Sampler,
	ScaleClamp,
	SimpleTransform,
	SimpleValueSelector,
	Switcher,
	Timer,
	ValueSelector,
	WaveSwitcher,
	// The other 14 patch types (A5-A7; plan §3.10 phasing, gap audio §11.1).
	Delta,
	DialogSampler,
	DistortionClip,
	DivisibleLoopPlayer,
	EventSwitcher,
	FrequencyShiftSsb,
	HighShelfIir2,
	Limiter,
	LowShelfIir2,
	Orientation,
	ParameterFilterLp,
	Route,
	Send,
	SequenceGenerator,
	// Master patch and mixer graphs only (A3, A4, A7).
	Bus,
	Compressor,
	Dac,
	IrReverb,
	IrReverbController,
	MasterUnit,
	MatrixPanner,
	MinMaxValueSelector,
	MixerInput,
	MixerOutput,
	MixerValue,
	UserMusicArbiter,
	// Child records.
	ConditionGroup,
	DeMuxOutput,
	LogicalExpressionEntry,
	MixerReceiveEntry,
	MultiCrossfaderGroup,
	ReceiveEntry,
	SwitcherEntry,
	ValueSelectorEntry,
	Count,
};

enum class SoundNodeRole : uint8_t {
	Patch, // a node of SoundPatchAsset graphs
	MasterOrMixer, // only in sound/master and MixerAsset graphs
	Child, // a record inside a node's array
};

struct SoundNodeTypeInfo {
	SoundNodeType type;
	std::string_view ebx_name; // e.g. "SamplerNodeData", "MultiCrossfaderGroup"
	SoundNodeRole role;
	bool a2; // evaluated by the A2 interpreter (the 32 weapon types and the children they use)
	bool audio_rate; // has a SoundGraphPluginRef (Plugin, the outputs' *Plugin) or Sampler Plugins[]: its audio path runs
	                 // as plugins of compiled voices
	std::span<const std::string_view> ports;
	std::span<const std::string_view> floats; // plain float fields the evaluator reads
	std::string_view option; // the enum field the evaluator reads ("" none), e.g. "Operation", "Loop", "FadeType"
	std::string_view children; // the array of child records ("" none), e.g. "CrossfaderGroups"
};

inline constexpr size_t kPatchNodeTypes = 46;
inline constexpr size_t kA2NodeTypes = 32;
inline constexpr size_t kAllNodeTypes = 58; // patch + master/mixer types (children excluded)

// One entry per SoundNodeType below Count, in enum order.
std::span<const SoundNodeTypeInfo> sound_node_types();
const SoundNodeTypeInfo &sound_node_type_info(SoundNodeType p_type); // p_type < Count
// By EBX type name (exact, case-sensitive); nullopt for any other type.
std::optional<SoundNodeType> sound_node_type(std::string_view p_ebx_name);

// The enums the evaluator reads, by enumerator name (the compiler maps names, never stored values, so a reordered
// enum in another build still compiles right). Enumerators and their stored values as the partitions' own enum
// tables list them [verified 2026-10-15, scratch `au_census enums` on the M16A3, M136 and 9K34 patches]. Weapons use
// Multiply 2,526, Add 492, Sine 9, Power 1, Subtract 1 (design §4.3).
enum class SimpleTransformOp : uint8_t {
	Add, // 0
	Subtract,
	Multiply,
	Divide,
	Sine,
	Cosine,
	ArcSine,
	ArcCosine,
	Modulus,
	Round,
	Floor, // 10
	Ceiling,
	Power,
	SquareRoot,
	Absolute,
	Exp,
	Log,
	Log10,
	Tangent,
	ArcTangent, // 19
	Unknown,
};
SimpleTransformOp simple_transform_op(std::string_view p_enumerator); // "SimpleTransformOperation_Multiply" -> Multiply

enum class ConditionOp : uint8_t { And, Or, XOr, Equal, Less, Greater, LessOrEqual, GreaterOrEqual, Unknown }; // ConditionType_*
enum class LogicalOp : uint8_t { And, Or, Nand, Nor, Unknown }; // LogicalExpressionOperator_*
enum class FaderType : uint8_t { Linear, Cosine, Unknown }; // FaderType_*
enum class SamplerLoop : uint8_t { None, Forward, Release, InstantRelease, Unknown }; // LtNone, LtForward, LtRelease, LtInstantRelease
enum class TimerMode : uint8_t { Repeating, SingleShot, Unknown }; // TimerMode_*
enum class NumberGeneratorMode : uint8_t { RandomUniform, Unknown }; // the only enumerator
enum class AngleUnit : uint8_t { Radians, Degrees, Unknown };
enum class OutputTransformSource : uint8_t { Sound, Listener, Output, Unknown }; // weapons: Sound only

} // namespace bf3::core::audio
