#include "bf3/net/master.h"

#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"
#include "bf3/net/enet_host.h"
#include "bit_archive.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace bf3::net {

namespace {

constexpr uint32_t kMasterMagic = 0x4F42334D;
constexpr size_t kMasterAddressBytes = 45;
constexpr uint64_t kNotConnected = std::numeric_limits<uint64_t>::max();

bool alnum_ascii(char p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9');
}

bool name_char(char p_c) {
	return alnum_ascii(p_c) || p_c == ' ' || p_c == '_' || p_c == '-' || p_c == '.';
}

bool ident_char(char p_c) {
	return alnum_ascii(p_c) || p_c == '_' || p_c == '/' || p_c == '-' || p_c == '.';
}

bool detail_char(char p_c) {
	return p_c >= 0x20 && p_c < 0x7F;
}

bool address_char(char p_c) {
	return (p_c >= '0' && p_c <= '9') || (p_c >= 'a' && p_c <= 'f') || (p_c >= 'A' && p_c <= 'F') || p_c == '.' ||
			p_c == ':';
}

template <class Allowed> bool all_of(std::string_view p_text, Allowed &&p_allowed) {
	return std::all_of(p_text.begin(), p_text.end(), p_allowed);
}

template <class A, MaybeConst<std::string> S, class Allowed>
void token(A &r_a, S &r_text, size_t p_max, Allowed &&p_allowed, const char *p_why) {
	uint64_t length = std::min(r_text.size(), p_max);
	r_a.varint(length);
	r_a.require(p_why, [&] { return length <= p_max && r_a.fits(length * 8); });
	length = r_a.resize(r_text, size_t(length));
	for (size_t i = 0; i < length; ++i) {
		r_a.bits(r_text[i], 8);
	}
	r_a.require(p_why, [&] { return all_of(r_text, p_allowed); });
}

const char *heartbeat_problem(const MasterHeartbeat &p_m) {
	if (!valid_server_name(p_m.name)) {
		return "bad server name";
	}
	if (p_m.port == 0) {
		return "port 0";
	}
	if (p_m.level.empty() || p_m.level.size() > kMasterIdentBytes || !all_of(p_m.level, ident_char) ||
			p_m.mode.empty() || p_m.mode.size() > kMasterIdentBytes || !all_of(p_m.mode, ident_char)) {
		return "bad level or mode name";
	}
	if (p_m.max_players == 0 || p_m.max_players > kMaxSlots) {
		return "max players out of range";
	}
	if (unsigned(p_m.players) + unsigned(p_m.bots) > p_m.max_players) {
		return "more players than max players";
	}
	return nullptr;
}

template <class A, MaybeConst<MasterHeartbeat> M> void serialize(A &r_a, M &r_m) {
	token(r_a, r_m.name, kMasterNameBytes, name_char, "bad server name");
	r_a.bits(r_m.port, 16);
	token(r_a, r_m.level, kMasterIdentBytes, ident_char, "bad level or mode name");
	token(r_a, r_m.mode, kMasterIdentBytes, ident_char, "bad level or mode name");
	r_a.bits(r_m.players, 8);
	r_a.bits(r_m.bots, 8);
	r_a.bits(r_m.max_players, 8);
	r_a.bits(r_m.protocol, 16);
	r_a.bits(r_m.build, 64);
	r_a.check("bad heartbeat", [&] { return heartbeat_problem(r_m) == nullptr; });
}

template <class A, MaybeConst<MasterAck> M> void serialize(A &r_a, M &r_m) {
	r_a.bits(r_m.refusal, 8);
	r_a.check("unknown refusal", [&] { return size_t(r_m.refusal) < kMasterRefusalCount; });
	token(r_a, r_m.detail, kMasterDetailBytes, detail_char, "bad detail text");
}

struct MasterQuery {};

template <class A, MaybeConst<MasterQuery> M> void serialize(A &, M &) {}

struct MasterList {
	std::vector<MasterEntry> entries;
};

template <class A, MaybeConst<MasterList> M> void serialize(A &r_a, M &r_m) {
	uint64_t count = r_m.entries.size();
	r_a.varint(count);
	r_a.require("too many entries", [&] { return count <= kMasterMaxEntries && r_a.fits(count * 8); });
	if (!r_a.ok()) {
		return;
	}
	count = r_a.resize(r_m.entries, size_t(count));
	for (size_t i = 0; i < count && r_a.ok(); ++i) {
		auto &e = r_m.entries[i];
		serialize(r_a, e.server);
		token(r_a, e.address, kMasterAddressBytes, address_char, "bad address");
		r_a.bits(e.age_s, 32);
	}
}

template <class M> std::vector<uint8_t> encode_kind(MasterKind p_kind, size_t p_cap, const M &p_message) {
	BitWriter w(p_cap);
	w.write_bits(kMasterMagic, 32);
	w.write_bits(uint8_t(p_kind), 8);
	BitWriteArchive archive(w);
	serialize(archive, p_message);
	if (w.overflowed()) {
		return {};
	}
	return w.take();
}

core::Error master_fail(const char *p_what, const std::string &p_why) {
	return core::make_error(std::string(p_what) + ": " + p_why);
}

template <class M>
core::Result<M> decode_kind(std::span<const uint8_t> p_datagram, MasterKind p_kind, size_t p_max, const char *p_what) {
	if (p_datagram.size() > p_max) {
		return master_fail(p_what, core::fmt("%zu bytes, more than %zu", p_datagram.size(), p_max));
	}
	BitReader r(p_datagram);
	if (r.read_bits(32) != kMasterMagic || r.failed()) {
		return master_fail(p_what, "not a master datagram");
	}
	if (r.read_bits(8) != uint8_t(p_kind) || r.failed()) {
		return master_fail(p_what, "wrong kind");
	}
	BitReadArchive archive(r);
	M message;
	serialize(archive, message);
	if (const char *why = archive.error()) {
		return master_fail(p_what, why);
	}
	if (!r.at_end()) {
		return master_fail(p_what, "trailing bytes");
	}
	return message;
}

bool loopback_host(std::string_view p_host) {
	return p_host.rfind("127.", 0) == 0 || core::iequals_ascii(p_host, "localhost") || p_host == "::1";
}

}

