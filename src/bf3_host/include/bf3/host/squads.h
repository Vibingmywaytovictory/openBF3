#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

inline constexpr uint8_t kSquadTeams = 2;
inline constexpr uint8_t kDefaultSquadSize = 4;
inline constexpr int kNoSquadMate = -1;

enum class SquadRefusal : uint8_t {
	None = 0,
	NoTeam = 1,
	BadSquad = 2,
	Full = 3,
	Empty = 4,
	SameSquad = 5,
	NotLeader = 6,
	NotMember = 7,
	Self = 8,
	Balance = 9,
	Phase = 10
};

struct SquadOrderState {
	net::SquadOrderKind kind = net::SquadOrderKind::None;
	uint16_t area = net::kNoSquadArea;
	sim::Vec3 position{};
};

class SquadTable {
public:
	void configure(size_t p_slots, uint8_t p_squad_size);
	void clear();

	uint8_t squad_size() const { return size_; }
	uint8_t squad_of(uint8_t p_slot) const;
	uint8_t team_of(uint8_t p_slot) const;
	bool leader(uint8_t p_slot) const;
	bool same_squad(uint8_t p_a, uint8_t p_b) const;
	std::span<const uint8_t> members(uint8_t p_team, uint8_t p_squad) const;
	int mate(uint8_t p_slot, uint8_t p_index) const;
	bool is_private(uint8_t p_team, uint8_t p_squad) const;
	const SquadOrderState &order(uint8_t p_team, uint8_t p_squad) const;
	uint64_t revision() const { return revision_; }

	uint8_t assign(uint8_t p_slot, uint8_t p_team);
	void remove(uint8_t p_slot);
	void follow_team(uint8_t p_slot, uint8_t p_team);
	void swap_teams();

	SquadRefusal join(uint8_t p_slot, uint8_t p_team, uint8_t p_squad);
	SquadRefusal create(uint8_t p_slot, uint8_t p_team);
	SquadRefusal leave(uint8_t p_slot);
	SquadRefusal kick(uint8_t p_leader, uint8_t p_target);
	SquadRefusal set_private(uint8_t p_leader, bool p_private);
	bool set_order(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order);

private:
	struct Squad {
		std::vector<uint8_t> members;
		bool privacy = false;
		SquadOrderState order;
	};
	struct Member {
		uint8_t team = 0;
		uint8_t squad = 0;
	};

	Squad *find(uint8_t p_team, uint8_t p_squad);
	const Squad *find(uint8_t p_team, uint8_t p_squad) const;
	void put(uint8_t p_slot, uint8_t p_team, uint8_t p_squad);
	uint8_t first_empty(uint8_t p_team) const;

	std::array<std::array<Squad, net::kMaxSquads>, kSquadTeams> squads_{};
	std::vector<Member> members_;
	uint8_t size_ = kDefaultSquadSize;
	uint64_t revision_ = 0;
};

}
