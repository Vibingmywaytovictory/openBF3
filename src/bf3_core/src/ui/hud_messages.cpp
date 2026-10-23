#include "bf3/core/ui/hud_messages.h"

#include "ui_ebx.h"

#include "bf3/core/ui/text_db.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf3::core::ui {

namespace {

struct InfoField {
	HudMessageSource source;
	std::string_view field;
};

constexpr std::array<InfoField, 8> kInfoFields{ {
		{ HudMessageSource::Kill, "KillMessageInfo" },
		{ HudMessageSource::LocalKill, "LocalKillMessageInfo" },
		{ HudMessageSource::Score, "ScoreMessageInfo" },
		{ HudMessageSource::ScoreMinor, "ScoreMinorMessageInfo" },
		{ HudMessageSource::ScoreMajor, "ScoreMajorMessageInfo" },
		{ HudMessageSource::Game, "GameMessageInfo" },
		{ HudMessageSource::Reward, "RewardMessageInfo" },
		{ HudMessageSource::ServerAdminYell, "ServerAdminYellMessageInfo" },
} };

std::string player_name(const HudModel &p_model, uint8_t p_player) {
	if (p_player >= p_model.players.size()) {
		return {};
	}
	const HudPlayerName &n = p_model.players[p_player].name;
	return std::string(n.data(), strnlen(n.data(), n.size()));
}

std::string localised(const HudClientState &p_client, uint32_t p_hash) {
	return p_hash != 0 && p_client.localise_hash ? p_client.localise_hash(p_hash) : std::string();
}

}

