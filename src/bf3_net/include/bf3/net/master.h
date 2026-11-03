#pragma once

#include "bf3/core/result.h"
#include "bf3/net/protocol.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bf3::net {

class Host;

inline constexpr uint16_t kMasterDefaultPort = 25250;
inline constexpr uint32_t kMasterHeartbeatMs = 30000;
inline constexpr uint32_t kMasterMissedHeartbeats = 3;
inline constexpr uint32_t kMasterExchangeMs = 5000;
inline constexpr size_t kMasterNameBytes = 32;
inline constexpr size_t kMasterIdentBytes = 64;
inline constexpr size_t kMasterDetailBytes = 128;
inline constexpr size_t kMasterMaxEntries = 256;
inline constexpr size_t kMasterMaxPerAddress = 8;
inline constexpr size_t kMasterMaxRequestBytes = 512;
inline constexpr size_t kMasterMaxListBytes = 64 * 1024;

struct Endpoint {
	std::string host;
	uint16_t port = 0;

	bool operator==(const Endpoint &) const = default;
};

core::Result<Endpoint> parse_endpoint(std::string_view p_text);

struct ConnectTarget {
	std::string name;
	Endpoint at;
};

core::Result<ConnectTarget> parse_connect_target(std::string_view p_text);
bool valid_server_name(std::string_view p_name);

struct MasterHeartbeat {
	std::string name;
	uint16_t port = 0;
	std::string level;
	std::string mode;
	uint8_t players = 0;
	uint8_t bots = 0;
	uint8_t max_players = 0;
	uint16_t protocol = kProtocolVersion;
	uint64_t build = 0;

	bool operator==(const MasterHeartbeat &) const = default;
};

struct MasterEntry {
	MasterHeartbeat server;
	std::string address;
	uint32_t age_s = 0;

	bool operator==(const MasterEntry &) const = default;
};

enum class MasterRefusal : uint8_t {
	None = 0,
	Malformed = 1,
	Full = 2,
	AddressFull = 3,
	NameTaken = 4,
	Unexpected = 5,
};
inline constexpr size_t kMasterRefusalCount = 6;
const char *master_refusal_name(MasterRefusal p_refusal);

struct MasterAck {
	MasterRefusal refusal = MasterRefusal::None;
	std::string detail;

	bool operator==(const MasterAck &) const = default;
};

enum class MasterKind : uint8_t {
	Heartbeat = 1,
	Ack = 2,
	Query = 3,
	List = 4,
};

std::vector<uint8_t> encode_master_heartbeat(const MasterHeartbeat &p_message);
core::Result<MasterHeartbeat> decode_master_heartbeat(std::span<const uint8_t> p_datagram);
std::vector<uint8_t> encode_master_ack(const MasterAck &p_message);
core::Result<MasterAck> decode_master_ack(std::span<const uint8_t> p_datagram);
std::vector<uint8_t> encode_master_query();
core::Status decode_master_query(std::span<const uint8_t> p_datagram);
std::vector<uint8_t> encode_master_list(const std::vector<MasterEntry> &p_entries);
core::Result<std::vector<MasterEntry>> decode_master_list(std::span<const uint8_t> p_datagram);
std::optional<MasterKind> peek_master_kind(std::span<const uint8_t> p_datagram);

struct MasterRegistryConfig {
	uint32_t heartbeat_ms = kMasterHeartbeatMs;
	uint32_t missed = kMasterMissedHeartbeats;
	size_t max_entries = kMasterMaxEntries;
	size_t max_per_address = kMasterMaxPerAddress;
};

struct MasterVerdict {
	MasterRefusal refusal = MasterRefusal::None;
	bool refreshed = false;
	std::string detail;
};

struct MasterRegistryStats {
	uint64_t registered = 0;
	uint64_t refreshed = 0;
	uint64_t expired = 0;
	uint64_t refused = 0;
	uint64_t queries = 0;
	std::array<uint64_t, kMasterRefusalCount> refusals{};
};

class MasterRegistry {
public:
	explicit MasterRegistry(MasterRegistryConfig p_config = {});

