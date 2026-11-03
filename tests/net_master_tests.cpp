#include <doctest/doctest.h>

#include "bf3/core/util/splitmix64.h"
#include "bf3/net/master.h"
#include "bf3/net/protocol.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bf3::net;
namespace core = bf3::core;

MasterHeartbeat beat(std::string p_name, uint16_t p_port = 25200) {
	MasterHeartbeat h;
	h.name = std::move(p_name);
	h.port = p_port;
	h.level = "MP_Subway";
	h.mode = "TeamDeathMatch0";
	h.players = 3;
	h.bots = 20;
	h.max_players = 32;
	h.protocol = kProtocolVersion;
	h.build = 0x1234'5678'9ABC'DEF0ull;
	return h;
}

std::vector<uint8_t> bytes(const MasterHeartbeat &p_heartbeat) {
	std::vector<uint8_t> b = encode_master_heartbeat(p_heartbeat);
	REQUIRE_FALSE(b.empty());
	return b;
}

std::vector<uint8_t> renamed(const MasterHeartbeat &p_heartbeat, const std::string &p_name) {
	const std::vector<uint8_t> b = bytes(p_heartbeat);
	std::vector<uint8_t> out(b.begin(), b.begin() + 5);
	out.push_back(uint8_t(p_name.size()));
	out.insert(out.end(), p_name.begin(), p_name.end());
	out.insert(out.end(), b.begin() + std::ptrdiff_t(5 + 1 + p_heartbeat.name.size()), b.end());
	return out;
}

constexpr uint64_t kWindow = uint64_t(kMasterHeartbeatMs) * kMasterMissedHeartbeats;

}

