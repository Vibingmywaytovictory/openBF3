#pragma once

#include "bf3/core/result.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/upnp.hpp>

#include <cstdint>
#include <string>

namespace bf3::godot_side {

inline constexpr int32_t kUpnpDiscoverMs = 2000;
inline constexpr const char *kUpnpDescription = "openBF3 server";

class UpnpMapping {
public:
	UpnpMapping() = default;
	~UpnpMapping();
	UpnpMapping(const UpnpMapping &) = delete;
	UpnpMapping &operator=(const UpnpMapping &) = delete;

	bf3::core::Status open(const std::string &p_bind_ip, uint16_t p_port);
	void close();
	bool mapped() const { return port_ != 0; }
	uint16_t port() const { return port_; }
	const std::string &external_address() const { return external_; }

private:
	godot::Ref<godot::UPNP> upnp_;
	uint16_t port_ = 0;
	std::string external_;
};

}
