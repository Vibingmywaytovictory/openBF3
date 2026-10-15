#pragma once
// InstanceRegistry<T> (H hygiene): the process-wide list of running ServerHost / LoopbackClient / ClientSession
// instances that their static stop_all() stops (the extension deinit safety net, register_types.cpp).
//
// The race it closes: stop_all used to copy the pointer list under the registry mutex, release it and then call
// stop() through each pointer, while the owner could destroy that instance on another thread in between (or while
// stop_all waited on the instance's lifecycle mutex): a use after free of the instance, its thread object and its
// mutex. Here stop_all pins every instance it took from the list under the same mutex the destructor's retire()
// takes, and retire() waits until no stop_all holds a pin on the instance, so an instance cannot be freed while a
// stop_all call is inside it. An instance retired before stop_all reaches it is skipped (its own destructor stops
// it).
//
// Rules for T:
//   * add(this) once its thread runs (start), remove(this) once it no longer does (stop). remove() never waits:
//     stop() is what stop_all calls on a pinned instance.
//   * The destructor calls retire(this) before anything else (before stop() and before members go away).
//   * Lock order: an instance's lifecycle mutex, then the registry mutex. The registry mutex is never held while
//     calling into an instance, so stop() may call remove() while stop_all is pinning or unpinning others.
// An instance started while a stop_all call runs is not stopped by that call.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace bf3::host::detail {

template <class T>
class InstanceRegistry {
public:
	void add(T *p_instance) {
		std::lock_guard lock(mutex_);
		live_.push_back(p_instance);
	}

	void remove(T *p_instance) {
		std::lock_guard lock(mutex_);
		erase_one(live_, p_instance, true);
	}

	// From the destructor: no stop_all can reach p_instance after this returns.
	void retire(T *p_instance) {
		std::unique_lock lock(mutex_);
		erase_one(live_, p_instance, true);
		unpinned_.wait(lock, [&] { return std::find(pinned_.begin(), pinned_.end(), p_instance) == pinned_.end(); });
	}

	// Calls p_fn(T *) on every instance registered when the call starts and still registered (not stopped or
	// retired meanwhile) when p_fn reaches it. Returns how many it called.
	template <class F>
	size_t for_each_pinned(F &&p_fn) {
		std::vector<T *> batch;
		{
			std::lock_guard lock(mutex_);
			batch = live_;
			pinned_.insert(pinned_.end(), batch.begin(), batch.end());
		}
		size_t called = 0;
		for (T *instance : batch) {
			bool alive = false;
			{
				std::lock_guard lock(mutex_);
				// Retired meanwhile (its destructor waits for this pin and stops it itself) or stopped meanwhile.
				alive = std::find(live_.begin(), live_.end(), instance) != live_.end();
			}
			if (alive) {
				p_fn(instance);
				++called;
			}
			{
				std::lock_guard lock(mutex_);
				erase_one(pinned_, instance, false);
			}
			unpinned_.notify_all();
		}
		return called;
	}

	size_t size() {
		std::lock_guard lock(mutex_);
		return live_.size();
	}

private:
	static void erase_one(std::vector<T *> &r_list, T *p_instance, bool p_all) {
		if (p_all) {
			r_list.erase(std::remove(r_list.begin(), r_list.end(), p_instance), r_list.end());
			return;
		}
		const auto it = std::find(r_list.begin(), r_list.end(), p_instance);
		if (it != r_list.end()) {
			r_list.erase(it);
		}
	}

	std::mutex mutex_;
	std::condition_variable unpinned_;
	std::vector<T *> live_; // running instances
	std::vector<T *> pinned_; // one entry per (stop_all call, instance) in progress
};

} // namespace bf3::host::detail
