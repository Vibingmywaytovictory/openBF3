#include "server_logic_sync.h"

#include "bf3/host/tdm_rules.h"

#include <utility>

namespace bf3::host {

void ServerLogicSync::ready(size_t p_slot) {
	const TdmRules *rules = match_.rules();
	const Seat &s = slots_[p_slot];
	if (rules == nullptr || !s.remote) {
		return;
	}
	std::optional<sim::logic::LogicGhost> ghost = rules->ghost();
	if (!ghost) {
		return;
	}
	if (!sent_) {
		sent_ = *ghost;
	}
	net::FullSync f;
	f.ghost = std::move(*ghost);
	outbox_.send(s.peer, net::kChannelBulk, net::encode_full_sync(f));
	ready_[p_slot] = 1;
	++stats_.full_syncs;
}

void ServerLogicSync::broadcast(uint8_t p_channel, const std::vector<uint8_t> &p_bytes) {
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Seat &s = slots_[i];
		if (ready_[i] != 0 && s.used && s.remote) {
			outbox_.send(s.peer, p_channel, p_bytes);
		}
	}
}

void ServerLogicSync::send(sim::Tick p_tick) {
	const TdmRules *rules = match_.rules();
	if (rules == nullptr) {
		return;
	}
	if (!rules->networked().empty()) {
		net::LogicEvents m;
		m.server_tick = p_tick;
		m.events.assign(rules->networked().begin(), rules->networked().end());
		size_t refused = 0;
		const std::vector<uint8_t> bytes = net::encode_logic_events(m, &refused);
		stats_.events_refused += refused;
		broadcast(net::kChannelReliable, bytes);
		++stats_.event_messages;
		stats_.events_sent += m.events.size() - refused;
	}
	std::optional<sim::logic::LogicGhost> ghost = rules->ghost();
	if (!ghost) {
		return;
	}
	if (!sent_) {
		sent_ = std::move(*ghost);
		return;
	}
	const bool whole = ghost->bool_count != sent_->bool_count || ghost->transforms != sent_->transforms ||
			ghost->sequence_start != sent_->sequence_start;
	if (whole) {
		net::FullSync f;
		f.ghost = *ghost;
		broadcast(net::kChannelReliable, net::encode_full_sync(f));
		++stats_.resyncs;
	} else if (std::vector<sim::logic::LogicGhost::BoolDelta> rows = ghost->bool_delta(*sent_); !rows.empty()) {
		net::LogicDelta d;
		d.tick = ghost->tick;
		d.rows = std::move(rows);
		broadcast(net::kChannelReliable, net::encode_logic_delta(d));
		++stats_.deltas;
	}
	sent_ = std::move(*ghost);
}

}
