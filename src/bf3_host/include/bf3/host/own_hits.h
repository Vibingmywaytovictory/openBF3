#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bf3::host {

struct OwnHitCounts {
	uint64_t predicted = 0;
	uint64_t confirmed = 0;
	uint64_t same_zone = 0;
	uint64_t unpredicted = 0;
};
static_assert(std::is_trivially_copyable_v<OwnHitCounts>);

class OwnHitLedger {
public:
	static constexpr size_t kShots = 4096;

	void predicted(uint32_t p_shot, uint8_t p_target, uint8_t p_zone);
	void confirmed(uint32_t p_shot, uint8_t p_victim, uint8_t p_zone);
	const OwnHitCounts &counts() const { return counts_; }

private:
	struct Shot {
		bool used = false;
		bool predicted = false;
		bool confirmed = false;
		bool unpredicted = false;
		uint32_t shot = 0;
		uint8_t target = 0;
		uint8_t zone = 0;
	};
	Shot &at(uint32_t p_shot);

	std::array<Shot, kShots> shots_{};
	OwnHitCounts counts_;
};

}
