#include "bf3/net/netsim.h"

#include "bf3/core/util/key_values.h"
#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace bf3::net {

namespace {

constexpr uint64_t kHeldBit = uint64_t(1) << 63;
constexpr double kHoldTimeoutMs = 100.0;
constexpr double kMaxLatencyMs = 10'000.0;
constexpr double kMaxSpikeMs = 60'000.0;
constexpr double kMaxRttMs = 2 * kMaxLatencyMs;

using bf3::core::splitmix64_next;

double uniform01(uint64_t &r_state) {
	return double(splitmix64_next(r_state) >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t ms_to_ns(double p_ms) {
	return p_ms <= 0.0 ? 0 : uint64_t(p_ms * 1.0e6 + 0.5);
}

using bf3::core::trim;

bool parse_number(std::string_view p_s, double &r_value) {
	p_s = trim(p_s);
	if (p_s.empty()) {
		return false;
	}
	const char *end = p_s.data() + p_s.size();
	const std::from_chars_result r = std::from_chars(p_s.data(), end, r_value);
	return r.ec == std::errc() && r.ptr == end && std::isfinite(r_value) && r_value >= 0.0;
}

bool parse_range(std::string_view p_s, double &r_min, double &r_max) {
	const size_t dash = p_s.find('-');
	if (dash == std::string_view::npos) {
		if (!parse_number(p_s, r_min)) {
			return false;
		}
		r_max = r_min;
		return true;
	}
	return parse_number(p_s.substr(0, dash), r_min) && parse_number(p_s.substr(dash + 1), r_max) && r_min <= r_max;
}

using Key = core::KeyDef<NetSimConfig>;
using core::KeyFault;
using core::KeyKind;
using core::KeyOutcome;

KeyOutcome bad(std::string_view p_why) {
	return { KeyFault::BadValue, 0.0, p_why };
}

KeyOutcome parse_base(std::string_view p_item, size_t p_index, NetSimConfig &r_config) {
	if (p_index != 0) {
		return bad("a base profile (off, bf3, <rtt_ms>/<loss_percent>) must come first");
	}
	NetSimConfig &c = r_config;
	if (p_item == "off") {
		c = NetSimConfig{};
	} else if (p_item == "bf3") {
		c.latency_min_ms = 100;
		c.latency_max_ms = 200;
		c.loss = 0.01f;
		c.spike_chance = 0.03f;
		c.spike_min_ms = 300;
		c.spike_max_ms = 1000;
		c.reorder = 0.01f;
		c.duplicate = 0.02f;
	} else {
		const size_t slash = p_item.find('/');
		double rtt = 0, loss = 0;
		if (slash == std::string_view::npos || !parse_number(p_item.substr(0, slash), rtt) ||
				!parse_number(p_item.substr(slash + 1), loss)) {
			return bad("expected off, bf3, <rtt_ms>/<loss_percent> or key=value items");
		}
		if (rtt > kMaxRttMs || loss > 100.0) {
			return bad("round trip above 20,000 ms or loss above 100 %");
		}
		const double one_way = rtt * 0.5;
		c.latency_min_ms = float(one_way * 0.9);
		c.latency_max_ms = float(one_way * 1.1);
		c.loss = float(loss / 100.0);
	}
	return {};
}

KeyOutcome parse_lat(const Key &, std::string_view p_value, NetSimConfig &r_config) {
	double a = 0, b = 0;
	if (!parse_range(p_value, a, b) || b > kMaxLatencyMs) {
		return bad("lat wants <ms> or <min_ms>-<max_ms>, at most 10,000 ms");
	}
	r_config.latency_min_ms = float(a);
	r_config.latency_max_ms = float(b);
	return {};
}

KeyOutcome parse_spike_ms(const Key &, std::string_view p_value, NetSimConfig &r_config) {
	double a = 0, b = 0;
	if (!parse_range(p_value, a, b) || b > kMaxSpikeMs) {
		return bad("spike_ms wants <ms> or <min_ms>-<max_ms>, at most 60,000 ms");
	}
	r_config.spike_min_ms = float(a);
	r_config.spike_max_ms = float(b);
	return {};
}

KeyOutcome parse_percent(const Key &p_key, std::string_view p_value, NetSimConfig &r_config) {
	double a = 0;
	if (!parse_number(p_value, a) || a > 100.0) {
		return bad("percentages are 0..100");
	}
	*static_cast<float *>(p_key.field(r_config)) = float(a / 100.0);
	return {};
}

KeyOutcome parse_seed(const Key &, std::string_view p_value, NetSimConfig &r_config) {
	uint64_t seed = 0;
	const char *end = p_value.data() + p_value.size();
	const std::from_chars_result r = std::from_chars(p_value.data(), end, seed);
	if (p_value.empty() || r.ec != std::errc() || r.ptr != end) {
		return bad("seed wants an unsigned 64-bit integer");
	}
	r_config.seed = seed;
	return {};
}

#define BF3_NETSIM_FIELD(f) [](NetSimConfig &c) -> void * { return &c.f; }

const Key kKeys[] = {
	{ "lat", KeyKind::Custom, 0, 0, nullptr, {}, parse_lat },
	{ "spike_ms", KeyKind::Custom, 0, 0, nullptr, {}, parse_spike_ms },
	{ "loss", KeyKind::Custom, 0, 0, BF3_NETSIM_FIELD(loss), {}, parse_percent },
	{ "spike", KeyKind::Custom, 0, 0, BF3_NETSIM_FIELD(spike_chance), {}, parse_percent },
	{ "reorder", KeyKind::Custom, 0, 0, BF3_NETSIM_FIELD(reorder), {}, parse_percent },
	{ "dup", KeyKind::Custom, 0, 0, BF3_NETSIM_FIELD(duplicate), {}, parse_percent },
	{ "seed", KeyKind::Custom, 0, 0, nullptr, {}, parse_seed },
};

#undef BF3_NETSIM_FIELD

std::string netsim_fault(const core::KeyFaultInfo &p_info) {
	switch (p_info.fault) {
		case KeyFault::TooLarge:
			return "netsim profile: " + std::to_string(p_info.count) + " bytes (limit " + std::to_string(p_info.limit) +
					")";
		case KeyFault::TooManyLines:
			return "netsim profile: " + std::to_string(p_info.count) + " items (limit " + std::to_string(p_info.limit) +
					")";
		default:
			break;
	}
	const std::string why = p_info.fault == KeyFault::EmptyItem ? std::string("empty item")
			: p_info.fault == KeyFault::UnknownKey
			? std::string("unknown key (lat, loss, spike, spike_ms, reorder, dup, seed)")
			: std::string(p_info.detail);
	return "netsim profile \"" + std::string(p_info.text) + "\": " + why;
}

const core::KeyValueGrammar<NetSimConfig> kGrammar{ "netsim profile", kKeys, kMaxNetSimProfileBytes,
	kMaxNetSimProfileItems, SIZE_MAX, false, ',', false, false, netsim_fault, parse_base };

void append_number(std::string &r_out, double p_value) {
	char buf[64];
	const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), float(p_value));
	r_out.append(buf, r.ptr);
}
}

