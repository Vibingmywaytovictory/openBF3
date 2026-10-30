#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf3::core::audio {

enum class SoundNodeType : uint8_t {

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
	Patch,
	MasterOrMixer,
	Child,
};

struct SoundNodeTypeInfo {
	SoundNodeType type;
	std::string_view ebx_name;
	SoundNodeRole role;
	bool a2;
	bool audio_rate;

	std::span<const std::string_view> ports;
	std::span<const std::string_view> floats;
	std::string_view option;
	std::string_view children;
};

inline constexpr size_t kPatchNodeTypes = 46;
inline constexpr size_t kA2NodeTypes = 32;
inline constexpr size_t kAllNodeTypes = 58;

std::span<const SoundNodeTypeInfo> sound_node_types();
const SoundNodeTypeInfo &sound_node_type_info(SoundNodeType p_type);

std::optional<SoundNodeType> sound_node_type(std::string_view p_ebx_name);

enum class SimpleTransformOp : uint8_t {
	Add,
	Subtract,
	Multiply,
	Divide,
	Sine,
	Cosine,
	ArcSine,
	ArcCosine,
	Modulus,
	Round,
	Floor,
	Ceiling,
	Power,
	SquareRoot,
	Absolute,
	Exp,
	Log,
	Log10,
	Tangent,
	ArcTangent,
	Unknown,
};
SimpleTransformOp simple_transform_op(std::string_view p_enumerator);
float simple_transform(uint8_t p_option, float p_x, float p_y);
float scale_clamp(float p_in, float p_in_min, float p_in_max, float p_out_min, float p_out_max);

enum class ConditionOp : uint8_t { And, Or, XOr, Equal, Less, Greater, LessOrEqual, GreaterOrEqual, Unknown };
enum class LogicalOp : uint8_t { And, Or, Nand, Nor, Unknown };
enum class FaderType : uint8_t { Linear, Cosine, Unknown };
enum class SamplerLoop : uint8_t { None, Forward, Release, InstantRelease, Unknown };
enum class TimerMode : uint8_t { Repeating, SingleShot, Unknown };
enum class NumberGeneratorMode : uint8_t { RandomUniform, Unknown };
enum class AngleUnit : uint8_t { Radians, Degrees, Unknown };
enum class OutputTransformSource : uint8_t { Sound, Listener, Output, Unknown };

}
