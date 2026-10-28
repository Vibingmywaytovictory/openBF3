#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace bf3::core {

struct JobQueueStats {
	uint64_t posted = 0;
	uint64_t done = 0;
	uint32_t pending = 0;
	double busy_seconds = 0.0;
};

class JobQueue {
public:
	explicit JobQueue(std::string_view p_name = "jobs");
	~JobQueue();
	JobQueue(const JobQueue &) = delete;
	JobQueue &operator=(const JobQueue &) = delete;

	bool post(std::function<void()> p_job);
	void stop();
	JobQueueStats stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
