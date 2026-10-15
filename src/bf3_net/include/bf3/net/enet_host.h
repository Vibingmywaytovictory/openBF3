#pragma once
// Thin wrapper over upstream ENet 1.3.18 (plan §2.2 bf3_net). One Host is used by one thread.
// ENet headers stay inside bf3_net (they pull in winsock2.h).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::net {

// Process-wide enet_initialize/enet_deinitialize, reference counted. Host does this itself.
bool runtime_acquire();
void runtime_release();
std::string enet_version();

class Host {
public:
	enum class EventType { None, Connect, Disconnect, Receive };
	struct Event {
		EventType type = EventType::None;
		uint32_t peer = 0; // index of the peer in this host
		uint8_t channel = 0;
		std::vector<uint8_t> data;
	};

	// p_bind_ip "127.0.0.1" / "0.0.0.0"; p_port 0 picks a free port (see bound_port()).
	static std::unique_ptr<Host> create_server(const char *p_bind_ip, uint16_t p_port, size_t p_max_peers, size_t p_channels,
			std::string *r_error = nullptr);
	static std::unique_ptr<Host> create_client(size_t p_channels, std::string *r_error = nullptr);
	~Host();
	Host(const Host &) = delete;
	Host &operator=(const Host &) = delete;

	uint16_t bound_port() const { return port_; }
	// Starts a connection; returns the peer index or -1.
	int connect(const char *p_host, uint16_t p_port);
	// Waits up to p_timeout_ms for one event. Returns 1 with r_event filled, 0 if none, -1 on error.
	int service(uint32_t p_timeout_ms, Event &r_event);
	bool send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, bool p_reliable);
	// M1 (docs/design/m1.md §7.1): how a packet goes out. Sequenced is ENet's plain unreliable mode (flag 0: a
	// packet older than the newest one received on its channel is dropped), what send(..., false) does.
	// Unsequenced (ENET_PACKET_FLAG_UNSEQUENCED) delivers every packet that arrives, late or not: channel 0's
	// snapshots and command packets, where a late CommandPacket still carries commands the server has not seen.
	// An unsequenced packet longer than one ENet fragment is refused (returns false) instead of being sent as
	// reliable fragments, which is what ENet does with an oversized unreliable packet.
	enum class SendMode : uint8_t { Sequenced, Reliable, Unsequenced };
	bool send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, SendMode p_mode);
	// The largest packet that leaves as one unfragmented ENet command to this peer (0 when not connected).
	size_t max_unfragmented_bytes(uint32_t p_peer) const;
	void flush();
	void disconnect_all();
	uint32_t peer_rtt_ms(uint32_t p_peer) const; // ENet's smoothed round trip time
	size_t connected_peers() const;

private:
	Host() = default;
	void *host_ = nullptr; // ENetHost*
	size_t channels_ = 0;
	uint16_t port_ = 0;
};

} // namespace bf3::net
