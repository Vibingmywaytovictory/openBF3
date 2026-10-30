#include "bf3/core/audio/sound_types.h"

#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bf3::core::audio {

namespace {

constexpr std::span<const std::string_view> kNone{};

constexpr std::string_view kAdsrPorts[] = { "A", "D", "Finished", "R", "Release", "S", "Trigger", "Value" };
constexpr std::string_view kBandPassIir2Ports[] = { "Bandwidth", "Frequency", "In", "Out" };
constexpr std::string_view kCameraShakePorts[] = { "Pitch", "Roll", "Yaw" };
constexpr std::string_view kConeOutputPorts[] = { "BypassHeadroom", "CenterLevel", "In", "InnerAngle", "LfeLevel", "OuterAngle", "PositionX", "PositionY", "PositionZ" };
constexpr std::string_view kConeOutputFloats[] = { "Gain", "HFDampingAngle", "HFDampingDistance", "HFDampingObstruction", "HFDampingOcclusion", "MinDistance", "OutsideGain", "PanSize", "ReverbGain" };
constexpr std::string_view kCrossfader2Ports[] = { "Ctrl", "CtrlOut1", "CtrlOut2" };
constexpr std::string_view kDeMuxPorts[] = { "Trigger", "Value" };
constexpr std::string_view kDelayPorts[] = { "DelayTime", "Feedback", "In", "Out" };
constexpr std::string_view kDelayFloats[] = { "MaxDelayTime" };
constexpr std::string_view kDirectOutputPorts[] = { "BypassHeadroom", "In" };
constexpr std::string_view kDirectOutputFloats[] = { "Gain", "HFDampingDistance", "HFDampingObstruction", "HFDampingOcclusion", "MinDistance" };
constexpr std::string_view kEventGatePorts[] = { "CoolDownTime", "Enable", "In", "Out" };
constexpr std::string_view kFlatOutputPorts[] = { "BypassHeadroom", "CenterLevel", "In", "LfeLevel" };
constexpr std::string_view kFlatOutputFloats[] = { "Angle", "Gain", "HFDampingDistance", "HFDampingObstruction", "HFDampingOcclusion", "MinDistance", "ReverbGain" };
constexpr std::string_view kGainPorts[] = { "Amplitude", "In", "Out" };
constexpr std::string_view kHighPassIir2Ports[] = { "Frequency", "In", "Out" };
constexpr std::string_view kLfoPorts[] = { "Amplitude", "Hz", "Out" };
constexpr std::string_view kLfoFloats[] = { "Max", "Min" };
constexpr std::string_view kLogicalExpressionPorts[] = { "Trigger" };
constexpr std::string_view kLowPassIir2Ports[] = { "Frequency", "In", "Out" };
constexpr std::string_view kMultiCrossfaderPorts[] = { "Control", "Start", "Stop" };
constexpr std::string_view kNumberGeneratorPorts[] = { "Trigger", "Y" };
constexpr std::string_view kNumberGeneratorFloats[] = { "Max", "Min" };
constexpr std::string_view kPadRumblePorts[] = { "RumbleHigh", "RumbleLow" };
constexpr std::string_view kParameterFilterLinearPorts[] = { "AttackSpeed", "In", "Out", "ReleaseSpeed" };
constexpr std::string_view kPeakingIir2Ports[] = { "Amplitude", "Frequency", "In", "Out", "Q" };
constexpr std::string_view kPhysicsPorts[] = { "Azimuth", "Distance", "ElevationAngle", "RelativeVelocity", "Velocity" };
constexpr std::string_view kSamplerPorts[] = { "Amplitude", "Buffer", "Buffered", "Delay", "EnableStep", "ExternalWave", "Finished", "Offset", "Output", "Pitch", "Position", "Release", "Step", "Trigger", "Variation" };
constexpr std::string_view kSamplerFloats[] = { "BasePitch" };
constexpr std::string_view kScaleClampPorts[] = { "In", "InMax", "InMin", "Out", "OutMax", "OutMin" };
constexpr std::string_view kSimpleTransformPorts[] = { "X", "Y", "Z" };
constexpr std::string_view kSimpleValueSelectorPorts[] = { "Index", "Out" };
constexpr std::string_view kSwitcherPorts[] = { "Trigger", "Value" };
constexpr std::string_view kSwitcherFloats[] = { "DefaultCaseValue" };
constexpr std::string_view kTimerPorts[] = { "Period", "Progress", "Start", "Stop", "Tick" };
constexpr std::string_view kValueSelectorPorts[] = { "Out", "Value" };
constexpr std::string_view kValueSelectorFloats[] = { "DefaultCaseValue" };
constexpr std::string_view kWaveSwitcherPorts[] = { "Advance", "Index", "IndexChanged", "Wave" };
constexpr std::string_view kWaveSwitcherFloats[] = { "DefaultIndex" };
constexpr std::string_view kConditionGroupPorts[] = { "True", "X", "Y" };
constexpr std::string_view kDeMuxOutputPorts[] = { "Trigger" };
constexpr std::string_view kLogicalExpressionEntryPorts[] = { "Trigger" };
constexpr std::string_view kMixerReceiveEntryPorts[] = { "Out" };
constexpr std::string_view kMultiCrossfaderGroupPorts[] = { "Amplitude", "Start", "Stop" };
constexpr std::string_view kMultiCrossfaderGroupFloats[] = { "FadeAmplitude", "FadeBegin", "FadeEnd" };
constexpr std::string_view kReceiveEntryPorts[] = { "Out" };
constexpr std::string_view kReceiveEntryFloats[] = { "Parameter", "SavedValue" };
constexpr std::string_view kSwitcherEntryPorts[] = { "CaseTrigger" };
constexpr std::string_view kSwitcherEntryFloats[] = { "CaseValue" };
constexpr std::string_view kValueSelectorEntryPorts[] = { "Input" };
constexpr std::string_view kValueSelectorEntryFloats[] = { "CaseValue" };

constexpr SoundNodeTypeInfo kTypes[] = {
	{ SoundNodeType::Adsr, "AdsrNodeData", SoundNodeRole::Patch, true, false, kAdsrPorts, kNone, "", "" },
	{ SoundNodeType::BandPassIir2, "BandPassIir2NodeData", SoundNodeRole::Patch, true, true, kBandPassIir2Ports, kNone, "", "" },
	{ SoundNodeType::CameraShake, "CameraShakeNodeData", SoundNodeRole::Patch, true, false, kCameraShakePorts, kNone, "", "" },
	{ SoundNodeType::Condition, "ConditionNodeData", SoundNodeRole::Patch, true, false, kNone, kNone, "", "Conditions" },
	{ SoundNodeType::ConeOutput, "ConeOutputNodeData", SoundNodeRole::Patch, true, true, kConeOutputPorts, kConeOutputFloats, "", "" },
	{ SoundNodeType::Crossfader2, "Crossfader2NodeData", SoundNodeRole::Patch, true, false, kCrossfader2Ports, kNone, "", "" },
	{ SoundNodeType::DeMux, "DeMuxNodeData", SoundNodeRole::Patch, true, false, kDeMuxPorts, kNone, "", "Outputs" },
	{ SoundNodeType::Delay, "DelayNodeData", SoundNodeRole::Patch, true, true, kDelayPorts, kDelayFloats, "", "" },
	{ SoundNodeType::DirectOutput, "DirectOutputNodeData", SoundNodeRole::Patch, true, true, kDirectOutputPorts, kDirectOutputFloats, "", "" },
	{ SoundNodeType::EventGate, "EventGateNodeData", SoundNodeRole::Patch, true, false, kEventGatePorts, kNone, "", "" },
	{ SoundNodeType::FlatOutput, "FlatOutputNodeData", SoundNodeRole::Patch, true, true, kFlatOutputPorts, kFlatOutputFloats, "", "" },
	{ SoundNodeType::Gain, "GainNodeData", SoundNodeRole::Patch, true, true, kGainPorts, kNone, "", "" },
	{ SoundNodeType::HighPassIir2, "HighPassIir2NodeData", SoundNodeRole::Patch, true, true, kHighPassIir2Ports, kNone, "", "" },
	{ SoundNodeType::Lfo, "LfoNodeData", SoundNodeRole::Patch, true, false, kLfoPorts, kLfoFloats, "", "" },
	{ SoundNodeType::LogicalExpression, "LogicalExpressionNodeData", SoundNodeRole::Patch, true, false, kLogicalExpressionPorts, kNone, "Operator", "Inputs" },
	{ SoundNodeType::LowPassIir2, "LowPassIir2NodeData", SoundNodeRole::Patch, true, true, kLowPassIir2Ports, kNone, "", "" },
	{ SoundNodeType::MixerReceive, "MixerReceiveNodeData", SoundNodeRole::Patch, true, false, kNone, kNone, "", "Entries" },
	{ SoundNodeType::MultiCrossfader, "MultiCrossfaderNodeData", SoundNodeRole::Patch, true, false, kMultiCrossfaderPorts, kNone, "", "CrossfaderGroups" },
	{ SoundNodeType::NumberGenerator, "NumberGeneratorNodeData", SoundNodeRole::Patch, true, false, kNumberGeneratorPorts, kNumberGeneratorFloats, "Mode", "" },
	{ SoundNodeType::PadRumble, "PadRumbleNodeData", SoundNodeRole::Patch, true, false, kPadRumblePorts, kNone, "", "" },
	{ SoundNodeType::ParameterFilterLinear, "ParameterFilterLinearNodeData", SoundNodeRole::Patch, true, false, kParameterFilterLinearPorts, kNone, "", "" },
	{ SoundNodeType::PeakingIir2, "PeakingIir2NodeData", SoundNodeRole::Patch, true, true, kPeakingIir2Ports, kNone, "", "" },
	{ SoundNodeType::Physics, "PhysicsNodeData", SoundNodeRole::Patch, true, false, kPhysicsPorts, kNone, "", "" },
	{ SoundNodeType::Receive, "ReceiveNodeData", SoundNodeRole::Patch, true, false, kNone, kNone, "", "Entries" },
	{ SoundNodeType::Sampler, "SamplerNodeData", SoundNodeRole::Patch, true, true, kSamplerPorts, kSamplerFloats, "Loop", "" },
	{ SoundNodeType::ScaleClamp, "ScaleClampNodeData", SoundNodeRole::Patch, true, false, kScaleClampPorts, kNone, "", "" },
	{ SoundNodeType::SimpleTransform, "SimpleTransformNodeData", SoundNodeRole::Patch, true, false, kSimpleTransformPorts, kNone, "Operation", "" },
	{ SoundNodeType::SimpleValueSelector, "SimpleValueSelectorNodeData", SoundNodeRole::Patch, true, false, kSimpleValueSelectorPorts, kNone, "", "" },
	{ SoundNodeType::Switcher, "SwitcherNodeData", SoundNodeRole::Patch, true, false, kSwitcherPorts, kSwitcherFloats, "", "Outputs" },
	{ SoundNodeType::Timer, "TimerNodeData", SoundNodeRole::Patch, true, false, kTimerPorts, kNone, "Mode", "" },
	{ SoundNodeType::ValueSelector, "ValueSelectorNodeData", SoundNodeRole::Patch, true, false, kValueSelectorPorts, kValueSelectorFloats, "", "Inputs" },
	{ SoundNodeType::WaveSwitcher, "WaveSwitcherNodeData", SoundNodeRole::Patch, true, false, kWaveSwitcherPorts, kWaveSwitcherFloats, "", "" },
	{ SoundNodeType::Delta, "DeltaNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::DialogSampler, "DialogSamplerNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::DistortionClip, "DistortionClipNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::DivisibleLoopPlayer, "DivisibleLoopPlayerNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::EventSwitcher, "EventSwitcherNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::FrequencyShiftSsb, "FrequencyShiftSsbNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::HighShelfIir2, "HighShelfIir2NodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Limiter, "LimiterNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::LowShelfIir2, "LowShelfIir2NodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Orientation, "OrientationNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::ParameterFilterLp, "ParameterFilterLpNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Route, "RouteNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Send, "SendNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::SequenceGenerator, "SequenceGeneratorNodeData", SoundNodeRole::Patch, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Bus, "BusNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Compressor, "CompressorNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::Dac, "DacNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::IrReverb, "IrReverbNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::IrReverbController, "IrReverbControllerNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MasterUnit, "MasterUnitNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MatrixPanner, "MatrixPannerNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MinMaxValueSelector, "MinMaxValueSelectorNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MixerInput, "MixerInputNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MixerOutput, "MixerOutputNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::MixerValue, "MixerValueNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::UserMusicArbiter, "UserMusicArbiterNodeData", SoundNodeRole::MasterOrMixer, false, false, kNone, kNone, "", "" },
	{ SoundNodeType::ConditionGroup, "ConditionGroup", SoundNodeRole::Child, true, false, kConditionGroupPorts, kNone, "Condition", "" },
	{ SoundNodeType::DeMuxOutput, "DeMuxOutput", SoundNodeRole::Child, true, false, kDeMuxOutputPorts, kNone, "", "" },
	{ SoundNodeType::LogicalExpressionEntry, "LogicalExpressionEntry", SoundNodeRole::Child, true, false, kLogicalExpressionEntryPorts, kNone, "", "" },
	{ SoundNodeType::MixerReceiveEntry, "MixerReceiveEntry", SoundNodeRole::Child, true, false, kMixerReceiveEntryPorts, kNone, "", "" },
	{ SoundNodeType::MultiCrossfaderGroup, "MultiCrossfaderGroup", SoundNodeRole::Child, true, false, kMultiCrossfaderGroupPorts, kMultiCrossfaderGroupFloats, "FadeType", "" },
	{ SoundNodeType::ReceiveEntry, "ReceiveEntry", SoundNodeRole::Child, true, false, kReceiveEntryPorts, kReceiveEntryFloats, "", "" },
	{ SoundNodeType::SwitcherEntry, "SwitcherEntry", SoundNodeRole::Child, true, false, kSwitcherEntryPorts, kSwitcherEntryFloats, "", "" },
	{ SoundNodeType::ValueSelectorEntry, "ValueSelectorEntry", SoundNodeRole::Child, true, false, kValueSelectorEntryPorts, kValueSelectorEntryFloats, "", "" },
};
static_assert(std::size(kTypes) == size_t(SoundNodeType::Count));

consteval bool table_in_enum_order() {
	for (size_t i = 0; i < std::size(kTypes); ++i) {
		if (size_t(kTypes[i].type) != i) {
			return false;
		}
	}
	return true;
}
static_assert(table_in_enum_order());

consteval size_t count_types(bool p_a2_only, bool p_patch_only) {
	size_t n = 0;
	for (const SoundNodeTypeInfo &info : kTypes) {
		if (info.role == SoundNodeRole::Child) {
			continue;
		}
		if (p_a2_only && !info.a2) {
			continue;
		}
		if (p_patch_only && info.role != SoundNodeRole::Patch) {
			continue;
		}
		++n;
	}
	return n;
}
static_assert(count_types(false, false) == kAllNodeTypes);
static_assert(count_types(false, true) == kPatchNodeTypes);
static_assert(count_types(true, true) == kA2NodeTypes);

struct EnumName {
	std::string_view name;
	SimpleTransformOp op;
};
constexpr EnumName kTransformOps[] = {
	{ "Add", SimpleTransformOp::Add },
	{ "Subtract", SimpleTransformOp::Subtract },
	{ "Multiply", SimpleTransformOp::Multiply },
	{ "Divide", SimpleTransformOp::Divide },
	{ "Sine", SimpleTransformOp::Sine },
	{ "Cosine", SimpleTransformOp::Cosine },
	{ "ArcSine", SimpleTransformOp::ArcSine },
	{ "ArcCosine", SimpleTransformOp::ArcCosine },
	{ "Modulus", SimpleTransformOp::Modulus },
	{ "Round", SimpleTransformOp::Round },
	{ "Floor", SimpleTransformOp::Floor },
	{ "Ceiling", SimpleTransformOp::Ceiling },
	{ "Power", SimpleTransformOp::Power },
	{ "SquareRoot", SimpleTransformOp::SquareRoot },
	{ "Absolute", SimpleTransformOp::Absolute },
	{ "Exp", SimpleTransformOp::Exp },
	{ "Log", SimpleTransformOp::Log },
	{ "Log10", SimpleTransformOp::Log10 },
	{ "Tangent", SimpleTransformOp::Tangent },
	{ "ArcTangent", SimpleTransformOp::ArcTangent },
};
static_assert(std::size(kTransformOps) == size_t(SimpleTransformOp::Unknown));

}

