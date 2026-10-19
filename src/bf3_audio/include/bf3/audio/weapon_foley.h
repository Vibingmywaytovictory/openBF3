#pragma once
// PT1 rld (docs/design/pt1-rld.md): the weapon foley of a kit patch, read from its sound graph. Godot-free, pure.
//
// How BF3's data triggers reloads and handling sounds [verified 2026-10-19 on Metro TDM's 11 kit firearms, the knife
// and the M67 (bf3dump dump of each SoundPatchAsset), names resolved with ebx_name_hash over a dictionary]:
//   - Reload (3294210644) -> MultiCrossfader on the distance (1-19 m linear; pistols 1-17 m, M98B 1-10 m) -> one
//     LtNone Sampler whose ExternalWave is a WaveSwitcher on the parameter IsShortReload (45329532): Waves[0] =
//     <weapon>_reload_1p_long_wave, Waves[1] = <weapon>_reload_1p_short_wave (M16A3: 2.905 s and 2.367 s; the sim's
//     empty and tactical ReloadTime are 2.37 s and 1.8 s, so long = empty, short = tactical). The same Sampler's
//     Release is the event #3520090073 (name not resolved): the reload sound stops when the reload is cut short.
//     Offset 0.015 s, Amplitude 1 (0.7 on the MK11 and SVD), Pitch 1. The voice routes to the "Handling" Cone output
//     (Gain -29 dB, MinDistance 1, its own 2-point curve: -0.55 dB at 0.54 m, -55.5 dB at 26.3 m).
//   - Action (2484178491) -> MultiCrossfader 1-15 m cosine (pistols 1-10 m) -> the handling Sampler, whose wave is a
//     WaveSwitcher on ActionType (1114728387), and whose Amplitude and Pitch are SimpleValueSelectors on it. The
//     Waves[] order is the same in every kit firearm (FoleyAction below); the knife's Action plays knife_deploy, the
//     M67's deploy sampler starts from a LogicalExpression over its events.
//   - Bolt (2088812592): the bolt-action rifles' bolt wave (the M98B: 1-10 m); no Metro TDM kit weapon has it.
//   - The reload sampler plays the whole reload (magazine out, in, bolt) as one wave from the Reload event, so the
//     sound needs no clip events; the 1p / 3p clips' Ant event tracks are not read (the repo's Ant reader has none).
//     Footsteps are a different mechanism (the soldier's SoundEntity driven by AntEventEntityData step events and
//     SoldierSoundComponentData, research/topics/gap-r1-audio-system.md §3), not this one.
// The game posts these where the weapon state changes (weapon_audio.cpp's PT1 rld block): the sim's timed events for
// the own soldier (kReloadStarted, kReloadCancelled, kDeployStarted, kFireModeChanged, kBoltStarted), the tick flag
// kDryFire and the zoom flag of the presented own weapon, and the presented fields of the others (reload_seq, def,
// phase, ads).
#include "bf3/core/audio/sound_program.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kHashIsShortReload = 45329532u; // ebx_name_hash("IsShortReload")
inline constexpr uint32_t kHashAction = 2484178491u; // ebx_name_hash("Action")
inline constexpr uint32_t kHashActionType = 1114728387u; // ebx_name_hash("ActionType")
inline constexpr uint32_t kHashBolt = 2088812592u; // ebx_name_hash("Bolt")

// ActionType values: the handling WaveSwitcher's Waves[] index, the same order in every kit firearm [verified on 11
// patches; audio.foley.install checks the wave names]: generic_deploy (m9_deploy on the pistols), zoom_iron_sight,
// trigger_click, <weapon>_bipod_deploy (m249_mount on the LMGs), <weapon>_bipod_undeploy, lightswitch,
// firemodeswitch_us / _ru.
enum class FoleyAction : uint8_t {
	Deploy = 0,
	Zoom = 1,
	DryFire = 2,
	BipodDeploy = 3,
	BipodUndeploy = 4,
	Light = 5,
	FireMode = 6,
};
inline constexpr uint8_t kFoleyActionCount = 7;
const char *foley_action_name(FoleyAction p_action);

inline constexpr float kFoleyUngated = std::numeric_limits<float>::infinity();

// What one input event of a program starts, from the graph (register dataflow from the event's Receive entries).
struct FoleyEvent {
	bool present = false; // the program takes the event
	uint32_t hash = 0;
	uint16_t reg = core::audio::kNoRegister; // the event's input register
	std::vector<uint32_t> samplers; // indices into SoundProgram::nodes whose Trigger the event reaches
	// The largest FadeEnd of the distance MultiCrossfaders (Control = the Physics Distance) the event starts:
	// beyond it the event starts nothing. kFoleyUngated when no such crossfader is on its path.
	float reach_m = kFoleyUngated;
	// The input event that drives the Release port of those samplers (0: none): the reload's #3520090073.
	uint32_t release_event = 0;
	// The program outputs (indices into SoundProgram::outputs) the samplers' voices feed.
	std::vector<uint16_t> outputs;
};

// The foley inputs of one weapon patch.
struct WeaponFoley {
	FoleyEvent reload; // Reload
	FoleyEvent action; // Action
	FoleyEvent bolt; // Bolt
	bool short_reload_param = false; // IsShortReload is an input
	bool action_type_param = false; // ActionType is an input
	// The wave names the ActionType / IsShortReload WaveSwitchers hold, by index (empty when the switcher is absent).
	std::vector<std::string> action_waves;
	std::vector<std::string> reload_waves;
};

// One cue the router posts (WeaponAudioObserver::on_foley; the audio.foley.install trigger table).
enum class FoleyCueKind : uint8_t {
	Reload, // Reload (+ IsShortReload)
	ReloadRelease, // the reload's release event on its running instance (the reload was cut short)
	Action, // Action + ActionType
	Bolt, // Bolt
};
struct FoleyCue {
	FoleyCueKind kind = FoleyCueKind::Reload;
	bool own = false;
	bool first_person = false;
	uint8_t slot = 0; // the soldier's slot
	uint16_t def = 0xFFFF; // the WeaponTable def
	FoleyAction action = FoleyAction::Deploy; // Action
	int8_t short_reload = -1; // Reload: the IsShortReload value posted (1 tactical, 0 empty), -1 not set (the data's default)
	int64_t time = 0; // the cause's sim::weapons::SimTime (the event, or the sample tick's end)
	uint64_t frame = 0; // the MixerClock frame the event is posted at
	float distance_m = 0.0f; // emitter to listener at the start
};

// The event p_hash of p_program (present false when the program has no such input event). Bounded: every register
// and node is visited at most once (O(nodes + ports)).
FoleyEvent trace_foley_event(const core::audio::SoundProgram &p_program, uint32_t p_hash);
// Every field of WeaponFoley.
WeaponFoley read_weapon_foley(const core::audio::SoundProgram &p_program);
// The outputs (indices into SoundProgram::outputs) a voice feeds through the voice connections (Sen0 / Rou0 sends).
std::vector<uint16_t> voice_outputs(const core::audio::SoundProgram &p_program, uint16_t p_voice);
// One line per input for a report: "Reload 1-19 m, 1 sampler, release #3520090073, outputs Handling".
std::string describe_weapon_foley(const core::audio::SoundProgram &p_program, const WeaponFoley &p_foley);

} // namespace bf3::audio
