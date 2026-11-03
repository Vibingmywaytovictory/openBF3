#include "upnp_mapping.h"

#include "godot_util.h"

#include "bf3/core/util/format.h"

#include <godot_cpp/classes/upnp_device.hpp>

namespace bf3::godot_side {

using namespace godot;

UpnpMapping::~UpnpMapping() {
	close();
}

bf3::core::Status UpnpMapping::open(const std::string &p_bind_ip, uint16_t p_port) {
	if (port_ != 0) {
		return bf3::core::make_error(bf3::core::fmt("UPnP: port %u is already mapped", unsigned(port_)));
	}
	if (p_bind_ip.rfind("127.", 0) == 0 || p_bind_ip == "localhost") {
		return bf3::core::make_error("UPnP: the server binds " + p_bind_ip +
				", which the router cannot reach; give --bind=0.0.0.0 or this machine's LAN address");
	}
	if (p_port == 0) {
		return bf3::core::make_error("UPnP: no port to map");
	}
	upnp_.instantiate();
	const int32_t found = upnp_->discover(kUpnpDiscoverMs, 2, "InternetGatewayDevice");
	if (found != UPNP::UPNP_RESULT_SUCCESS) {
		upnp_.unref();
		return bf3::core::make_error(bf3::core::fmt("UPnP: discover found no gateway (UPNPResult %d)", int(found)));
	}
	const Ref<UPNPDevice> gateway = upnp_->get_gateway();
	if (gateway.is_null() || !gateway->is_valid_gateway()) {
		upnp_.unref();
		return bf3::core::make_error("UPnP: no valid internet gateway answered");
	}
	const int32_t added = upnp_->add_port_mapping(p_port, p_port, kUpnpDescription, "UDP", 0);
	if (added != UPNP::UPNP_RESULT_SUCCESS) {
		upnp_.unref();
		return bf3::core::make_error(
				bf3::core::fmt("UPnP: the gateway refused UDP %u (UPNPResult %d)", unsigned(p_port), int(added)));
	}
	port_ = p_port;
	external_ = to_std(upnp_->query_external_address());
	return {};
}

void UpnpMapping::close() {
	if (port_ != 0 && upnp_.is_valid()) {
		upnp_->delete_port_mapping(port_, "UDP");
	}
	port_ = 0;
	external_.clear();
	upnp_.unref();
}

}
