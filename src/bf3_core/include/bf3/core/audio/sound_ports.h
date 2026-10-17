#pragma once
// Port and float positions of the compiled sound-graph nodes (M2a audio A2; docs/design/m2a-audio.md §6.2). The
// compiler stores a node's AudioGraphNodePorts in the order sound_node_type_info(type).ports lists them (the
// census order, alphabetical) and its plain floats in the order .floats lists them; the interpreter reads them by
// the positions below. audio.contract checks every enumerator against the type table by name, so a reordered table
// cannot silently shift a port.
//
// Compiled extras (not EBX fields of their own):
//   ReceiveEntry   one extra port after Out: `Source`, the register of the AudioGraphParameter/Event the entry
//                  copies (its ValueIndex), always connected.
//   SimpleValueSelector  OutValues[] follow the (zero) plain floats.
//   SimpleTransform      SoundNode::option = SimpleTransformOp in bits 0-6, bit 7 set for AngleUnit_Degrees.
//
// Which ports a node writes (the rest it reads) drives the topological sort: output_ports() below.

#include "bf3/core/audio/sound_types.h"

#include <cstdint>

namespace bf3::core::audio::port {

enum Adsr : uint8_t { AdsrA, AdsrD, AdsrFinished, AdsrR, AdsrRelease, AdsrS, AdsrTrigger, AdsrValue };
enum BandPass : uint8_t { BandPassBandwidth, BandPassFrequency, BandPassIn, BandPassOut };
enum CameraShake : uint8_t { ShakePitch, ShakeRoll, ShakeYaw };
enum ConeOutput : uint8_t { ConeBypassHeadroom, ConeCenterLevel, ConeIn, ConeInnerAngle, ConeLfeLevel, ConeOuterAngle, ConePositionX, ConePositionY, ConePositionZ };
enum Crossfader2 : uint8_t { Cf2Ctrl, Cf2Out1, Cf2Out2 };
enum DeMux : uint8_t { DeMuxTrigger, DeMuxValue };
enum Delay : uint8_t { DelayTime, DelayFeedback, DelayIn, DelayOut };
enum EventGate : uint8_t { GateCoolDown, GateEnable, GateIn, GateOut };
enum Gain : uint8_t { GainAmplitude, GainIn, GainOut };
enum Iir2 : uint8_t { Iir2Frequency, Iir2In, Iir2Out }; // HighPassIir2, LowPassIir2
enum Lfo : uint8_t { LfoAmplitude, LfoHz, LfoOut };
enum LogicalExpression : uint8_t { LogicTrigger };
enum MultiCrossfader : uint8_t { McfControl, McfStart, McfStop };
enum NumberGenerator : uint8_t { NumTrigger, NumY };
enum PadRumble : uint8_t { RumbleHigh, RumbleLow };
enum ParameterFilterLinear : uint8_t { PflAttack, PflIn, PflOut, PflRelease };
enum Peaking : uint8_t { PeakAmplitude, PeakFrequency, PeakIn, PeakOut, PeakQ };
enum Physics : uint8_t { PhysAzimuth, PhysDistance, PhysElevation, PhysRelativeVelocity, PhysVelocity };
enum Sampler : uint8_t {
	SmpAmplitude,
	SmpBuffer,
	SmpBuffered,
	SmpDelay,
	SmpEnableStep,
	SmpExternalWave,
	SmpFinished,
	SmpOffset,
	SmpOutput,
	SmpPitch,
	SmpPosition,
	SmpRelease,
	SmpStep,
	SmpTrigger,
	SmpVariation,
};
enum ScaleClamp : uint8_t { ScIn, ScInMax, ScInMin, ScOut, ScOutMax, ScOutMin };
enum SimpleTransform : uint8_t { StX, StY, StZ };
enum SimpleValueSelector : uint8_t { SvsIndex, SvsOut };
enum Switcher : uint8_t { SwTrigger, SwValue };
enum Timer : uint8_t { TimerPeriod, TimerProgress, TimerStart, TimerStop, TimerTick };
enum ValueSelector : uint8_t { VsOut, VsValue };
enum WaveSwitcher : uint8_t { WsAdvance, WsIndex, WsIndexChanged, WsWave };
// Child records.
enum ConditionGroup : uint8_t { CondTrue, CondX, CondY };
enum DeMuxOutput : uint8_t { DeMuxOutTrigger };
enum LogicalExpressionEntry : uint8_t { LogicEntryTrigger };
enum MixerReceiveEntry : uint8_t { MixerEntryOut };
enum MultiCrossfaderGroup : uint8_t { GroupAmplitude, GroupStart, GroupStop };
enum ReceiveEntry : uint8_t { ReceiveOut, ReceiveSource /* compiled extra */ };
enum SwitcherEntry : uint8_t { SwitchCaseTrigger };
enum ValueSelectorEntry : uint8_t { VsEntryInput };

// Plain floats.
enum ConeFloat : uint8_t { ConeGain, ConeHfAngle, ConeHfDistance, ConeHfObstruction, ConeHfOcclusion, ConeMinDistance, ConeOutsideGain, ConePanSize, ConeReverbGain };
enum DelayFloat : uint8_t { DelayMax };
enum DirectFloat : uint8_t { DirectGain, DirectHfDistance, DirectHfObstruction, DirectHfOcclusion, DirectMinDistance };
enum FlatFloat : uint8_t { FlatAngle, FlatGain, FlatHfDistance, FlatHfObstruction, FlatHfOcclusion, FlatMinDistance, FlatReverbGain };
enum LfoFloat : uint8_t { LfoMax, LfoMin };
enum NumberGeneratorFloat : uint8_t { NumMax, NumMin };
enum SamplerFloat : uint8_t { SmpBasePitch };
enum SwitcherFloat : uint8_t { SwDefault };
enum ValueSelectorFloat : uint8_t { VsDefault };
enum WaveSwitcherFloat : uint8_t { WsDefaultIndex };
enum GroupFloat : uint8_t { GroupFadeAmplitude, GroupFadeBegin, GroupFadeEnd };
enum ReceiveFloat : uint8_t { ReceiveParameter, ReceiveSavedValue };
enum CaseFloat : uint8_t { CaseValue }; // SwitcherEntry, ValueSelectorEntry

inline constexpr uint8_t kSimpleTransformDegrees = 0x80; // SoundNode::option bit of SimpleTransform

// Extra compiled ports after the type's own (ReceiveEntry: Source).
constexpr uint16_t extra_ports(SoundNodeType p_type) {
	return p_type == SoundNodeType::ReceiveEntry ? 1 : 0;
}

// Bit i set: port i is written by the node (an output); the others are read. Audio In/Out/Output ports (register 0)
// are neither: the compiler stores them unconnected.
constexpr uint32_t output_ports(SoundNodeType p_type) {
	switch (p_type) {
		case SoundNodeType::Adsr:
			return (1u << AdsrFinished) | (1u << AdsrValue);
		case SoundNodeType::Crossfader2:
			return (1u << Cf2Out1) | (1u << Cf2Out2);
		case SoundNodeType::EventGate:
			return 1u << GateOut;
		case SoundNodeType::Lfo:
			return 1u << LfoOut;
		case SoundNodeType::LogicalExpression:
			return 1u << LogicTrigger;
		case SoundNodeType::NumberGenerator:
			return 1u << NumY;
		case SoundNodeType::ParameterFilterLinear:
			return 1u << PflOut;
		case SoundNodeType::Physics:
			return 0x1fu;
		case SoundNodeType::Sampler:
			return (1u << SmpBuffered) | (1u << SmpFinished) | (1u << SmpPosition);
		case SoundNodeType::ScaleClamp:
			return 1u << ScOut;
		case SoundNodeType::SimpleTransform:
			return 1u << StZ;
		case SoundNodeType::SimpleValueSelector:
			return 1u << SvsOut;
		case SoundNodeType::Timer:
			return (1u << TimerProgress) | (1u << TimerTick);
		case SoundNodeType::ValueSelector:
			return 1u << VsOut;
		case SoundNodeType::WaveSwitcher:
			return (1u << WsIndexChanged) | (1u << WsWave);
		case SoundNodeType::ConditionGroup:
			return 1u << CondTrue;
		case SoundNodeType::DeMuxOutput:
			return 1u << DeMuxOutTrigger;
		case SoundNodeType::MixerReceiveEntry:
			return 1u << MixerEntryOut;
		case SoundNodeType::MultiCrossfaderGroup:
			return (1u << GroupAmplitude) | (1u << GroupStart) | (1u << GroupStop);
		case SoundNodeType::ReceiveEntry:
			return 1u << ReceiveOut;
		case SoundNodeType::SwitcherEntry:
			return 1u << SwitchCaseTrigger;
		default:
			return 0;
	}
}

// Audio-path ports (register 0, "v0"): never part of the control program.
constexpr uint32_t audio_ports(SoundNodeType p_type) {
	switch (p_type) {
		case SoundNodeType::BandPassIir2:
			return (1u << BandPassIn) | (1u << BandPassOut);
		case SoundNodeType::ConeOutput:
			return 1u << ConeIn;
		case SoundNodeType::Delay:
			return (1u << DelayIn) | (1u << DelayOut);
		case SoundNodeType::DirectOutput:
			return 1u << 1; // In
		case SoundNodeType::FlatOutput:
			return 1u << 2; // In
		case SoundNodeType::Gain:
			return (1u << GainIn) | (1u << GainOut);
		case SoundNodeType::HighPassIir2:
		case SoundNodeType::LowPassIir2:
			return (1u << Iir2In) | (1u << Iir2Out);
		case SoundNodeType::PeakingIir2:
			return (1u << PeakIn) | (1u << PeakOut);
		case SoundNodeType::Sampler:
			return 1u << SmpOutput;
		default:
			return 0;
	}
}

// The child record type a node's children array holds (Count = none).
constexpr SoundNodeType child_type(SoundNodeType p_type) {
	switch (p_type) {
		case SoundNodeType::Condition:
			return SoundNodeType::ConditionGroup;
		case SoundNodeType::DeMux:
			return SoundNodeType::DeMuxOutput;
		case SoundNodeType::LogicalExpression:
			return SoundNodeType::LogicalExpressionEntry;
		case SoundNodeType::MixerReceive:
			return SoundNodeType::MixerReceiveEntry;
		case SoundNodeType::MultiCrossfader:
			return SoundNodeType::MultiCrossfaderGroup;
		case SoundNodeType::Receive:
			return SoundNodeType::ReceiveEntry;
		case SoundNodeType::Switcher:
			return SoundNodeType::SwitcherEntry;
		case SoundNodeType::ValueSelector:
			return SoundNodeType::ValueSelectorEntry;
		default:
			return SoundNodeType::Count;
	}
}

// Plugin references a node carries (SoundNode::plugins_count): Sampler 4 per slot (SndPlayer, Resample, Pause,
// Gain), the audio-rate nodes 1 (Plugin), the outputs 5 (LowPass, Pan, MainSend, ReverbSend, Vu).
inline constexpr uint16_t kSamplerSlotPlugins = 4;
enum SamplerPlugin : uint8_t { SlotSndPlayer, SlotResample, SlotPause, SlotGain };
enum OutputPlugin : uint8_t { OutLowPass, OutPan, OutMainSend, OutReverbSend, OutVu };
inline constexpr uint16_t kOutputPlugins = 5;

// Plugin attributes an audio-rate node drives, by AttributeIndex [verified on the M16A3's LinkedPluginAttributes:
// HighPassIir2 Frequency 0; PeakingIir2 Frequency 0, Amplitude 1, Q 2; Gain Amplitude 0]. BandPassIir2 and Delay
// follow the same field order [inferred]. kNoPort = the node has no such attribute.
inline constexpr uint8_t kNoPort = 0xff;
inline constexpr size_t kMaxPluginAttributes = 16; // sound/master links attribute 4+ of its MasterUnit plugins
constexpr uint8_t attribute_port(SoundNodeType p_type, uint8_t p_attribute) {
	switch (p_type) {
		case SoundNodeType::HighPassIir2:
		case SoundNodeType::LowPassIir2:
			return p_attribute == 0 ? uint8_t(Iir2Frequency) : kNoPort;
		case SoundNodeType::PeakingIir2:
			return p_attribute == 0 ? uint8_t(PeakFrequency) : p_attribute == 1 ? uint8_t(PeakAmplitude) : p_attribute == 2 ? uint8_t(PeakQ) : kNoPort;
		case SoundNodeType::BandPassIir2:
			return p_attribute == 0 ? uint8_t(BandPassFrequency) : p_attribute == 1 ? uint8_t(BandPassBandwidth) : kNoPort;
		case SoundNodeType::Gain:
			return p_attribute == 0 ? uint8_t(GainAmplitude) : kNoPort;
		case SoundNodeType::Delay:
			return p_attribute == 0 ? uint8_t(DelayTime) : p_attribute == 1 ? uint8_t(DelayFeedback) : kNoPort;
		default:
			return kNoPort;
	}
}

} // namespace bf3::core::audio::port