std::span<const SoundNodeTypeInfo> sound_node_types() {
	return kTypes;
}

const SoundNodeTypeInfo &sound_node_type_info(SoundNodeType p_type) {
	size_t index = size_t(p_type);
	return kTypes[index < std::size(kTypes) ? index : 0];
}

std::optional<SoundNodeType> sound_node_type(std::string_view p_ebx_name) {
	for (const SoundNodeTypeInfo &info : kTypes) {
		if (info.ebx_name == p_ebx_name) {
			return info.type;
		}
	}
	return std::nullopt;
}

SimpleTransformOp simple_transform_op(std::string_view p_enumerator) {
	constexpr std::string_view kPrefix = "SimpleTransformOperation_";
	if (!p_enumerator.starts_with(kPrefix)) {
		return SimpleTransformOp::Unknown;
	}
	p_enumerator.remove_prefix(kPrefix.size());
	for (const EnumName &entry : kTransformOps) {
		if (entry.name == p_enumerator) {
			return entry.op;
		}
	}
	return SimpleTransformOp::Unknown;
}

float simple_transform(uint8_t p_option, float p_x, float p_y) {
	constexpr float kPi = 3.14159265358979323846f;
	const bool degrees = (p_option & port::kSimpleTransformDegrees) != 0;
	const auto op = SimpleTransformOp(p_option & 0x7f);
	const double to_rad = degrees ? double(kPi) / 180.0 : 1.0;
	const double from_rad = degrees ? 180.0 / double(kPi) : 1.0;
	const double X = p_x, Y = p_y;
	switch (op) {
		case SimpleTransformOp::Add:
			return finite_or_zero(X + Y);
		case SimpleTransformOp::Subtract:
			return finite_or_zero(X - Y);
		case SimpleTransformOp::Multiply:
			return finite_or_zero(X * Y);
		case SimpleTransformOp::Divide:
			return Y == 0.0 ? 0.0f : finite_or_zero(X / Y);
		case SimpleTransformOp::Sine:
			return finite_or_zero(std::sin(X * to_rad));
		case SimpleTransformOp::Cosine:
			return finite_or_zero(std::cos(X * to_rad));
		case SimpleTransformOp::Tangent:
			return finite_or_zero(std::tan(X * to_rad));
		case SimpleTransformOp::ArcSine:
			return (X < -1.0 || X > 1.0) ? 0.0f : finite_or_zero(std::asin(X) * from_rad);
		case SimpleTransformOp::ArcCosine:
			return (X < -1.0 || X > 1.0) ? 0.0f : finite_or_zero(std::acos(X) * from_rad);
		case SimpleTransformOp::ArcTangent:
			return finite_or_zero(std::atan(X) * from_rad);
		case SimpleTransformOp::Modulus:
			return Y == 0.0 ? 0.0f : finite_or_zero(std::fmod(X, Y));
		case SimpleTransformOp::Round:
			return finite_or_zero(std::round(X));
		case SimpleTransformOp::Floor:
			return finite_or_zero(std::floor(X));
		case SimpleTransformOp::Ceiling:
			return finite_or_zero(std::ceil(X));
		case SimpleTransformOp::Power:
			if (X < 0.0 && Y != std::floor(Y)) {
				return 0.0f;
			}
			if (X == 0.0 && Y < 0.0) {
				return 0.0f;
			}
			return finite_or_zero(std::pow(X, Y));
		case SimpleTransformOp::SquareRoot:
			return X < 0.0 ? 0.0f : finite_or_zero(std::sqrt(X));
		case SimpleTransformOp::Absolute:
			return finite_or_zero(std::fabs(X));
		case SimpleTransformOp::Exp:
			return finite_or_zero(std::exp(X));
		case SimpleTransformOp::Log:
			return X <= 0.0 ? 0.0f : finite_or_zero(std::log(X));
		case SimpleTransformOp::Log10:
			return X <= 0.0 ? 0.0f : finite_or_zero(std::log10(X));
		case SimpleTransformOp::Unknown:
			return 0.0f;
	}
	return 0.0f;
}

float scale_clamp(float p_in, float p_in_min, float p_in_max, float p_out_min, float p_out_max) {
	if (p_in_min == p_in_max) {
		return p_in < p_in_min ? p_out_min : p_out_max;
	}
	const double t = std::clamp((double(p_in) - p_in_min) / (double(p_in_max) - p_in_min), 0.0, 1.0);
	return finite_or_zero(p_out_min + t * (double(p_out_max) - p_out_min));
}

}