TEST_SUITE("net-master") {
	TEST_CASE("a heartbeat registers a server under its source address, the next one refreshes it, and the list "
			  "carries it") {
		MasterRegistry reg;
		const MasterVerdict first = reg.heartbeat(1000, "127.0.0.1", bytes(beat("alpha")));
		CHECK(first.refusal == MasterRefusal::None);
		CHECK_FALSE(first.refreshed);
		MasterHeartbeat later = beat("alpha");
		later.players = 5;
		later.level = "MP_011";
		const MasterVerdict second = reg.heartbeat(31000, "127.0.0.1", bytes(later));
		CHECK(second.refusal == MasterRefusal::None);
		CHECK(second.refreshed);
		REQUIRE(reg.heartbeat(32000, "10.0.0.7", bytes(beat("Bravo", 25300))).refusal == MasterRefusal::None);
		const std::vector<MasterEntry> list = reg.list(41000);
		REQUIRE(list.size() == 2);
		CHECK(list[0].server == later);
		CHECK(list[0].address == "127.0.0.1");
		CHECK(list[0].age_s == 10);
		CHECK(list[1].server.name == "Bravo");
		CHECK(list[1].address == "10.0.0.7");
		CHECK(list[1].server.port == 25300);
		CHECK(reg.stats().registered == 2);
		CHECK(reg.stats().refreshed == 1);
		CHECK(reg.stats().refused == 0);
	}

	TEST_CASE("an entry expires after three missed 30 s heartbeats, and not one millisecond before") {
		MasterRegistry reg;
		REQUIRE(reg.heartbeat(5000, "127.0.0.1", bytes(beat("alpha"))).refusal == MasterRefusal::None);
		CHECK(reg.window_ms() == 90000);
		CHECK(reg.expire(5000 + kWindow) == 0);
		CHECK(reg.list(5000 + kWindow).size() == 1);
		CHECK(reg.expire(5000 + kWindow + 1) == 1);
		CHECK(reg.list(5000 + kWindow + 1).empty());
		CHECK(reg.stats().expired == 1);
		REQUIRE(reg.heartbeat(200000, "127.0.0.1", bytes(beat("alpha"))).refusal == MasterRefusal::None);
		REQUIRE(reg.heartbeat(200000 + kWindow, "127.0.0.1", bytes(beat("alpha"))).refreshed);
		CHECK(reg.list(200000 + 2 * kWindow).size() == 1);
		CHECK(reg.stats().registered == 2);
	}

	TEST_CASE("a name held by another server is refused, and the per-address and total limits hold") {
		MasterRegistryConfig config;
		config.max_entries = 4;
		config.max_per_address = 2;
		MasterRegistry reg(config);
		REQUIRE(reg.heartbeat(0, "10.0.0.1", bytes(beat("alpha", 1))).refusal == MasterRefusal::None);
		const MasterVerdict taken = reg.heartbeat(0, "10.0.0.2", bytes(beat("ALPHA", 1)));
		CHECK(taken.refusal == MasterRefusal::NameTaken);
		CHECK(taken.detail == "the name alpha is held by 10.0.0.1:1");
		CHECK(reg.heartbeat(0, "10.0.0.1", bytes(beat("alpha", 2))).refusal == MasterRefusal::NameTaken);
		CHECK(reg.heartbeat(0, "10.0.0.1", bytes(beat("beta", 2))).refusal == MasterRefusal::None);
		CHECK(reg.heartbeat(0, "10.0.0.1", bytes(beat("gamma", 3))).refusal == MasterRefusal::AddressFull);
		CHECK(reg.heartbeat(0, "10.0.0.1", bytes(beat("renamed", 2))).refreshed);
		CHECK(reg.heartbeat(0, "10.0.0.2", bytes(beat("gamma", 1))).refusal == MasterRefusal::None);
		CHECK(reg.heartbeat(0, "10.0.0.3", bytes(beat("delta", 1))).refusal == MasterRefusal::None);
		CHECK(reg.heartbeat(0, "10.0.0.4", bytes(beat("epsilon", 1))).refusal == MasterRefusal::Full);
		CHECK(reg.size() == 4);
		const std::vector<MasterEntry> list = reg.list(0);
		REQUIRE(list.size() == 4);
		CHECK(list[0].server.name == "alpha");
		CHECK(list[3].server.name == "renamed");
		CHECK(reg.stats().refusals[size_t(MasterRefusal::NameTaken)] == 2);
		CHECK(reg.stats().refusals[size_t(MasterRefusal::AddressFull)] == 1);
		CHECK(reg.stats().refusals[size_t(MasterRefusal::Full)] == 1);
		CHECK(reg.heartbeat(kWindow + 1, "10.0.0.4", bytes(beat("alpha", 1))).refusal == MasterRefusal::None);
	}

	TEST_CASE("every master message round-trips, and a full list of the longest entries fits") {
		const MasterHeartbeat h = beat("openBF3 server-1.a_b");
		core::Result<MasterHeartbeat> back = decode_master_heartbeat(bytes(h));
		REQUIRE(back.ok());
		CHECK(*back == h);
		for (const MasterRefusal r : { MasterRefusal::None, MasterRefusal::NameTaken, MasterRefusal::Unexpected }) {
			const MasterAck ack{ r, "the name alpha is held by 10.0.0.1:1" };
			core::Result<MasterAck> a = decode_master_ack(encode_master_ack(ack));
			REQUIRE(a.ok());
			CHECK(*a == ack);
		}
		const core::Result<MasterAck> scrubbed = decode_master_ack(encode_master_ack({ MasterRefusal::Full, "a\nb" }));
		REQUIRE(scrubbed.ok());
		CHECK(scrubbed->detail == "a?b");
		CHECK(decode_master_query(encode_master_query()).ok());
		CHECK(peek_master_kind(encode_master_query()) == MasterKind::Query);
		std::vector<MasterEntry> entries;
		for (size_t i = 0; i < kMasterMaxEntries; ++i) {
			MasterHeartbeat e = beat(std::string(kMasterNameBytes - 3, 'n') + std::to_string(100 + i), uint16_t(1 + i));
			e.level = std::string(kMasterIdentBytes, 'L');
			e.mode = std::string(kMasterIdentBytes, 'M');
			e.players = 64;
			e.bots = 64;
			e.max_players = 128;
			entries.push_back(MasterEntry{ e, "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255", uint32_t(i * 1000) });
		}
		const std::vector<uint8_t> list = encode_master_list(entries);
		REQUIRE_FALSE(list.empty());
		MESSAGE("a full list of ", entries.size(), " longest entries is ", list.size(), " bytes");
		CHECK(list.size() <= kMasterMaxListBytes);
		core::Result<std::vector<MasterEntry>> decoded = decode_master_list(list);
		REQUIRE(decoded.ok());
		CHECK(*decoded == entries);
		entries.push_back(entries.front());
		CHECK(encode_master_list(entries).empty());
		CHECK(decode_master_list(encode_master_list({})).ok());
	}

	TEST_CASE("a heartbeat that breaks a rule is not encoded") {
		MasterHeartbeat h = beat("alpha");
		h.port = 0;
		CHECK(encode_master_heartbeat(h).empty());
		h = beat("alpha");
		h.players = 30;
		CHECK(encode_master_heartbeat(h).empty());
		h = beat("al@pha");
		CHECK(encode_master_heartbeat(h).empty());
		h = beat("alpha");
		h.level = "MP Subway";
		CHECK(encode_master_heartbeat(h).empty());
		h = beat("alpha");
		h.max_players = 0;
		h.players = 0;
		h.bots = 0;
		CHECK(encode_master_heartbeat(h).empty());
	}

	TEST_CASE("hostile datagrams are refused as malformed and change nothing") {
		MasterRegistry reg;
		REQUIRE(reg.heartbeat(0, "127.0.0.1", bytes(beat("alpha"))).refusal == MasterRefusal::None);
		const std::vector<uint8_t> good = bytes(beat("bravo"));
		std::vector<std::vector<uint8_t>> hostile = {
			{},
			{ 0x00 },
			std::vector<uint8_t>(good.begin(), good.begin() + 5),
			encode_master_query(),
			encode_master_ack({ MasterRefusal::None, "" }),
			encode_master_list({}),
			encode_hello(Hello{ kProtocolVersion, game_build(), "alpha" }),
			renamed(beat("bravo"), "br@vo"),
			renamed(beat("bravo"), "br:vo"),
			renamed(beat("bravo"), " bravo"),
			renamed(beat("bravo"), "bravo "),
			renamed(beat("bravo"), std::string("br\x01vo")),
			renamed(beat("bravo"), std::string("br\xC3\xA9vo")),
			renamed(beat("bravo"), std::string(kMasterNameBytes + 1, 'n')),
		};
		std::vector<uint8_t> wrong_magic = good;
		wrong_magic[0] ^= 0x01;
		hostile.push_back(wrong_magic);
		std::vector<uint8_t> unknown_kind = good;
		unknown_kind[4] = 9;
		hostile.push_back(unknown_kind);
		std::vector<uint8_t> trailing = good;
		trailing.push_back(0);
		hostile.push_back(trailing);
		std::vector<uint8_t> oversize = good;
		oversize.resize(kMasterMaxRequestBytes + 1, 0);
		hostile.push_back(oversize);
		for (size_t n = 0; n < good.size(); ++n) {
			hostile.emplace_back(good.begin(), good.begin() + std::ptrdiff_t(n));
		}
		const size_t players_at = good.size() - 8 - 2 - 3;
		std::vector<uint8_t> crowded = good;
		crowded[players_at] = 30;
		hostile.push_back(crowded);
		std::vector<uint8_t> port_zero = bytes(beat("bravo", 1));
		const std::vector<uint8_t> port_two = bytes(beat("bravo", 2));
		REQUIRE(port_zero.size() == port_two.size());
		for (size_t i = 0; i < port_zero.size(); ++i) {
			if (port_zero[i] != port_two[i]) {
				port_zero[i] = 0;
			}
		}
		hostile.push_back(port_zero);
		for (const std::vector<uint8_t> &d : hostile) {
			const MasterVerdict v = reg.heartbeat(1, "127.0.0.1", d);
			CHECK_MESSAGE(v.refusal == MasterRefusal::Malformed, "accepted ", d.size(), " bytes");
			CHECK_FALSE(v.detail.empty());
		}
		CHECK(reg.heartbeat(1, "", good).refusal == MasterRefusal::Malformed);
		CHECK(reg.heartbeat(1, "127.0.0.1; rm", good).refusal == MasterRefusal::Malformed);
		CHECK(reg.size() == 1);
		CHECK(reg.stats().registered == 1);
		CHECK(reg.stats().refused == hostile.size() + 2);
		CHECK(reg.stats().refusals[size_t(MasterRefusal::Malformed)] == hostile.size() + 2);
		const std::vector<MasterEntry> list = reg.list(1);
		REQUIRE(list.size() == 1);
		CHECK(list[0].server == beat("alpha"));
		CHECK(reg.heartbeat(1, "127.0.0.1", good).refusal == MasterRefusal::None);
	}

	TEST_CASE("20,000 mutated heartbeats and lists never crash the decoders, and whatever a decoder accepts keeps "
			  "every rule") {
		MasterRegistryConfig config;
		config.max_entries = 64;
		MasterRegistry reg(config);
		std::vector<MasterEntry> entries;
		for (int i = 0; i < 3; ++i) {
			entries.push_back(MasterEntry{ beat("srv" + std::to_string(i), uint16_t(100 + i)), "10.0.0.1", 7 });
		}
		const std::vector<uint8_t> seeds[] = { bytes(beat("alpha")), encode_master_list(entries) };
		uint64_t state = 0x6D61737465720001ull;
		size_t accepted = 0;
		for (int round = 0; round < 20000; ++round) {
			std::vector<uint8_t> d = seeds[round & 1];
			const int flips = 1 + int(bf3::core::splitmix64_next(state) % 4);
			for (int f = 0; f < flips; ++f) {
				const uint64_t r = bf3::core::splitmix64_next(state);
				const size_t at = size_t(r % d.size());
				switch ((r >> 32) % 3) {
					case 0:
						d[at] ^= uint8_t(1u << ((r >> 40) % 8));
						break;
					case 1:
						d[at] = uint8_t(r >> 48);
						break;
					default:
						d.resize(at + 1);
						break;
				}
			}
			if ((round & 1) == 0) {
				const MasterVerdict v = reg.heartbeat(uint64_t(round), "127.0.0.1", d);
				accepted += v.refusal == MasterRefusal::None ? 1 : 0;
				if (core::Result<MasterHeartbeat> h = decode_master_heartbeat(d)) {
					CHECK(valid_server_name(h->name));
					CHECK(h->port != 0);
					CHECK(unsigned(h->players) + h->bots <= h->max_players);
					CHECK(decode_master_heartbeat(encode_master_heartbeat(*h)).value() == *h);
				}
			} else if (core::Result<std::vector<MasterEntry>> l = decode_master_list(d)) {
				CHECK(l->size() <= kMasterMaxEntries);
				CHECK(decode_master_list(encode_master_list(*l)).value() == *l);
			}
		}
		MESSAGE(accepted, " of 10,000 mutated heartbeats were well formed");
		CHECK(reg.size() <= config.max_entries);
		CHECK(reg.stats().registered + reg.stats().refreshed + reg.stats().refused == 10000);
	}

	TEST_CASE("--connect takes host:port or name@host:port, and a name resolves through the master's list") {
		core::Result<ConnectTarget> direct = parse_connect_target("127.0.0.1:25200");
		REQUIRE(direct.ok());
		CHECK(direct->name.empty());
		CHECK(direct->at == Endpoint{ "127.0.0.1", 25200 });
		core::Result<ConnectTarget> named = parse_connect_target("my server@master.lan:25250");
		REQUIRE(named.ok());
		CHECK(named->name == "my server");
		CHECK(named->at == Endpoint{ "master.lan", 25250 });
		for (const char *bad : { "", "host", ":25200", "host:0", "host:65536", "host:port", "@host:1", "a@b@c:1",
					 "bad!name@host:1", " lead@host:1", "a b:1" }) {
			CHECK_FALSE_MESSAGE(parse_connect_target(bad).ok(), bad);
		}
		const Endpoint lan{ "192.168.1.20", kMasterDefaultPort };
		const Endpoint local{ "127.0.0.1", kMasterDefaultPort };
		MasterEntry beside{ beat("beside", 25201), "127.0.0.1", 0 };
		MasterEntry remote{ beat("remote", 25202), "192.168.1.30", 0 };
		MasterEntry old = remote;
		old.server.name = "old";
		old.server.protocol = uint16_t(kProtocolVersion - 1);
		const std::vector<MasterEntry> list = { beside, remote, old };
		CHECK(entry_endpoint(beside, lan) == Endpoint{ "192.168.1.20", 25201 });
		CHECK(entry_endpoint(beside, local) == Endpoint{ "127.0.0.1", 25201 });
		CHECK(entry_endpoint(remote, lan) == Endpoint{ "192.168.1.30", 25202 });
		core::Result<Endpoint> found = find_server(list, "REMOTE", lan);
		REQUIRE(found.ok());
		CHECK(*found == Endpoint{ "192.168.1.30", 25202 });
		const core::Result<Endpoint> missing = find_server(list, "nobody", lan);
		REQUIRE_FALSE(missing.ok());
		CHECK(missing.error().message == "no server named nobody on the master 192.168.1.20:25250 (3 listed)");
		const core::Result<Endpoint> stale = find_server(list, "old", lan);
		REQUIRE_FALSE(stale.ok());
		CHECK(stale.error().message.find("runs protocol") != std::string::npos);
		core::Result<Endpoint> passthrough = resolve_connect_target(*direct, 1);
		REQUIRE(passthrough.ok());
		CHECK(*passthrough == direct->at);
	}
}