core::Result<Endpoint> parse_endpoint(std::string_view p_text) {
	const size_t colon = p_text.rfind(':');
	uint32_t port = 0;
	if (colon == std::string_view::npos || colon == 0 ||
			core::parse_u32_value(p_text.substr(colon + 1), 1.0, 65535.0, port).fault != core::KeyFault::None) {
		return core::make_error("'" + std::string(p_text) + "' is not host:port with a port 1..65535");
	}
	const std::string_view host = p_text.substr(0, colon);
	if (host.find_first_of("@ \t") != std::string_view::npos) {
		return core::make_error("'" + std::string(p_text) + "': bad host '" + std::string(host) + "'");
	}
	return Endpoint{ std::string(host), uint16_t(port) };
}

bool valid_server_name(std::string_view p_name) {
	return !p_name.empty() && p_name.size() <= kMasterNameBytes && all_of(p_name, name_char) && p_name.front() != ' ' &&
			p_name.back() != ' ';
}

core::Result<ConnectTarget> parse_connect_target(std::string_view p_text) {
	const size_t at = p_text.find('@');
	ConnectTarget target;
	if (at != std::string_view::npos) {
		target.name = std::string(p_text.substr(0, at));
		if (!valid_server_name(target.name)) {
			return core::make_error("'" + std::string(p_text) + "': '" + target.name +
					"' is not a server name (1 to 32 of A-Z a-z 0-9 space _ - .)");
		}
	}
	core::Result<Endpoint> endpoint = parse_endpoint(at == std::string_view::npos ? p_text : p_text.substr(at + 1));
	if (!endpoint) {
		return endpoint.error();
	}
	target.at = std::move(*endpoint);
	return target;
}

const char *master_refusal_name(MasterRefusal p_refusal) {
	switch (p_refusal) {
		case MasterRefusal::None:
			return "none";
		case MasterRefusal::Malformed:
			return "malformed";
		case MasterRefusal::Full:
			return "full";
		case MasterRefusal::AddressFull:
			return "address full";
		case MasterRefusal::NameTaken:
			return "name taken";
		case MasterRefusal::Unexpected:
			return "unexpected";
	}
	return "?";
}

std::vector<uint8_t> encode_master_heartbeat(const MasterHeartbeat &p_message) {
	if (heartbeat_problem(p_message) != nullptr) {
		return {};
	}
	return encode_kind(MasterKind::Heartbeat, kMasterMaxRequestBytes, p_message);
}

