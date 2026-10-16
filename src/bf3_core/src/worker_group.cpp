// Persistent worker lanes (S3 crowd; docs/design/s3.md §3). See worker_group.h.
//
// Items are pulled from one 64-bit counter tagged with the job's generation (high 32 bits) and the next index (low
// 32 bits). A lane that wakes late, after its job already ended and the next one was kicked, therefore can never
// take an item of the wrong job: its compare-exchange sees a different generation and it goes back to sleep. Each
// lane holds its own reference to the job's work function, so a new kick never replaces a function a lane is
// still calling.
#include "bf3/core/worker_group.h"

#include "win32_text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bf3::core {

struct WorkerGroup::Impl {
	using Work = std::function<void(size_t)>;

	std::vector<std::thread> threads;
	mutable std::mutex mutex;
	std::condition_variable wake; // lanes wait for a job (or stop)
	std::condition_variable done; // the owner waits for the job's end
	// Guarded by mutex.
	std::shared_ptr<const Work> work;
	size_t count = 0;
	uint32_t generation = 0; // bumped per kick
	bool stop = false;
	bool kicked = false; // a job was kicked and wait() has not returned yet
	// Lock-free.
	std::atomic<uint64_t> next{ 0 }; // generation << 32 | next item
	std::atomic<size_t> finished{ 0 };

	void lane_main(unsigned p_index, const std::string &p_name) {
#ifdef _WIN32
		const std::wstring name = detail::widen(p_name + " " + std::to_string(p_index));
		SetThreadDescription(GetCurrentThread(), name.c_str());
#else
		(void)p_index;
		(void)p_name;
#endif
		uint32_t seen = 0;
		std::unique_lock<std::mutex> lock(mutex);
		for (;;) {
			wake.wait(lock, [&] { return stop || generation != seen; });
			if (stop) {
				return;
			}
			seen = generation;
			const std::shared_ptr<const Work> job = work;
			const size_t n = count;
			lock.unlock();
			for (;;) {
				uint64_t v = next.load(std::memory_order_relaxed);
				bool took = false;
				while (uint32_t(v >> 32) == seen && size_t(uint32_t(v)) < n) {
					if (next.compare_exchange_weak(v, v + 1, std::memory_order_relaxed)) {
						took = true;
						break;
					}
				}
				if (!took) {
					break;
				}
				(*job)(size_t(uint32_t(v)));
				if (finished.fetch_add(1, std::memory_order_acq_rel) + 1 == n) {
					std::lock_guard<std::mutex> g(mutex);
					done.notify_all();
				}
			}
			lock.lock();
		}
	}
};

WorkerGroup::WorkerGroup(unsigned p_lanes, std::string_view p_name) :
		impl_(new Impl()) {
	const unsigned lanes = std::clamp(p_lanes, 1u, kMaxLanes);
	const std::string name(p_name);
	impl_->threads.reserve(lanes);
	for (unsigned i = 0; i < lanes; ++i) {
		impl_->threads.emplace_back([this, i, name] { impl_->lane_main(i, name); });
	}
}

WorkerGroup::~WorkerGroup() {
	wait();
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->stop = true;
	}
	impl_->wake.notify_all();
	for (std::thread &t : impl_->threads) {
		t.join();
	}
	delete impl_;
}

unsigned WorkerGroup::lanes() const {
	return unsigned(impl_->threads.size());
}

bool WorkerGroup::kick(size_t p_count, std::function<void(size_t)> p_work) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->kicked) {
		return false;
	}
	if (p_count == 0) {
		return true;
	}
	if (p_count > size_t(UINT32_MAX)) {
		return false; // the tagged counter holds 32-bit item indices
	}
	impl_->work = std::make_shared<const Impl::Work>(std::move(p_work));
	impl_->count = p_count;
	++impl_->generation;
	impl_->finished.store(0, std::memory_order_relaxed);
	impl_->next.store(uint64_t(impl_->generation) << 32, std::memory_order_release);
	impl_->kicked = true;
	impl_->wake.notify_all();
	return true;
}

double WorkerGroup::wait() {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	if (!impl_->kicked) {
		return 0.0;
	}
	auto complete = [&] { return impl_->finished.load(std::memory_order_acquire) >= impl_->count; };
	double blocked = 0.0;
	if (!complete()) {
		const auto t0 = std::chrono::steady_clock::now();
		impl_->done.wait(lock, complete);
		blocked = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
	impl_->kicked = false;
	impl_->work.reset(); // lanes still holding the job keep their own reference
	return blocked;
}

bool WorkerGroup::busy() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->kicked;
}

} // namespace bf3::core