TEST_SUITE("net-master-enet") {
	TEST_CASE("over ENet on 127.0.0.1 a heartbeater registers with a master, a query lists it, a name resolves to "
			  "it, and garbage is answered with a refusal") {
		MasterServerConfig config;
		config.port = 0;
		core::Result<std::unique_ptr<MasterServer>> started = MasterServer::start(config);
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		MasterServer &master = **started;
		const Endpoint at{ "127.0.0.1", master.port() };
		std::atomic<bool> stop{ false };
		std::thread service([&] {
			uint64_t now = 0;
			while (!stop.load(std::memory_order_acquire)) {
				master.service(5, now);
				now += 5;
			}
		});
		MasterHeartbeater heartbeater;
		REQUIRE(heartbeater.start(at, [] { return beat("procs-alpha", 25201); }, 60000, 5000).ok());
		CHECK(bf3::test::wait_until([&] { return heartbeater.stats().acked == 1; }, std::chrono::seconds(10)));
		core::Result<std::vector<MasterEntry>> listed = query_master(at);
		REQUIRE_MESSAGE(listed.ok(), (listed.ok() ? std::string() : listed.error().message));
		REQUIRE(listed->size() == 1);
		CHECK((*listed)[0].server == beat("procs-alpha", 25201));
		CHECK((*listed)[0].address == "127.0.0.1");
		core::Result<ConnectTarget> target = parse_connect_target("procs-alpha@127.0.0.1:" + std::to_string(at.port));
		REQUIRE(target.ok());
		core::Result<Endpoint> resolved = resolve_connect_target(*target);
		REQUIRE_MESSAGE(resolved.ok(), (resolved.ok() ? std::string() : resolved.error().message));
		CHECK(*resolved == Endpoint{ "127.0.0.1", 25201 });
		const MasterHeartbeat clash = beat("procs-alpha", 25202);
		core::Result<MasterAck> refused = send_master_heartbeat(at, clash);
		REQUIRE(refused.ok());
		CHECK(refused->refusal == MasterRefusal::NameTaken);
		const std::vector<uint8_t> garbage = { 1, 2, 3, 4, 5, 6 };
		core::Result<std::vector<uint8_t>> answer = master_exchange(at, garbage);
		REQUIRE(answer.ok());
		core::Result<MasterAck> ack = decode_master_ack(*answer);
		REQUIRE(ack.ok());
		CHECK(ack->refusal == MasterRefusal::Malformed);
		heartbeater.stop();
		CHECK(heartbeater.stats().sent == 1);
		CHECK(heartbeater.stats().failed == 0);
		const Endpoint nobody{ "127.0.0.1", 1 };
		const core::Result<std::vector<MasterEntry>> none = query_master(nobody, 300);
		CHECK_FALSE(none.ok());
		stop.store(true, std::memory_order_release);
		service.join();
		const MasterServerStats s = master.stats();
		CHECK(s.servers == 1);
		CHECK(s.registry.queries == 2);
		CHECK(s.registry.refusals[size_t(MasterRefusal::NameTaken)] == 1);
		CHECK(s.registry.refusals[size_t(MasterRefusal::Malformed)] == 1);
		CHECK(s.connections == 5);
	}
}