core::Result<MasterHeartbeat> decode_master_heartbeat(std::span<const uint8_t> p_datagram) {
	return decode_kind<MasterHeartbeat>(p_datagram, MasterKind::Heartbeat, kMasterMaxRequestBytes, "MasterHeartbeat");
}

std::vector<uint8_t> encode_master_ack(const MasterAck &p_message) {
	MasterAck m = p_message;
	m.detail.resize(std::min(m.detail.size(), kMasterDetailBytes));
	std::replace_if(m.detail.begin(), m.detail.end(), [](char p_c) { return !detail_char(p_c); }, '?');
	return encode_kind(MasterKind::Ack, kMasterMaxRequestBytes, m);
}

core::Result<MasterAck> decode_master_ack(std::span<const uint8_t> p_datagram) {
	return decode_kind<MasterAck>(p_datagram, MasterKind::Ack, kMasterMaxRequestBytes, "MasterAck");
}

std::vector<uint8_t> encode_master_query() {
	return encode_kind(MasterKind::Query, kMasterMaxRequestBytes, MasterQuery{});
}

core::Status decode_master_query(std::span<const uint8_t> p_datagram) {
	core::Result<MasterQuery> q =
			decode_kind<MasterQuery>(p_datagram, MasterKind::Query, kMasterMaxRequestBytes, "MasterQuery");
	if (!q) {
		return q.error();
	}
	return {};
}

std::vector<uint8_t> encode_master_list(const std::vector<MasterEntry> &p_entries) {
	if (p_entries.size() > kMasterMaxEntries) {
		return {};
	}
	return encode_kind(MasterKind::List, kMasterMaxListBytes, MasterList{ p_entries });
}

core::Result<std::vector<MasterEntry>> decode_master_list(std::span<const uint8_t> p_datagram) {
	core::Result<MasterList> list =
			decode_kind<MasterList>(p_datagram, MasterKind::List, kMasterMaxListBytes, "MasterList");
	if (!list) {
		return list.error();
	}
	return std::move(list->entries);
}

std::optional<MasterKind> peek_master_kind(std::span<const uint8_t> p_datagram) {
	if (p_datagram.size() < 5) {
		return std::nullopt;
	}
	BitReader r(p_datagram);
	if (r.read_bits(32) != kMasterMagic) {
		return std::nullopt;
	}
	const uint8_t kind = uint8_t(r.read_bits(8));
	if (kind < uint8_t(MasterKind::Heartbeat) || kind > uint8_t(MasterKind::List)) {
		return std::nullopt;
	}
	return MasterKind(kind);
}

MasterRegistry::MasterRegistry(MasterRegistryConfig p_config) : config_(p_config) {}

bool MasterRegistry::live(const Slot &p_slot, uint64_t p_now_ms) const {
	return p_now_ms < p_slot.seen_ms || p_now_ms - p_slot.seen_ms <= window_ms();
}

MasterVerdict MasterRegistry::refuse(MasterRefusal p_refusal, std::string p_detail) {
	stats_.refused++;
	stats_.refusals[size_t(p_refusal)]++;
	return MasterVerdict{ p_refusal, false, std::move(p_detail) };
}

MasterVerdict MasterRegistry::heartbeat(
		uint64_t p_now_ms, std::string_view p_address, std::span<const uint8_t> p_datagram) {
	core::Result<MasterHeartbeat> hb = decode_master_heartbeat(p_datagram);
	if (!hb) {
		return refuse(MasterRefusal::Malformed, hb.error().message);
	}
	if (p_address.empty() || p_address.size() > kMasterAddressBytes || !all_of(p_address, address_char)) {
		return refuse(MasterRefusal::Malformed, "no usable source address");
	}
	expire(p_now_ms);
	const Key key{ std::string(p_address), hb->port };
	for (const auto &[k, slot] : entries_) {
		if (k != key && core::iequals_ascii(slot.server.name, hb->name)) {
			return refuse(MasterRefusal::NameTaken,
					core::fmt("the name %s is held by %s:%u", slot.server.name.c_str(), k.first.c_str(),
							unsigned(k.second)));
		}
	}
	if (auto it = entries_.find(key); it != entries_.end()) {
		it->second = Slot{ std::move(*hb), p_now_ms };
		stats_.refreshed++;
		return MasterVerdict{ MasterRefusal::None, true, {} };
	}
	if (entries_.size() >= config_.max_entries) {
		return refuse(MasterRefusal::Full, core::fmt("%zu servers are listed", entries_.size()));
	}
	const size_t from_address = size_t(std::count_if(
			entries_.begin(), entries_.end(), [&](const auto &p_entry) { return p_entry.first.first == key.first; }));
	if (from_address >= config_.max_per_address) {
		return refuse(MasterRefusal::AddressFull,
				core::fmt("%zu servers are listed from %s", from_address, key.first.c_str()));
	}
	entries_.emplace(key, Slot{ std::move(*hb), p_now_ms });
	stats_.registered++;
	return MasterVerdict{};
}

