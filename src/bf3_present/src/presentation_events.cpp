#include "bf3/present/presentation_events.h"

#include <algorithm>

namespace bf3::present {

bool PresentationEvents::push(const PresentEvent &p_event) {
	const uint64_t head = head_.load(std::memory_order_relaxed);
	const uint64_t tail = tail_.load(std::memory_order_acquire);
	if (head - tail >= kPresentationEventCapacity) {
		overflows_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	ring_[size_t(head % kPresentationEventCapacity)] = p_event;
	head_.store(head + 1, std::memory_order_release);
	return true;
}

size_t PresentationEvents::drain(std::span<PresentEvent> r_out) {
	const uint64_t tail = tail_.load(std::memory_order_relaxed);
	const uint64_t head = head_.load(std::memory_order_acquire);
	const size_t n = size_t(std::min<uint64_t>(head - tail, r_out.size()));
	for (size_t i = 0; i < n; ++i) {
		r_out[i] = ring_[size_t((tail + i) % kPresentationEventCapacity)];
	}
	tail_.store(tail + n, std::memory_order_release);
	return n;
}

uint64_t PresentationEvents::overflows() const {
	return overflows_.load(std::memory_order_relaxed);
}

}
