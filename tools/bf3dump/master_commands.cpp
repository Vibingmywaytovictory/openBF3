#include "master_commands.h"

#include "bf3/core/util/format.h"
#include "bf3/core/util/key_values.h"
#include "bf3/net/master.h"
#include "bf3/net/protocol.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string_view>

namespace bf3dump {

namespace {

using namespace bf3;

struct MasterArgs {
	std::string bind_ip = "127.0.0.1";
	uint32_t port = net::kMasterDefaultPort;
	float seconds = 0.0f;
	std::string port_file;
	uint32_t timeout_ms = net::kMasterExchangeMs;
	std::vector<std::string> positional;
};

std::string parse_master_args(const std::vector<std::string> &p_args, bool p_serve, MasterArgs &r_args) {
	for (const std::string &a : p_args) {
		if (a.rfind("--", 0) != 0) {
			r_args.positional.push_back(a);
			continue;
		}
		const size_t eq = a.find('=');
		const std::string key = a.substr(0, eq);
		const std::string_view value =
				eq == std::string::npos ? std::string_view() : std::string_view(a).substr(eq + 1);
		bool ok = eq != std::string::npos;
		if (p_serve && key == "--port") {
			ok = ok && core::parse_u32_value(value, 0.0, 65535.0, r_args.port).fault == core::KeyFault::None;
		} else if (p_serve && key == "--bind") {
			r_args.bind_ip = std::string(value);
			ok = ok && !value.empty();
		} else if (p_serve && key == "--seconds") {
			ok = ok && core::parse_float_value(value, 0.0, 1.0e7, r_args.seconds).fault == core::KeyFault::None;
		} else if (p_serve && key == "--port-file") {
			r_args.port_file = std::string(value);
			ok = ok && !value.empty();
		} else if (!p_serve && key == "--timeout-ms") {
			ok = ok && core::parse_u32_value(value, 1.0, 60000.0, r_args.timeout_ms).fault == core::KeyFault::None;
		} else {
			return "unknown option " + a;
		}
		if (!ok) {
			return "bad value in " + a;
		}
	}
	if (p_serve && !r_args.positional.empty()) {
		return "master takes no positional arguments";
	}
	if (!p_serve && r_args.positional.size() != 1) {
		return "expected <master host:port>";
	}
	return {};
}

std::string refusals_text(const net::MasterRegistryStats &p_stats) {
	std::string text;
	for (size_t i = 1; i < net::kMasterRefusalCount; ++i) {
		text += core::fmt("%s%s %llu", text.empty() ? "" : ", ", net::master_refusal_name(net::MasterRefusal(i)),
				(unsigned long long)p_stats.refusals[i]);
	}
	return text;
}

std::string master_line(const net::MasterServerStats &p_stats) {
	const net::MasterRegistryStats &r = p_stats.registry;
	return core::fmt("master: %zu servers | registered %llu, refreshed %llu, expired %llu, queries %llu | refused %llu "
					 "(%s) | connections %llu, idle drops %llu",
			p_stats.servers, (unsigned long long)r.registered, (unsigned long long)r.refreshed,
			(unsigned long long)r.expired, (unsigned long long)r.queries, (unsigned long long)r.refused,
			refusals_text(r).c_str(), (unsigned long long)p_stats.connections, (unsigned long long)p_stats.idle_drops);
}

uint64_t steady_ms() {
	return uint64_t(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

}

const char *master_usage() {
	return "  master [--port=N] [--bind=IP] [--seconds=S] [--port-file=<path>]\n"
		   "      the master server: binds 127.0.0.1 unless --bind is given, port 25250 unless --port\n"
		   "  servers <master host:port> [--timeout-ms=N]\n";
}

int cmd_master(const std::vector<std::string> &p_args) {
	MasterArgs a;
	if (const std::string error = parse_master_args(p_args, true, a); !error.empty()) {
		std::fprintf(stderr, "master: %s\n%s", error.c_str(), master_usage());
		return 2;
	}
	net::MasterServerConfig config;
	config.bind_ip = a.bind_ip;
	config.port = uint16_t(a.port);
	core::Result<std::unique_ptr<net::MasterServer>> started = net::MasterServer::start(config);
	if (!started) {
		std::printf("master: failed: %s\n", started.error().message.c_str());
		return 1;
	}
	net::MasterServer &master = **started;
	if (!a.port_file.empty()) {
		std::ofstream out(a.port_file, std::ios::binary | std::ios::trunc);
		out << master.port() << "\n";
		if (!out) {
			std::printf("master: failed: --port-file=%s: cannot write\n", a.port_file.c_str());
			return 1;
		}
	}
	std::printf("master: listening on %s:%u, protocol %u, heartbeat %u s, an entry expires after %u missed\n",
			a.bind_ip.c_str(), unsigned(master.port()), unsigned(net::kProtocolVersion),
			unsigned(net::kMasterHeartbeatMs / 1000u), unsigned(net::kMasterMissedHeartbeats));
	std::fflush(stdout);
	const uint64_t start = steady_ms();
	const uint64_t end = a.seconds > 0.0f ? start + uint64_t(double(a.seconds) * 1000.0) : 0;
	std::string last;
	uint64_t printed_at = start;
	for (uint64_t now = start; end == 0 || now < end; now = steady_ms()) {
		master.service(20, now);
		const std::string line = master_line(master.stats());
		if (line != last || now - printed_at >= 10000) {
			std::printf("%s\n", line.c_str());
			std::fflush(stdout);
			last = line;
			printed_at = now;
		}
	}
	std::printf("%s\nmaster: stopped by --seconds\n", master_line(master.stats()).c_str());
	std::fflush(stdout);
	return 0;
}

int cmd_servers(const std::vector<std::string> &p_args) {
	MasterArgs a;
	if (const std::string error = parse_master_args(p_args, false, a); !error.empty()) {
		std::fprintf(stderr, "servers: %s\n%s", error.c_str(), master_usage());
		return 2;
	}
	core::Result<net::Endpoint> master = net::parse_endpoint(a.positional[0]);
	if (!master) {
		std::fprintf(stderr, "servers: %s\n%s", master.error().message.c_str(), master_usage());
		return 2;
	}
	core::Result<std::vector<net::MasterEntry>> entries = net::query_master(*master, a.timeout_ms);
	if (!entries) {
		std::printf("servers: failed: %s\n", entries.error().message.c_str());
		return 1;
	}
	std::printf("servers: %zu on %s:%u\n", entries->size(), master->host.c_str(), unsigned(master->port));
	const uint64_t build = net::game_build();
	for (const net::MasterEntry &e : *entries) {
		const net::Endpoint at = net::entry_endpoint(e, *master);
		const net::MasterHeartbeat &s = e.server;
		std::printf("server: %s | %s:%u | %s %s | players %u, bots %u, max %u | protocol %u%s, build %016llx%s | "
					"heard %u s ago\n",
				s.name.c_str(), at.host.c_str(), unsigned(at.port), s.level.c_str(), s.mode.c_str(),
				unsigned(s.players), unsigned(s.bots), unsigned(s.max_players), unsigned(s.protocol),
				s.protocol == net::kProtocolVersion ? "" : " (not this game's)", (unsigned long long)s.build,
				s.build == build ? " (this build)" : " (another build)", unsigned(e.age_s));
	}
	std::fflush(stdout);
	return 0;
}

}
