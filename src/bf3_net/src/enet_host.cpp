#include "bf3/net/enet_host.h"

#include <enet/enet.h>

#include <mutex>

namespace bf3::net {

namespace {
std::mutex g_runtime_mutex;
int g_runtime_users = 0;
}

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
	ENetHost *h = p_channels <= kCountedChannels ? enet_host_create(&addr, p_max_peers, p_channels, 0, 0) : nullptr;
	if (h == nullptr) {
		runtime_release();
		if (r_error) {
			*r_error = p_channels <= kCountedChannels ? "enet_host_create (server) failed; port in use?"
													  : "more channels than are counted";
		}
		return nullptr;
	}
	std::unique_ptr<Host> host = wrap(h, p_channels);
	host->port_ = h->address.port;
	return host;
}

std::unique_ptr<Host> Host::create_client(size_t p_channels, std::string *r_error) {
	if (!runtime_acquire()) {
		if (r_error) {
			*r_error = "enet_initialize failed";
		}
		return nullptr;
	}
	ENetHost *h = p_channels <= kCountedChannels ? enet_host_create(nullptr, 1, p_channels, 0, 0) : nullptr;
	if (h == nullptr) {
		runtime_release();
		if (r_error) {
			*r_error = p_channels <= kCountedChannels ? "enet_host_create (client) failed"
													  : "more channels than are counted";
		}
		return nullptr;
	}
	return wrap(h, p_channels);
}

std::unique_ptr<Host> Host::wrap(void *p_host, size_t p_channels) {
	std::unique_ptr<Host> host(new Host());
	host->host_ = p_host;
	host->channels_ = p_channels;
	host->counters_.resize(static_cast<ENetHost *>(p_host)->peerCount);
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
			if (r_event.peer < counters_.size()) {
				counters_[r_event.peer] = PeerCounters();
			}
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			r_event.type = EventType::Disconnect;
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			r_event.type = EventType::Receive;
			r_event.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
			if (r_event.peer < counters_.size() && ev.channelID < kCountedChannels) {
				ChannelCounters &c = counters_[r_event.peer].received[ev.channelID];
				c.packets++;
				c.bytes += command_bytes(r_event.peer, ev.packet->dataLength, ev.packet->flags);
			}
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
	return fragment_bytes(p_peer);
}

size_t Host::fragment_bytes(uint32_t p_peer) const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
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
	if (p_channel < kCountedChannels) {
		ChannelCounters &c = counters_[p_peer].sent[p_channel];
		c.packets++;
		c.bytes += command_bytes(p_peer, p_size, flags);
	}
	return true;
}

size_t Host::command_bytes(uint32_t p_peer, size_t p_size, uint32_t p_packet_flags) const {
	SendMode mode = SendMode::Sequenced;
	if ((p_packet_flags & ENET_PACKET_FLAG_RELIABLE) != 0) {
		mode = SendMode::Reliable;
	} else if ((p_packet_flags & ENET_PACKET_FLAG_UNSEQUENCED) != 0) {
		mode = SendMode::Unsequenced;
	}
	return enet_command_bytes(p_size, mode, fragment_bytes(p_peer));
}

size_t enet_default_fragment_bytes() {
	return ENET_HOST_DEFAULT_MTU - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment);
}

size_t enet_command_bytes(size_t p_size, Host::SendMode p_mode, size_t p_fragment) {
	if (p_fragment > 0 && p_size > p_fragment) {
		return p_size + ((p_size + p_fragment - 1) / p_fragment) * sizeof(ENetProtocolSendFragment);
	}
	if (p_mode == Host::SendMode::Reliable) {
		return p_size + sizeof(ENetProtocolSendReliable);
	}
	if (p_mode == Host::SendMode::Unsequenced) {
		return p_size + sizeof(ENetProtocolSendUnsequenced);
	}
	return p_size + sizeof(ENetProtocolSendUnreliable);
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

bool Host::disconnect_later(uint32_t p_peer) {
	ENetHost *h = static_cast<ENetHost *>(host_);
	if (p_peer >= h->peerCount || h->peers[p_peer].state != ENET_PEER_STATE_CONNECTED) {
		return false;
	}
	enet_peer_disconnect_later(&h->peers[p_peer], 0);
	return true;
}

bool Host::set_peer_timeout(uint32_t p_peer, uint32_t p_limit, uint32_t p_minimum_ms, uint32_t p_maximum_ms) {
	ENetHost *h = static_cast<ENetHost *>(host_);
	if (p_peer >= h->peerCount) {
		return false;
	}
	enet_peer_timeout(&h->peers[p_peer], p_limit, p_minimum_ms, p_maximum_ms);
	return true;
}

bool Host::peer_connected(uint32_t p_peer) const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
	return p_peer < h->peerCount && h->peers[p_peer].state == ENET_PEER_STATE_CONNECTED;
}

PeerCounters Host::peer_counters(uint32_t p_peer) const {
	return p_peer < counters_.size() ? counters_[p_peer] : PeerCounters();
}

WireTotals Host::wire_totals() const {
	const ENetHost *h = static_cast<const ENetHost *>(host_);
	WireTotals t;
	t.sent_bytes = h->totalSentData;
	t.sent_datagrams = h->totalSentPackets;
	t.received_bytes = h->totalReceivedData;
	t.received_datagrams = h->totalReceivedPackets;
	return t;
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

}
