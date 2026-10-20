#pragma once

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

	void retire(T *p_instance) {
		std::unique_lock lock(mutex_);
		erase_one(live_, p_instance, true);
		unpinned_.wait(lock, [&] { return std::find(pinned_.begin(), pinned_.end(), p_instance) == pinned_.end(); });
	}

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
	std::vector<T *> live_;
	std::vector<T *> pinned_;
};

}