	MasterVerdict heartbeat(uint64_t p_now_ms, std::string_view p_address, std::span<const uint8_t> p_datagram);
	MasterVerdict refuse(MasterRefusal p_refusal, std::string p_detail);
	size_t expire(uint64_t p_now_ms);
	std::vector<MasterEntry> list(uint64_t p_now_ms);
	std::vector<MasterEntry> query(uint64_t p_now_ms);
	size_t size() const { return entries_.size(); }
	uint64_t window_ms() const { return uint64_t(config_.heartbeat_ms) * config_.missed; }
	const MasterRegistryStats &stats() const { return stats_; }

private:
	struct Slot {
		MasterHeartbeat server;
		uint64_t seen_ms = 0;
	};
	using Key = std::pair<std::string, uint16_t>;

	bool live(const Slot &p_slot, uint64_t p_now_ms) const;

	MasterRegistryConfig config_;
	std::map<Key, Slot> entries_;
	MasterRegistryStats stats_;
};

struct MasterServerConfig {
	std::string bind_ip = "127.0.0.1";
	uint16_t port = kMasterDefaultPort;
	size_t max_peers = 64;
	uint32_t idle_ms = kMasterExchangeMs;
	MasterRegistryConfig registry;
};

struct MasterServerStats {
	MasterRegistryStats registry;
	size_t servers = 0;
	uint64_t connections = 0;
	uint64_t idle_drops = 0;
};

class MasterServer {
public:
	~MasterServer();
	MasterServer(const MasterServer &) = delete;
	MasterServer &operator=(const MasterServer &) = delete;

	static core::Result<std::unique_ptr<MasterServer>> start(const MasterServerConfig &p_config);

	uint16_t port() const;
	void service(uint32_t p_timeout_ms, uint64_t p_now_ms);
	std::vector<MasterEntry> list(uint64_t p_now_ms) { return registry_.list(p_now_ms); }
	MasterServerStats stats() const;

private:
	explicit MasterServer(const MasterServerConfig &p_config);
	void answer(uint32_t p_peer, std::span<const uint8_t> p_datagram, uint64_t p_now_ms);

	MasterServerConfig config_;
	std::unique_ptr<Host> host_;
	MasterRegistry registry_;
	std::vector<uint64_t> connected_at_;
	uint64_t connections_ = 0;
	uint64_t idle_drops_ = 0;
};

core::Result<std::vector<uint8_t>> master_exchange(const Endpoint &p_master, std::span<const uint8_t> p_request,
		uint32_t p_timeout_ms = kMasterExchangeMs, const std::atomic<bool> *p_cancel = nullptr);
core::Result<MasterAck> send_master_heartbeat(const Endpoint &p_master, const MasterHeartbeat &p_heartbeat,
		uint32_t p_timeout_ms = kMasterExchangeMs, const std::atomic<bool> *p_cancel = nullptr);
core::Result<std::vector<MasterEntry>> query_master(const Endpoint &p_master, uint32_t p_timeout_ms = kMasterExchangeMs,
		const std::atomic<bool> *p_cancel = nullptr);
Endpoint entry_endpoint(const MasterEntry &p_entry, const Endpoint &p_master);
core::Result<Endpoint> find_server(
		const std::vector<MasterEntry> &p_entries, std::string_view p_name, const Endpoint &p_master);
core::Result<Endpoint> resolve_connect_target(const ConnectTarget &p_target, uint32_t p_timeout_ms = kMasterExchangeMs,
		const std::atomic<bool> *p_cancel = nullptr);

struct MasterHeartbeatStats {
	uint64_t sent = 0;
	uint64_t acked = 0;
	uint64_t refused = 0;
	uint64_t failed = 0;
	MasterRefusal last_refusal = MasterRefusal::None;
	std::string last_error;
};

class MasterHeartbeater {
public:
	using Source = std::function<MasterHeartbeat()>;

	MasterHeartbeater() = default;
	~MasterHeartbeater();
	MasterHeartbeater(const MasterHeartbeater &) = delete;
	MasterHeartbeater &operator=(const MasterHeartbeater &) = delete;

	core::Status start(Endpoint p_master, Source p_source, uint32_t p_interval_ms = kMasterHeartbeatMs,
			uint32_t p_timeout_ms = kMasterExchangeMs);
	void stop();
	bool running() const { return thread_.joinable(); }
	MasterHeartbeatStats stats() const;

private:
	void run();

	Endpoint master_;
	Source source_;
	uint32_t interval_ms_ = kMasterHeartbeatMs;
	uint32_t timeout_ms_ = kMasterExchangeMs;
	std::thread thread_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	bool stop_ = false;
	std::atomic<bool> cancel_{ false };
	MasterHeartbeatStats stats_;
};

}
