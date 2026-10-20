#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::present {

enum class PresentKind : uint8_t { OwnShot = 0, OwnTimed = 1, RemoteShot = 2, Impact = 3, SoldierContact = 4, Hit = 5, Death = 6, Detonation = 7, Spawn = 8, Revive = 9 };
enum class PresentTimeline : uint8_t { Predicted = 0, Render = 1, Immediate = 2 };

namespace flags {
inline constexpr uint8_t kTracer = 1u << 0;
inline constexpr uint8_t kHeadshot = 1u << 1;
inline constexpr uint8_t kDud = 1u << 2;
inline constexpr uint8_t kOwnVictim = 1u << 3;
inline constexpr uint8_t kOwnShooter = 1u << 4;
inline constexpr uint8_t kRevived = 1u << 5;
}

struct PresentEvent {
	PresentKind kind = PresentKind::OwnShot;
	PresentTimeline timeline = PresentTimeline::Predicted;
	uint8_t slot = 0;
	uint8_t aux = 0;
	uint16_t def = 0xFFFF;
	uint8_t flags = 0;
	uint8_t pellet = 0;
	uint32_t id = 0;
	uint32_t material = 0;
	int64_t time = 0;
	float pos[3] = { 0, 0, 0 };
	float dir[3] = { 0, 0, 0 };
	float value = 0;
	uint32_t extra = 0;
	uint64_t seed = 0;
};
static_assert(sizeof(PresentEvent) == 64);
static_assert(std::is_trivially_copyable_v<PresentEvent>);

inline constexpr size_t kPresentationEventCapacity = 2048;

class PresentationEvents {
public:
	bool push(const PresentEvent &p_event);
	size_t drain(std::span<PresentEvent> r_out);
	uint64_t overflows() const;

private:
	std::array<PresentEvent, kPresentationEventCapacity> ring_{};

	std::atomic<uint64_t> head_{ 0 };
	std::atomic<uint64_t> tail_{ 0 };
	std::atomic<uint64_t> overflows_{ 0 };
};

}