size_t MasterRegistry::expire(uint64_t p_now_ms) {
	const size_t removed =
			std::erase_if(entries_, [&](const auto &p_entry) { return !live(p_entry.second, p_now_ms); });
	stats_.expired += removed;
	return removed;
}

std::vector<MasterEntry> MasterRegistry::list(uint64_t p_now_ms) {
	expire(p_now_ms);
	std::vector<MasterEntry> out;
	out.reserve(entries_.size());
	for (const auto &[key, slot] : entries_) {
		const uint64_t age_ms = p_now_ms > slot.seen_ms ? p_now_ms - slot.seen_ms : 0;
		out.push_back(MasterEntry{ slot.server, key.first, uint32_t(age_ms / 1000u) });
	}
	std::sort(out.begin(), out.end(), [](const MasterEntry &p_a, const MasterEntry &p_b) {
		return core::to_lower_ascii(p_a.server.name) < core::to_lower_ascii(p_b.server.name);
	});
	return out;
}

std::vector<MasterEntry> MasterRegistry::query(uint64_t p_now_ms) {
	stats_.queries++;
	return list(p_now_ms);
}

MasterServer::MasterServer(const MasterServerConfig &p_config) : config_(p_config), registry_(p_config.registry) {}

MasterServer::~MasterServer() {
	if (host_ != nullptr) {
		host_->disconnect_all();
	}
}

core::Result<std::unique_ptr<MasterServer>> MasterServer::start(const MasterServerConfig &p_config) {
	std::unique_ptr<MasterServer> server(new MasterServer(p_config));
	std::string error;
	server->host_ = Host::create_server(p_config.bind_ip.c_str(), p_config.port, p_config.max_peers, 1, &error);
	if (server->host_ == nullptr) {
		return core::make_error(
				core::fmt("master on %s:%u: %s", p_config.bind_ip.c_str(), unsigned(p_config.port), error.c_str()));
	}
	server->connected_at_.assign(server->host_->peer_capacity(), kNotConnected);
	return std::move(server);
}

uint16_t MasterServer::port() const {
	return host_->bound_port();
}

void MasterServer::answer(uint32_t p_peer, std::span<const uint8_t> p_datagram, uint64_t p_now_ms) {
	std::vector<uint8_t> reply;
	const std::optional<MasterKind> kind = peek_master_kind(p_datagram);
	if (kind == MasterKind::Heartbeat) {
		const MasterVerdict v = registry_.heartbeat(p_now_ms, host_->peer_ip(p_peer), p_datagram);
		reply = encode_master_ack(MasterAck{ v.refusal, v.detail });
	} else if (kind == MasterKind::Query && p_datagram.size() <= kMasterMaxRequestBytes) {
		if (const core::Status q = decode_master_query(p_datagram); q.ok()) {
			reply = encode_master_list(registry_.query(p_now_ms));
		} else {
			const MasterVerdict v = registry_.refuse(MasterRefusal::Malformed, q.error().message);
			reply = encode_master_ack(MasterAck{ v.refusal, v.detail });
		}
	} else {
		const MasterVerdict v =
				registry_.refuse(kind.has_value() ? MasterRefusal::Unexpected : MasterRefusal::Malformed,
						kind.has_value() ? "a master takes heartbeats and queries" : "not a master datagram");
		reply = encode_master_ack(MasterAck{ v.refusal, v.detail });
	}
	if (!reply.empty()) {
		host_->send(p_peer, 0, reply.data(), reply.size(), Host::SendMode::Reliable);
	}
	connected_at_[p_peer] = kNotConnected;
	host_->disconnect_later(p_peer);
}

