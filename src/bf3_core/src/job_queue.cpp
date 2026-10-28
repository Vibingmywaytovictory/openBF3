#include "bf3/core/job_queue.h"

#include "win32_text.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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

struct JobQueue::Impl {
	std::string name;
	mutable std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::function<void()>> jobs;
	JobQueueStats stats;
	bool stopped = false;
	std::thread worker;

	void run() {
#ifdef _WIN32
		const std::wstring wide = detail::widen(name);
		SetThreadDescription(GetCurrentThread(), wide.c_str());
#endif
		std::unique_lock lock(mutex);
		for (;;) {
			wake.wait(lock, [this] { return stopped || !jobs.empty(); });
			if (stopped) {
				return;
			}
			std::function<void()> job = std::move(jobs.front());
			jobs.pop_front();
			lock.unlock();
			const auto t0 = std::chrono::steady_clock::now();
			job();
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			lock.lock();
			stats.busy_seconds += seconds;
			++stats.done;
			--stats.pending;
		}
	}
};

JobQueue::JobQueue(std::string_view p_name) : impl_(std::make_unique<Impl>()) {
	impl_->name = std::string(p_name);
}

JobQueue::~JobQueue() {
	stop();
}

bool JobQueue::post(std::function<void()> p_job) {
	Impl &m = *impl_;
	{
		std::lock_guard lock(m.mutex);
		if (m.stopped || !p_job) {
			return false;
		}
		m.jobs.push_back(std::move(p_job));
		++m.stats.posted;
		++m.stats.pending;
		if (!m.worker.joinable()) {
			m.worker = std::thread([&m] { m.run(); });
		}
	}
	m.wake.notify_one();
	return true;
}

void JobQueue::stop() {
	Impl &m = *impl_;
	{
		std::lock_guard lock(m.mutex);
		m.stopped = true;
		m.stats.pending -= uint32_t(m.jobs.size());
		m.jobs.clear();
	}
	m.wake.notify_all();
	if (m.worker.joinable()) {
		m.worker.join();
	}
}

JobQueueStats JobQueue::stats() const {
	std::lock_guard lock(impl_->mutex);
	return impl_->stats;
}

}
