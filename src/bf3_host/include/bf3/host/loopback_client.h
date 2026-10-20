#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace bf3::host {

struct LoopbackClientStats {
	bool running = false;
	bool connected = false;
	bool failed = false;
	std::string error;
	uint64_t pings_sent = 0;
	uint64_t pongs_received = 0;
	double last_rtt_ms = 0.0;
	double mean_rtt_ms = 0.0;
	double max_rtt_ms = 0.0;
	uint32_t enet_rtt_ms = 0;
};

class LoopbackClient {
public:
	LoopbackClient() = default;
	~LoopbackClient();
	LoopbackClient(const LoopbackClient &) = delete;
	LoopbackClient &operator=(const LoopbackClient &) = delete;

	bool start(uint16_t p_port, int p_ping_hz = 10, std::string *r_error = nullptr);
	void stop();
	LoopbackClientStats stats() const;

	static size_t stop_all();

private:
	void run(uint16_t p_port, int p_ping_hz);

	std::thread thread_;
	std::mutex lifecycle_mutex_;
	std::atomic<bool> stop_requested_{ false };
	std::atomic<bool> running_{ false };
	std::atomic<bool> connected_{ false };
	std::atomic<bool> failed_{ false };
	mutable std::mutex error_mutex_;
	std::string error_;
	std::atomic<uint64_t> sent_{ 0 };
	std::atomic<uint64_t> received_{ 0 };
	std::atomic<uint64_t> rtt_sum_us_{ 0 };
	std::atomic<uint32_t> last_rtt_us_{ 0 };
	std::atomic<uint32_t> max_rtt_us_{ 0 };
	std::atomic<uint32_t> enet_rtt_ms_{ 0 };
};

}