core::Result<NetSimConfig> parse_netsim(std::string_view p_profile) {
	const std::string_view profile = trim(p_profile);
	if (profile.empty() || profile == "off") {
		return NetSimConfig{};
	}
	return core::parse_key_values(p_profile, kGrammar);
}

std::string format_netsim(const NetSimConfig &p_config) {
	if (!p_config.enabled()) {
		return "off";
	}
	std::string out = "lat=";
	append_number(out, p_config.latency_min_ms);
	out += '-';
	append_number(out, p_config.latency_max_ms);
	out += ",loss=";
	append_number(out, double(p_config.loss) * 100.0);
	out += ",spike=";
	append_number(out, double(p_config.spike_chance) * 100.0);
	out += ",spike_ms=";
	append_number(out, p_config.spike_min_ms);
	out += '-';
	append_number(out, p_config.spike_max_ms);
	out += ",reorder=";
	append_number(out, double(p_config.reorder) * 100.0);
	out += ",dup=";
	append_number(out, double(p_config.duplicate) * 100.0);
	out += ",seed=" + std::to_string(p_config.seed);
	return out;
}

NetSim::NetSim(const NetSimConfig &p_config) :
		config_(p_config), rng_(p_config.seed) {}

void NetSim::submit(Datagram p_datagram, uint64_t p_now_ns) {
	stats_.submitted++;
	const auto before = [](const Pending &p_a, const Pending &p_b) {
		return p_a.release_ns != p_b.release_ns ? p_a.release_ns < p_b.release_ns
												: (p_a.order & ~kHeldBit) < (p_b.order & ~kHeldBit);
	};
	const auto insert = [&](Pending p_pending) {
		queue_.insert(std::upper_bound(queue_.begin(), queue_.end(), p_pending, before), std::move(p_pending));
	};
	if (!config_.enabled()) {
		insert(Pending{ p_now_ns, next_order_++, std::move(p_datagram) });
		return;
	}
	const bool unreliable = !p_datagram.reliable;

	if (config_.spike_chance > 0.0f) {
		if (stats_.submitted > 1 && p_now_ns > last_spike_check_ns_ && p_now_ns >= spike_until_ns_) {
			const double dt = double(p_now_ns - last_spike_check_ns_) * 1.0e-9;
			const double chance = std::clamp(double(config_.spike_chance), 0.0, 1.0);
			const double p = chance >= 1.0 ? 1.0 : 1.0 - std::pow(1.0 - chance, dt);
			if (uniform01(rng_) < p) {
				const double ms = config_.spike_min_ms + (config_.spike_max_ms - config_.spike_min_ms) * uniform01(rng_);
				spike_until_ns_ = p_now_ns + ms_to_ns(ms);
			}
		}
		last_spike_check_ns_ = std::max(last_spike_check_ns_, p_now_ns);
	}
	if (unreliable && p_now_ns < spike_until_ns_) {
		stats_.dropped++;
		stats_.spike_dropped++;
		return;
	}
	if (unreliable && config_.loss > 0.0f && uniform01(rng_) < double(config_.loss)) {
		stats_.dropped++;
		return;
	}
	const bool duplicate = unreliable && config_.duplicate > 0.0f && uniform01(rng_) < double(config_.duplicate);
	const int copies = duplicate ? 2 : 1;
	stats_.duplicated += duplicate ? 1 : 0;
	const double span_ms = std::max(0.0, double(config_.latency_max_ms) - double(config_.latency_min_ms));
	for (int copy = 0; copy < copies; ++copy) {
		const double delay_ms = double(config_.latency_min_ms) + (span_ms > 0.0 ? span_ms * uniform01(rng_) : 0.0);
		uint64_t release = p_now_ns + ms_to_ns(delay_ms);
		const uint64_t scheduled = stats_.delivered + queue_.size();
		stats_.mean_delay_ms += (delay_ms - stats_.mean_delay_ms) / double(scheduled + 1);
		bool held = false;
		if (!unreliable) {

			for (const Pending &p : queue_) {
				if (p.datagram.reliable && p.datagram.peer == p_datagram.peer && p.datagram.channel == p_datagram.channel) {
					release = std::max(release, p.release_ns);
				}
			}
		} else {
			held = config_.reorder > 0.0f && uniform01(rng_) < double(config_.reorder);
			if (!held) {

				bool moved = false;
				for (Pending &p : queue_) {
					if ((p.order & kHeldBit) != 0 && p.datagram.peer == p_datagram.peer) {
						p.order &= ~kHeldBit;
						p.release_ns = std::min(p.release_ns, release + 1);
						moved = true;
					}
				}
				if (moved) {
					std::stable_sort(queue_.begin(), queue_.end(), before);
				}
			}
		}
		uint64_t order = next_order_++;
		if (held) {
			order |= kHeldBit;
			release += ms_to_ns(kHoldTimeoutMs + span_ms);
			stats_.reordered++;
		}
		Pending pending{ release, order, copy + 1 < copies ? p_datagram : std::move(p_datagram) };
		insert(std::move(pending));
	}
}

bool NetSim::poll(uint64_t p_now_ns, Datagram &r_datagram) {
	if (queue_.empty() || queue_.front().release_ns > p_now_ns) {
		return false;
	}
	r_datagram = std::move(queue_.front().datagram);
	queue_.pop_front();
	stats_.delivered++;
	return true;
}

uint64_t NetSim::next_release_ns() const {
	return queue_.empty() ? 0 : std::max<uint64_t>(queue_.front().release_ns, 1);
}

size_t NetSim::pending() const {
	return queue_.size();
}

}
