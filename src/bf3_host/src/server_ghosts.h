#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

struct GhostTier {
	float metres = 0;
	uint8_t target = 1;
	uint8_t required = 1;
};

inline constexpr float kEnemyScopeMetres = 1500.0f;
inline constexpr std::array<GhostTier, 4> kEnemyTiers{ { { 100.0f, 1, 1 }, { 300.0f, 2, 3 }, { 800.0f, 5, 6 },
		{ kEnemyScopeMetres, 15, 24 } } };
inline constexpr std::array<GhostTier, 3> kTeammateTiers{ { { 100.0f, 2, 3 }, { 300.0f, 6, 8 }, { 1.0e9f, 15, 24 } } };
inline constexpr GhostTier kAttackerTier{ 0.0f, 1, 1 };
inline constexpr GhostTier kAimConeTier{ 0.0f, 1, 2 };
inline constexpr float kAimConeCos = 0.98480775f;
inline constexpr sim::Tick kAttackerTicks = 5 * sim::kSimHz;
inline constexpr size_t kGhostSentRing = 16;

struct GhostViewer {
	uint8_t slot = 0;
	uint8_t team = 0;
	bool spawned = false;
	bool zoomed = false;
	sim::Vec3 position{};
	sim::Vec3 eye{};
	sim::Vec3 aim{ 0.0f, 0.0f, 1.0f };
};

struct GhostCandidate {
	uint8_t slot = 0;
	uint8_t team = 0;
	bool squad_mate = false;
	sim::Vec3 position{};
	net::OtherSoldier record;
};

struct GhostPlan {
	std::vector<net::OtherSoldier> others;
	net::SnapshotBases bases;
	std::vector<uint8_t> removed;
	std::vector<uint8_t> in_scope;
};

std::optional<GhostTier> ghost_tier(const GhostViewer &p_viewer, const GhostCandidate &p_ghost, bool p_attacker);

struct FlyBy {
	float radius = 0;
	float range = 0;
};

bool shot_passes_by(
		const sim::Vec3 &p_origin, const sim::Vec3 &p_direction, const FlyBy &p_fly_by, const sim::Vec3 &p_eye);

class GhostScheduler {
public:
	void configure(size_t p_slots, int64_t p_budget_bytes);
	void reset(size_t p_client);
	void ack(size_t p_client, const net::CommandPacket &p_packet);
	void attacked(size_t p_victim, size_t p_shooter, sim::Tick p_tick);

	int64_t open_interval(size_t p_client, uint64_t p_sent_total);
	void plan(size_t p_client, const GhostViewer &p_viewer, std::span<const GhostCandidate> p_candidates,
			uint16_t p_seq, sim::Tick p_tick, GhostPlan &r_plan);
	void own_base(size_t p_client, uint16_t p_seq, sim::Tick p_tick, net::OwnBaseline &r_base) const;
	void sent(size_t p_client, const net::Snapshot &p_snapshot, std::span<const net::OtherSoldier> p_main,
			std::span<const net::OtherSoldier> p_others);

private:
	struct SentGhost {
		bool valid = false;
		bool in_main = false;
		uint16_t seq = 0;
		sim::Tick tick = 0;
		net::OtherSoldier record;
	};
	struct Ghost {
		bool known = false;
		bool removing = false;
		uint8_t generation = 0;
		uint16_t last_seq = 0;
		bool ever = false;
		bool scoped = false;
		uint16_t scope_seq = 0;
		float accumulator = 1.0f;
		sim::Tick attacked_until = 0;
		std::array<SentGhost, kGhostSentRing> ring{};
		size_t ring_next = 0;
	};
	struct SentSnapshot {
		bool valid = false;
		bool main_acked = false;
		bool others_acked = false;
		uint16_t seq = 0;
		sim::Tick tick = 0;
		bool has_own = false;
		sim::SoldierState own;
		std::optional<net::WeaponsWire> weapons;
		std::vector<uint8_t> removed;
	};
	struct Client {
		bool open = false;
		std::vector<Ghost> ghosts;
		std::vector<SentSnapshot> snapshots;
		int64_t credit = 0;
		uint64_t sent_total = 0;
	};

	const SentSnapshot *entry(const Client &p_client, uint16_t p_seq) const;
	SentSnapshot *entry(Client &r_client, uint16_t p_seq);
	bool acked(const Client &p_client, const SentGhost &p_sent) const;
	const SentGhost *baseline(const Client &p_client, const Ghost &p_ghost, const net::OtherSoldier &p_record,
			uint16_t p_seq, sim::Tick p_tick) const;
	void forget(Ghost &r_ghost);
	void finish_removals(Client &r_client, const SentSnapshot &p_snapshot);

	std::vector<Client> clients_;
	int64_t budget_ = int64_t(net::kMaxDatagramBytes);
};

}
