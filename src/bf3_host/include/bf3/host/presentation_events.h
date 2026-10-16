#pragma once
// Gameplay events from the client thread to the Godot main thread (docs/design/m2a.md §8.3; owner cl). The
// PresentationFrame is a latest-value copy (M1 §9.5), so events, which must not be lost between two reads, go
// through this single-producer single-consumer ring instead (2,048 entries x 64 B), with an overflow counter (gated
// 0, E14). Godot-free; the triggers (fx3), the audio router (au3), the viewmodel (vm) and the HUD feed (hud) read
// the drained entries.
//
// Each entry carries its sim time and its timeline, so the main thread schedules effects (start age) and sounds
// (mixer frame) exactly:
//   Predicted  the own soldier's events (OwnShot, OwnTimed, own-shot Impacts and SoldierContacts), placed at the
//              predicted time the own viewmodel and camera show;
//   Render     everything about other soldiers (RemoteShot and its impacts, Hit and Death of another soldier,
//              Detonation), placed at render time = server time - d, the time present_others draws them at;
//   Immediate  the two server events about the own player, shown on arrival: a Hit whose victim is the own soldier
//              (health, damage direction) and a Hit whose shooter is the own soldier (the hit indicator, §11.4).
// Only the first prediction of a tick emits (replays keep state but discard their events, wp §7.2), so effects
// and sounds never fire twice.
//
// Field use per kind (time = sim time, SimTime units, on the entry's timeline):
//   OwnShot / RemoteShot  slot = shooter, id = shot index, def, pos = origin, dir = direction, aux = weapon slot,
//                         flags kTracer, seed = the effect seed (§4.4), value = power, extra = pellets
//   OwnTimed              slot = own slot, def, aux = weapon slot, id = the weapon_events:: bit
//   Impact                slot = shooter, id = shot index, pellet, pos, dir = surface normal, material = the
//                         surface's physics material, extra = impact ordinal, seed = the impact seed
//   SoldierContact        slot = shooter, aux = victim slot, id = shot index, pellet, pos, dir = normal, value = zone
//   Hit                   slot = shooter, aux = victim, id = shot index, pellet, pos, dir = normal, value = damage,
//                         extra = health after x 100, material = zone
//   Death                 slot = killer, aux = victim, def, id = shot index, flags kHeadshot, material = zone
//   Detonation            slot = shooter, id = shot index, def, pos, dir = normal, flags kDud
//   Spawn                 slot, aux = kit, extra = team, id = life

#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::host {

enum class PresentKind : uint8_t { OwnShot = 0, OwnTimed = 1, RemoteShot = 2, Impact = 3, SoldierContact = 4, Hit = 5, Death = 6, Detonation = 7, Spawn = 8 };
enum class PresentTimeline : uint8_t { Predicted = 0, Render = 1, Immediate = 2 };

namespace present_flags {
inline constexpr uint8_t kTracer = 1u << 0;
inline constexpr uint8_t kHeadshot = 1u << 1;
inline constexpr uint8_t kDud = 1u << 2;
inline constexpr uint8_t kOwnVictim = 1u << 3; // Hit / Death: the victim is the own soldier
inline constexpr uint8_t kOwnShooter = 1u << 4; // Hit / Death: the shooter is the own soldier
} // namespace present_flags

struct PresentEvent {
	PresentKind kind = PresentKind::OwnShot;
	PresentTimeline timeline = PresentTimeline::Predicted;
	uint8_t slot = 0;
	uint8_t aux = 0;
	uint16_t def = 0xFFFF; // sim::weapons::kNoWeapon when none
	uint8_t flags = 0; // present_flags::
	uint8_t pellet = 0;
	uint32_t id = 0;
	uint32_t material = 0;
	int64_t time = 0; // sim::weapons::SimTime
	float pos[3] = { 0, 0, 0 };
	float dir[3] = { 0, 0, 0 };
	float value = 0;
	uint32_t extra = 0;
	uint64_t seed = 0;
};
static_assert(sizeof(PresentEvent) == 64);
static_assert(std::is_trivially_copyable_v<PresentEvent>);

inline constexpr size_t kPresentationEventCapacity = 2048;

// SPSC: push only from the client thread, drain only from the main thread.
class PresentationEvents {
public:
	bool push(const PresentEvent &p_event); // client thread; false and counted when full
	size_t drain(std::span<PresentEvent> r_out); // main thread; oldest first, at most r_out.size()
	uint64_t overflows() const;

private:
	std::array<PresentEvent, kPresentationEventCapacity> ring_{};
	// The members are cl's to change (padding the indices apart, for one, needs C4324 handled: /W4 warns on alignas).
	std::atomic<uint64_t> head_{ 0 }; // written by the producer
	std::atomic<uint64_t> tail_{ 0 }; // written by the consumer
	std::atomic<uint64_t> overflows_{ 0 };
};

} // namespace bf3::host
