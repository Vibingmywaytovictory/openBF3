#include "bf3/net/enet_host.h"

#include <enet/enet.h>

#include <mutex>

namespace bf3::net {

namespace {
std::mutex g_runtime_mutex;
int g_runtime_users = 0;
} // namespace

bool runtime_acquire() {
	std::lock_guard lock(g_runtime_mutex);
	if (g_runtime_users == 0 && enet_initialize() != 0) {
		return false;
	}
	g_runtime_users++;
	return true;
}

void runtime_release() {
	std::lock_guard lock(g_runtime_mutex);
	if (g_runtime_users > 0 && --g_runtime_users == 0) {
		enet_deinitialize();
	}
}

std::string enet_version() {
	const ENetVersion v = enet_linked_version();
	return std::to_string(ENET_VERSION_GET_MAJOR(v)) + "." + std::to_string(ENET_VERSION_GET_MINOR(v)) + "." +
			std::to_string(ENET_VERSION_GET_PATCH(v));
}

std::unique_ptr<Host> Host::create_server(const char *p_bind_ip, uint16_t p_port, size_t p_max_peers, size_t p_channels,
		std::string *r_error) {
	if (!runtime_acquire()) {
		if (r_error) {
			*r_error = "enet_initialize failed";
		}
		return nullptr;
	}
	ENetAddress addr{};
	if (enet_address_set_host_ip(&addr, p_bind_ip) != 0) {
		runtime_release();
		if (r_error) {
			*r_error = std::string("bad bind address ") + p_bind_ip;
		}
		return nullptr;
	}
	addr.port = p_port;
	ENetHost *h = enet_host_create(&addr, p_max_peers, p_channels, 0, 0);
	if (h == nullptr) {
		runtime_release();
		if (r_error) {
			*r_error = "enet_host_create (server) failed; port in use?";
		}
		return nullptr;
	}
	std::unique_ptr<Host> host(new Host());
	host->host_ = h;
	host->channels_ = p_channels;
	host->port_ = h->address.port; // enet_host_create reads the bound address back
	return host;
}

std::unique_ptr<Host> Host::create_client(size_t p_channels, std::string *r_error) {
	if (!runtime_acquire()) {
		if (r_error) {
			*r_error = "enet_initialize failed";
		}
		return nullptr;
	}
	ENetHost *h = enet_host_create(nullptr, 1, p_channels, 0, 0);
	if (h == nullptr) {
		runtime_release();
		if (r_error) {
			*r_error = "enet_host_create (client) failed";
		}
		return nullptr;
	}
	std::unique_ptr<Host> host(new Host());
	host->host_ = h;
	host->channels_ = p_channels;
	return host;
}

Host::~Host() {
	if (host_ != nullptr) {
		enet_host_destroy(static_cast<ENetHost *>(host_));
		host_ = nullptr;
		runtime_release();
	}
}

int Host::connect(const char *p_host, uint16_t p_port) {
	ENetAddress addr{};
	if (enet_address_set_host_ip(&addr, p_host) != 0 && enet_address_set_host(&addr, p_host) != 0) {
		return -1;
	}
	addr.port = p_port;
	ENetHost *h = static_cast<ENetHost *>(host_);
	ENetPeer *peer = enet_host_connect(h, &addr, channels_, 0);
	return peer ? static_cast<int>(peer - h->peers) : -1;
}

int Host::service(uint32_t p_timeout_ms, Event &r_event) {
	ENetHost *h = static_cast<ENetHost *>(host_);
	ENetEvent ev{};
	const int r = enet_host_service(h, &ev, p_timeout_ms);
	if (r <= 0) {
		return r;
	}
	r_event = Event();
	r_event.peer = ev.peer ? static_cast<uint32_t>(ev.peer - h->peers) : 0;
	r_event.channel = ev.channelID;
	switch (ev.type) {
		case ENET_EVENT_TYPE_CONNECT:
			r_event.type = EventType::Connect;
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			r_event.type = EventType::Disconnect;
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			r_event.type = EventType::Receive;
			r_event.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
			enet_packet_destroy(ev.packet);
			break;
		default:
			r_event.type = EventType::None;
			break;
	}
	return 1;
}

bool Host::send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, bool p_reliable) {
	return send(p_peer, p_channel, p_data, p_size, p_reliable ? SendMode::Reliable : SendMode::Sequenced);
}

size_t Host::max_unfragmented_bytes(uint32_t p_peer) const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
	if (p_peer >= h->peerCount || h->peers[p_peer].state != ENET_PEER_STATE_CONNECTED) {
		return 0;
	}
	// enet_peer_send's fragment threshold (peer.c): the MTU minus the protocol and fragment headers, minus the
	// checksum when the host uses one.
	size_t n = h->peers[p_peer].mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment);
	if (h->checksum != nullptr) {
		n -= sizeof(enet_uint32);
	}
	return n;
}

bool Host::send(uint32_t p_peer, uint8_t p_channel, const void *p_data, size_t p_size, SendMode p_mode) {
	ENetHost *h = static_cast<ENetHost *>(host_);
	if (p_peer >= h->peerCount || h->peers[p_peer].state != ENET_PEER_STATE_CONNECTED) {
		return false;
	}
	if (p_mode == SendMode::Unsequenced && p_size > max_unfragmented_bytes(p_peer)) {
		return false;
	}
	enet_uint32 flags = 0;
	if (p_mode == SendMode::Reliable) {
		flags = ENET_PACKET_FLAG_RELIABLE;
	} else if (p_mode == SendMode::Unsequenced) {
		flags = ENET_PACKET_FLAG_UNSEQUENCED;
	}
	ENetPacket *pkt = enet_packet_create(p_data, p_size, flags);
	if (pkt == nullptr) {
		return false;
	}
	if (enet_peer_send(&h->peers[p_peer], p_channel, pkt) != 0) {
		enet_packet_destroy(pkt);
		return false;
	}
	return true;
}

void Host::flush() {
	enet_host_flush(static_cast<ENetHost *>(host_));
}

void Host::disconnect_all() {
	ENetHost *h = static_cast<ENetHost *>(host_);
	for (size_t i = 0; i < h->peerCount; ++i) {
		if (h->peers[i].state == ENET_PEER_STATE_CONNECTED) {
			enet_peer_disconnect_now(&h->peers[i], 0);
		}
	}
	enet_host_flush(h);
}

uint32_t Host::peer_rtt_ms(uint32_t p_peer) const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
	return p_peer < h->peerCount ? h->peers[p_peer].roundTripTime : 0;
}

size_t Host::connected_peers() const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
	size_t n = 0;
	for (size_t i = 0; i < h->peerCount; ++i) {
		n += h->peers[i].state == ENET_PEER_STATE_CONNECTED ? 1 : 0;
	}
	return n;
}

} // namespace bf3::net