void MasterServer::service(uint32_t p_timeout_ms, uint64_t p_now_ms) {
	Host::Event ev;
	for (uint32_t wait = p_timeout_ms; host_->service(wait, ev) > 0; wait = 0) {
		if (ev.peer >= connected_at_.size()) {
			continue;
		}
		switch (ev.type) {
			case Host::EventType::Connect:
				connected_at_[ev.peer] = p_now_ms;
				connections_++;
				break;
			case Host::EventType::Disconnect:
				connected_at_[ev.peer] = kNotConnected;
				break;
			case Host::EventType::Receive:
				if (connected_at_[ev.peer] != kNotConnected) {
					answer(ev.peer, ev.data, p_now_ms);
				}
				break;
			case Host::EventType::None:
				break;
		}
	}
	for (uint32_t peer = 0; peer < connected_at_.size(); ++peer) {
		const uint64_t at = connected_at_[peer];
		if (at != kNotConnected && p_now_ms > at && p_now_ms - at > config_.idle_ms) {
			connected_at_[peer] = kNotConnected;
			idle_drops_++;
			host_->disconnect_later(peer);
		}
	}
	registry_.expire(p_now_ms);
	host_->flush();
}

MasterServerStats MasterServer::stats() const {
	MasterServerStats s;
	s.registry = registry_.stats();
	s.servers = registry_.size();
	s.connections = connections_;
	s.idle_drops = idle_drops_;
	return s;
}

core::Result<std::vector<uint8_t>> master_exchange(const Endpoint &p_master, std::span<const uint8_t> p_request,
		uint32_t p_timeout_ms, const std::atomic<bool> *p_cancel) {
	const std::string where = p_master.host + ":" + std::to_string(p_master.port);
	if (p_request.empty()) {
		return core::make_error("master " + where + ": nothing to send");
	}
	std::string error;
	std::unique_ptr<Host> host = Host::create_client(1, &error);
	if (host == nullptr) {
		return core::make_error("master " + where + ": " + error);
	}
	const int peer = host->connect(p_master.host.c_str(), p_master.port);
	if (peer < 0) {
		return core::make_error("master " + where + ": cannot resolve " + p_master.host);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_timeout_ms);
	bool connected = false;
	while (std::chrono::steady_clock::now() < deadline &&
			(p_cancel == nullptr || !p_cancel->load(std::memory_order_acquire))) {
		Host::Event ev;
		const int r = host->service(10, ev);
		if (r < 0) {
			return core::make_error("master " + where + ": the ENet service failed");
		}
		if (r == 0) {
			continue;
		}
		if (ev.type == Host::EventType::Connect) {
			connected = true;
			if (!host->send(uint32_t(peer), 0, p_request.data(), p_request.size(), Host::SendMode::Reliable)) {
				return core::make_error("master " + where + ": the request was not queued");
			}
			host->flush();
		} else if (ev.type == Host::EventType::Receive) {
			host->disconnect_all();
			return std::move(ev.data);
		} else if (ev.type == Host::EventType::Disconnect) {
			return core::make_error("master " + where +
					(connected ? ": the master closed the connection without an answer" : ": no master answered"));
		}
	}
	host->disconnect_all();
	if (p_cancel != nullptr && p_cancel->load(std::memory_order_acquire)) {
		return core::make_error("master " + where + ": cancelled");
	}
	return core::make_error(core::fmt("master %s: %s within %u ms", where.c_str(),
			connected ? "no answer" : "no connection", unsigned(p_timeout_ms)));
}

core::Result<MasterAck> send_master_heartbeat(const Endpoint &p_master, const MasterHeartbeat &p_heartbeat,
		uint32_t p_timeout_ms, const std::atomic<bool> *p_cancel) {
	const std::vector<uint8_t> request = encode_master_heartbeat(p_heartbeat);
	if (request.empty()) {
		return core::make_error(std::string("heartbeat not sent: ") + heartbeat_problem(p_heartbeat));
	}
	core::Result<std::vector<uint8_t>> reply = master_exchange(p_master, request, p_timeout_ms, p_cancel);
	if (!reply) {
		return reply.error();
	}
	return decode_master_ack(*reply);
}

