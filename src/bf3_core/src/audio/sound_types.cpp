// The sound-graph node type table (sound_types.h). Ports and floats of the A2 types are the AudioGraphNodePort and
// float fields every instance of the type carries in the 164 handheld weapon patches [verified 2026-10-15, scratch
// census, design §4.3]; the other types list none until their wave (A5-A7) implements them.

#include "bf3/core/audio/sound_types.h"

#include <array>

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

} // namespace

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

} // namespace bf3::core::audio
