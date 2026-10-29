#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::net {

bool runtime_acquire();
void runtime_release();
std::string enet_version();

class Host {
public:
	enum class EventType { None, Connect, Disconnect, Receive };
	struct Event {
		EventType type = EventType::None;
		uint32_t peer = 0;
		uint8_t channel = 0;
		std::vector<uint8_t> data;
	};

	static std::unique_ptr<Host> create_server(const char *p_bind_ip, uint16_t p_port, size_t p_max_peers, size_t p_channels,
			std::string *r_error = nullptr);
	static std::unique_ptr<Host> create_client(size_t p_channels, std::string *r_error = nullptr);
	~Host();
	Host(const Host &) = delete;
	Host &operator=(const Host &) = delete;

	uint16_t bound_port() const { return port_; }

	int connect(const char *p_host, uint16_t p_port);

	int service(uint32_t p_timeout_ms, Event &r_event);
	bool send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, bool p_reliable);

	enum class SendMode : uint8_t { Sequenced, Reliable, Unsequenced };
	bool send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, SendMode p_mode);

	size_t max_unfragmented_bytes(uint32_t p_peer) const;
	void flush();
	void disconnect_all();
	bool disconnect_later(uint32_t p_peer);
	bool set_peer_timeout(uint32_t p_peer, uint32_t p_limit, uint32_t p_minimum_ms, uint32_t p_maximum_ms);
	uint32_t peer_rtt_ms(uint32_t p_peer) const;
	size_t connected_peers() const;
	bool peer_connected(uint32_t p_peer) const;

private:
	Host() = default;
	void *host_ = nullptr;
	size_t channels_ = 0;
	uint16_t port_ = 0;
};

}