Result<HudMessageCompData> read_hud_message_comp(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kUiMessageCompPartition);
	if (part == nullptr) {
		return make_error("hud messages: no partition " + std::string(kUiMessageCompPartition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("UIMessageCompData")) {
		return make_error("hud messages: " + std::string(kUiMessageCompPartition) + " is a " + std::string(root.type_name()));
	}
	HudMessageCompData out;
	for (const InfoField &f : kInfoFields) {
		const std::optional<EbxStruct> s = detail::sub(root.data(), f.field);
		if (!s) {
			return make_error("hud messages: UIMessageCompData has no " + std::string(f.field));
		}
		HudMessageInfo &i = out.info[size_t(f.source)];
		const std::optional<EbxValue> q = s->get("MessageQueueSize");
		i.queue_size = q ? q->as_i32().value_or(0) : 0;
		i.normal_seconds = detail::f32_or(*s, "NormalMessageTime", 0.0f);
		i.short_seconds = detail::f32_or(*s, "ShortMessageTime", 0.0f);
		const std::optional<EbxValue> t = s->get("RowTypeName");
		i.row_type = std::string(t ? t->as_str().value_or(std::string_view()) : std::string_view());
		if (i.row_type.empty() || i.queue_size <= 0) {
			return make_error("hud messages: " + std::string(f.field) + " has no row type or queue");
		}
	}
	out.info[size_t(HudMessageSource::ScoreAggregation)].row_type = "ScoreAggregation";
	out.score_aggregate_time = detail::f32_or(root.data(), "ScoreAggregateTime", 0.0f);
	return out;
}

HudMessageSource hud_message_source(int32_t p_data_key) {
	for (const HudMessageKey &k : kHudMessageKeys) {
		if (k.data_key == p_data_key) {
			return k.source;
		}
	}
	return HudMessageSource::None;
}

std::string hud_format_text(std::string_view p_format, std::span<const std::string> p_args) {
	std::string out;
	size_t i = 0;
	while (i < p_format.size()) {
		const char c = p_format[i];
		if (c == '{') {
			const size_t close = p_format.find('}', i);
			if (close != std::string_view::npos && close > i + 1 && p_format[i + 1] >= '0' && p_format[i + 1] <= '9') {
				size_t at = i + 1;
				size_t index = 0;
				while (at < close && p_format[at] >= '0' && p_format[at] <= '9') {
					index = index * 10 + size_t(p_format[at] - '0');
					++at;
				}
				if (at == close || p_format[at] == ':') {
					if (index < p_args.size()) {
						out += p_args[index];
					}
					i = close + 1;
					continue;
				}
			}
		}
		out += c;
		++i;
	}
	return out;
}

int32_t hud_relation(const HudModel &p_model, uint8_t p_player) {
	if (p_player >= p_model.players.size()) {
		return kHudRelationEnemy;
	}
	if (p_player == p_model.local_player) {
		return kHudRelationSquad;
	}
	const HudPlayer &p = p_model.players[p_player];
	if (p_model.local_player < p_model.players.size()) {
		const HudPlayer &me = p_model.players[p_model.local_player];
		if (me.squad != 0 && p.squad == me.squad && p.team == me.team) {
			return kHudRelationSquad;
		}
	}
	return p.team != 0 && p.team == p_model.local_team ? kHudRelationFriendly : kHudRelationEnemy;
}

As2Value HudMessageComp::kill_value(const HudModel &p_model, const HudClientState &p_client, const HudKill &p_kill, bool p_local) const {
	const bool no_killer = p_kill.suicide || p_kill.killer >= p_model.players.size();
	As2Value v = As2Value::object();
	v.set("KillerColor", As2Value::of(no_killer ? kHudRelationEnemy : hud_relation(p_model, p_kill.killer)));
	v.set("VictimColor", As2Value::of(hud_relation(p_model, p_kill.victim)));
	if (no_killer) {
		v.set("Text", As2Value::of(std::string(kHudKillsIsNoSid)));
	} else {
		const std::string weapon = localised(p_client, p_kill.weapon_name_sid_hash);
		const std::string format = localised(p_client, kHudKillWeaponFormatHash);
		v.set("Text", As2Value::of(weapon.empty() ? std::string() : hud_format_text(format, std::span<const std::string>(&weapon, 1))));
		v.set("KillerName", As2Value::of(player_name(p_model, p_kill.killer)));
	}
	v.set("VictimName", As2Value::of(player_name(p_model, p_kill.victim)));
	v.set("HeadShot", As2Value::of(p_kill.headshot));
	if (p_local) {
		v.set("IsLocalPlayer", As2Value::of(p_kill.victim == p_model.local_player));
	} else {
		v.set("TeamKill", As2Value::of(p_kill.team_kill && !no_killer && p_kill.killer == p_model.local_player));
	}
	return v;
}

void HudMessageComp::add(HudMessageSource p_source, As2Value p_value, double p_now_s) {
	const HudMessageInfo &info = data_.of(p_source);
	Queue &q = queues_[size_t(p_source)];
	size_t alive = 0;
	for (const Row &r : q.rows) {
		alive += r.removing ? 0 : 1;
	}
	if (info.queue_size > 0 && alive >= size_t(info.queue_size)) {
		for (Row &r : q.rows) {
			if (!r.removing) {
				r.expires_s = std::min(r.expires_s, p_now_s + double(std::max(info.short_seconds, 0.0f)));
				break;
			}
		}
	}
	Row row;
	row.id = ++next_id_;
	row.expires_s = info.normal_seconds < 0.0f ? INFINITY : p_now_s + double(info.normal_seconds);
	row.value = As2Value::object();
	row.value.set("Id", As2Value::of(row.id));
	row.value.set("Type", As2Value::of(info.row_type));
	row.value.set("Value", std::move(p_value));
	q.rows.push_back(std::move(row));
	q.added++;
}

void HudMessageComp::expire(HudMessageSource p_source, double p_now_s) {
	Queue &q = queues_[size_t(p_source)];
	q.rows.erase(std::remove_if(q.rows.begin(), q.rows.end(), [&](const Row &r) { return r.removing && r.removed_at < updates_; }), q.rows.end());
	for (Row &r : q.rows) {
		if (!r.removing && r.expires_s <= p_now_s) {
			r.removing = true;
			r.removed_at = updates_;
		}
	}
}

void HudMessageComp::publish(HudMessageSource p_source) {
	Queue &q = queues_[size_t(p_source)];
	As2Value list = As2Value::array();
	for (const Row &r : q.rows) {
		As2Value e = r.value;
		e.set("RemoveFlag", As2Value::of(r.removing ? 1 : 0));
		list.push(std::move(e));
	}
	q.list = std::move(list);
}

void HudMessageComp::update(const HudModel &p_model, const HudClientState &p_client, double p_now_s) {
	++updates_;
	const double now = std::isfinite(p_now_s) ? p_now_s : 0.0;
	const auto feed = [&](HudMessageSource p_source, const HudKillFeed &p_feed, bool p_local) {
		Queue &q = queues_[size_t(p_source)];
		if (p_feed.newest_serial < q.last_serial) {
			q.last_serial = 0;
		}
		const uint32_t first = std::max(q.last_serial + 1, p_feed.newest_serial >= p_feed.lines.size() ? p_feed.newest_serial - uint32_t(p_feed.lines.size()) + 1 : 1u);
		for (uint32_t s = first; s <= p_feed.newest_serial && s != 0; ++s) {
			const HudKill &k = p_feed.lines[s % p_feed.lines.size()];
			if (k.serial == s) {
				add(p_source, kill_value(p_model, p_client, k, p_local), now);
			}
		}
		q.last_serial = p_feed.newest_serial;
	};
	constexpr std::array<HudMessageSource, 3> kQueues{ HudMessageSource::Kill, HudMessageSource::LocalKill, HudMessageSource::Score };
	for (const HudMessageSource src : kQueues) {
		expire(src, now);
	}
	feed(HudMessageSource::Kill, p_model.messages.kill_message, false);
	feed(HudMessageSource::LocalKill, p_model.messages.local_kill_message, true);
	const HudScoreFeed &scores = p_model.messages.score_message;
	if (scores.newest_serial < score_serial_) {
		score_serial_ = 0;
	}
	const uint32_t first = std::max(score_serial_ + 1, scores.newest_serial >= scores.events.size() ? scores.newest_serial - uint32_t(scores.events.size()) + 1 : 1u);
	for (uint32_t s = first; s <= scores.newest_serial && s != 0; ++s) {
		const HudScoreEvent &e = scores.events[s % scores.events.size()];
		if (e.serial != s) {
			continue;
		}
		As2Value v = As2Value::object();
		v.set("Text", As2Value::of(localised(p_client, e.sid_hash)));
		v.set("Score", As2Value::of(e.points));
		add(HudMessageSource::Score, std::move(v), now);
		if (now >= aggregate_until_s_) {
			aggregate_sum_ = 0;
		}
		aggregate_sum_ += e.points;
		aggregate_until_s_ = now + double(data_.score_aggregate_time);
		aggregate_ = As2Value::of(aggregate_sum_);
	}
	score_serial_ = scores.newest_serial;
	if (aggregate_.defined() && aggregate_sum_ != 0 && now >= aggregate_until_s_) {
		aggregate_sum_ = 0;
		aggregate_ = As2Value::of(0);
	}
	for (const HudMessageSource src : kQueues) {
		expire(src, now);
		publish(src);
	}
}

}