core::Result<std::vector<MasterEntry>> query_master(
		const Endpoint &p_master, uint32_t p_timeout_ms, const std::atomic<bool> *p_cancel) {
	core::Result<std::vector<uint8_t>> reply = master_exchange(p_master, encode_master_query(), p_timeout_ms, p_cancel);
	if (!reply) {
		return reply.error();
	}
	if (peek_master_kind(*reply) == MasterKind::Ack) {
		core::Result<MasterAck> ack = decode_master_ack(*reply);
		if (!ack) {
			return ack.error();
		}
		return core::make_error(std::string("the master refused the query: ") + master_refusal_name(ack->refusal) +
				(ack->detail.empty() ? "" : ": ") + ack->detail);
	}
	return decode_master_list(*reply);
}

Endpoint entry_endpoint(const MasterEntry &p_entry, const Endpoint &p_master) {
	const bool beside_master = loopback_host(p_entry.address) && !loopback_host(p_master.host);
	return Endpoint{ beside_master ? p_master.host : p_entry.address, p_entry.server.port };
}

core::Result<Endpoint> find_server(
		const std::vector<MasterEntry> &p_entries, std::string_view p_name, const Endpoint &p_master) {
	for (const MasterEntry &e : p_entries) {
		if (!core::iequals_ascii(e.server.name, p_name)) {
			continue;
		}
		if (e.server.protocol != kProtocolVersion) {
			return core::make_error(core::fmt("the server %s runs protocol %u, this game %u", e.server.name.c_str(),
					unsigned(e.server.protocol), unsigned(kProtocolVersion)));
		}
		return entry_endpoint(e, p_master);
	}
	return core::make_error(core::fmt("no server named %s on the master %s:%u (%zu listed)",
			std::string(p_name).c_str(), p_master.host.c_str(), unsigned(p_master.port), p_entries.size()));
}

core::Result<Endpoint> resolve_connect_target(
		const ConnectTarget &p_target, uint32_t p_timeout_ms, const std::atomic<bool> *p_cancel) {
	if (p_target.name.empty()) {
		return p_target.at;
	}
	core::Result<std::vector<MasterEntry>> entries = query_master(p_target.at, p_timeout_ms, p_cancel);
	if (!entries) {
		return entries.error();
	}
	return find_server(*entries, p_target.name, p_target.at);
}

MasterHeartbeater::~MasterHeartbeater() {
	stop();
}

core::Status MasterHeartbeater::start(
		Endpoint p_master, Source p_source, uint32_t p_interval_ms, uint32_t p_timeout_ms) {
	if (thread_.joinable()) {
		return core::make_error("heartbeat: already running");
	}
	if (p_source == nullptr || p_interval_ms == 0) {
		return core::make_error("heartbeat: no source or a zero interval");
	}
	master_ = std::move(p_master);
	source_ = std::move(p_source);
	interval_ms_ = p_interval_ms;
	timeout_ms_ = p_timeout_ms;
	stop_ = false;
	cancel_.store(false, std::memory_order_release);
	thread_ = std::thread([this] { run(); });
	return {};
}

void MasterHeartbeater::stop() {
	{
		std::lock_guard lock(mutex_);
		stop_ = true;
	}
	cancel_.store(true, std::memory_order_release);
	wake_.notify_all();
	if (thread_.joinable()) {
		thread_.join();
	}
}

MasterHeartbeatStats MasterHeartbeater::stats() const {
	std::lock_guard lock(mutex_);
	return stats_;
}

void MasterHeartbeater::run() {
	std::unique_lock lock(mutex_);
	while (!stop_) {
		lock.unlock();
		core::Result<MasterAck> ack = send_master_heartbeat(master_, source_(), timeout_ms_, &cancel_);
		lock.lock();
		stats_.sent++;
		if (!ack) {
			stats_.failed++;
			stats_.last_refusal = MasterRefusal::None;
			stats_.last_error = ack.error().message;
		} else if (ack->refusal != MasterRefusal::None) {
			stats_.refused++;
			stats_.last_refusal = ack->refusal;
			stats_.last_error =
					std::string(master_refusal_name(ack->refusal)) + (ack->detail.empty() ? "" : ": ") + ack->detail;
		} else {
			stats_.acked++;
			stats_.last_refusal = MasterRefusal::None;
			stats_.last_error.clear();
		}
		wake_.wait_for(lock, std::chrono::milliseconds(interval_ms_), [this] { return stop_; });
	}
}

}
