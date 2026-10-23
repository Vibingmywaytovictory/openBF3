#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kUiMessageCompPartition = "ui/uicomponents/uimessagecomp";
inline constexpr uint32_t kHudKillWeaponFormatHash = 0x52ddee25u;
inline constexpr std::string_view kHudKillWeaponFormat = "[{0:~}]";
inline constexpr std::string_view kHudKillsIsNoSid = "ID_H_MSG_KILLS_ISNO";
inline constexpr std::string_view kHudScoreKillSid = "ID_SCORE_KILL";
inline constexpr std::string_view kHudScoreHeadshotSid = "ID_SCORE_HEADSHOT";
inline constexpr int32_t kHudRelationFriendly = 0;
inline constexpr int32_t kHudRelationEnemy = 1;
inline constexpr int32_t kHudRelationSquad = 2;

enum class HudMessageSource : uint8_t {
	None,
	Kill,
	LocalKill,
	Score,
	ScoreMinor,
	ScoreMajor,
	Game,
	Reward,
	ServerAdminYell,
	ScoreAggregation,
	Count,
};

struct HudMessageInfo {
	int32_t queue_size = 0;
	float normal_seconds = 0.0f;
	float short_seconds = 0.0f;
	std::string row_type;
};

struct HudMessageCompData {
	std::array<HudMessageInfo, size_t(HudMessageSource::Count)> info{};
	float score_aggregate_time = 0.0f;

	const HudMessageInfo &of(HudMessageSource p_source) const { return info[size_t(p_source)]; }
};

Result<HudMessageCompData> read_hud_message_comp(const EbxResolver &p_db);

struct HudMessageKey {
	int32_t data_key = 0;
	HudMessageSource source = HudMessageSource::None;
	std::string_view data_source;
};

inline constexpr std::array<HudMessageKey, 6> kHudMessageKeys{ {
		{ 224794678, HudMessageSource::Kill, "KillMessage" },
		{ 1675248891, HudMessageSource::LocalKill, "LocalKillMessage" },
		{ 1802462620, HudMessageSource::Score, "ScoreMessage" },
		{ -1226273021, HudMessageSource::Reward, "RewardMessage" },
		{ 710724930, HudMessageSource::ServerAdminYell, "ServerAdminYellMessage" },
		{ -29960869, HudMessageSource::ScoreAggregation, "ScoreAggregation" },
} };

HudMessageSource hud_message_source(int32_t p_data_key);

std::string hud_format_text(std::string_view p_format, std::span<const std::string> p_args);

int32_t hud_relation(const HudModel &p_model, uint8_t p_player);

class HudMessageComp {
public:
	HudMessageComp() = default;
	explicit HudMessageComp(HudMessageCompData p_data) :
			data_(std::move(p_data)) {}

	void update(const HudModel &p_model, const HudClientState &p_client, double p_now_s);

	const As2Value &list(HudMessageSource p_source) const { return queues_[size_t(p_source)].list; }
	const As2Value &score_aggregate() const { return aggregate_; }
	uint64_t messages_added(HudMessageSource p_source) const { return queues_[size_t(p_source)].added; }
	const HudMessageCompData &data() const { return data_; }

private:
	struct Row {
		double id = 0.0;
		double expires_s = 0.0;
		bool removing = false;
		uint64_t removed_at = 0;
		As2Value value;
	};
	struct Queue {
		std::deque<Row> rows;
		uint32_t last_serial = 0;
		As2Value list = As2Value::array();
		uint64_t added = 0;
	};

	void add(HudMessageSource p_source, As2Value p_value, double p_now_s);
	void expire(HudMessageSource p_source, double p_now_s);
	void publish(HudMessageSource p_source);
	As2Value kill_value(const HudModel &p_model, const HudClientState &p_client, const HudKill &p_kill, bool p_local) const;

	HudMessageCompData data_;
	std::array<Queue, size_t(HudMessageSource::Count)> queues_{};
	uint64_t updates_ = 0;
	double next_id_ = 0.0;
	uint32_t score_serial_ = 0;
	int32_t aggregate_sum_ = 0;
	double aggregate_until_s_ = 0.0;
	As2Value aggregate_;
};

}
