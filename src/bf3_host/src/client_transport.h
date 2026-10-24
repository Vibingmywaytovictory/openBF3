#pragma once

#include "bf3/net/enet_host.h"

#include <cstdint>
#include <functional>
#include <span>

namespace bf3::host {

class ClientTransport {
public:
	virtual ~ClientTransport() = default;
	virtual int service(uint32_t p_wait_ms, net::Host::Event &r_event) = 0;
	virtual void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) = 0;
	virtual void flush() = 0;
	virtual void disconnect_all() = 0;
	virtual uint32_t rtt_ms() const = 0;
};

using MonotonicClock = std::function<uint64_t()>;

}
