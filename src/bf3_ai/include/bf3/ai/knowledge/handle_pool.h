#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::ai {

inline constexpr uint32_t kHandleIndexMask = 0xFFFFu;
inline constexpr uint32_t kHandleGenerationMask = 0x7FFFu;
inline constexpr uint32_t kHandlePoolCapacity = 65535u;

template <class T> class HandlePool {
public:
	uint32_t acquire(std::unique_ptr<T> p_item) {
		uint32_t index = 0;
		if (!free_.empty()) {
			index = free_.back();
			free_.pop_back();
		} else {
			if (slots_.size() >= kHandlePoolCapacity) {
				return 0;
			}
			slots_.push_back({});
			index = uint32_t(slots_.size());
		}
		Slot &slot = slots_[index - 1];
		slot.item = std::move(p_item);
		slot.live = true;
		return (uint32_t(slot.generation) << 16) | index;
	}

	T *get(uint32_t p_handle) const {
		const Slot *slot = find(p_handle);
		return slot ? slot->item.get() : nullptr;
	}

	bool set(uint32_t p_handle, std::unique_ptr<T> p_item) {
		Slot *slot = find(p_handle);
		if (slot == nullptr) {
			return false;
		}
		slot->item = std::move(p_item);
		return true;
	}

	bool release(uint32_t p_handle) {
		Slot *slot = find(p_handle);
		if (slot == nullptr) {
			return false;
		}
		slot->item.reset();
		slot->live = false;
		slot->generation = uint16_t((slot->generation + 1) & kHandleGenerationMask);
		free_.push_back(p_handle & kHandleIndexMask);
		return true;
	}

	bool swap(uint32_t p_a, uint32_t p_b) {
		Slot *a = find(p_a);
		Slot *b = find(p_b);
		if (a == nullptr || b == nullptr) {
			return false;
		}
		std::swap(a->item, b->item);
		return true;
	}

	void reset() {
		slots_.clear();
		free_.clear();
	}

	size_t live_count() const { return slots_.size() - free_.size(); }

private:
	struct Slot {
		std::unique_ptr<T> item;
		uint16_t generation = 0;
		bool live = false;
	};

	Slot *find(uint32_t p_handle) { return const_cast<Slot *>(static_cast<const HandlePool *>(this)->find(p_handle)); }

	const Slot *find(uint32_t p_handle) const {
		const uint32_t index = p_handle & kHandleIndexMask;
		if (p_handle == kNoHandle || index == 0 || index > slots_.size()) {
			return nullptr;
		}
		const Slot &slot = slots_[index - 1];
		if (!slot.live || slot.generation != uint16_t(p_handle >> 16)) {
			return nullptr;
		}
		return &slot;
	}

	std::vector<Slot> slots_;
	std::vector<uint32_t> free_;
};

}
