#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

namespace bf3::core {

class WorkerGroup {
public:
	static constexpr unsigned kMaxLanes = 32;

	explicit WorkerGroup(unsigned p_lanes, std::string_view p_name = "worker");

	~WorkerGroup();
	WorkerGroup(const WorkerGroup &) = delete;
	WorkerGroup &operator=(const WorkerGroup &) = delete;

	unsigned lanes() const;

	bool kick(size_t p_count, std::function<void(size_t)> p_work);

	double wait();

	bool busy() const;

private:
	struct Impl;
	Impl *impl_ = nullptr;
};

}
