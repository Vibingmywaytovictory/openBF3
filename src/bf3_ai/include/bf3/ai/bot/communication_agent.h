#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <queue>
#include <vector>

namespace bf3::ai {

inline constexpr size_t kRadioMessageTypes = 18;
inline constexpr int kRadioCallForMedic = 3;
inline constexpr int kRadioCallForRepair = 4;
inline constexpr int kRadioChannelInfantry = 0;
inline constexpr int kRadioChannelVehicle = 6;
inline constexpr unsigned kRadioCallPriority = 5;

struct RadioMessageSettings {
	int type = 0;
	float modifier = 1.0f;
	float receive_time_to_live = 0.0f;
	float max_send_period = 0.0f;
};

class RadioVoices {
public:
	virtual ~RadioVoices() = default;
	virtual bool settings(int p_type, RadioMessageSettings &r_settings) const = 0;
};

class CallRadioVoices final : public RadioVoices {
public:
	explicit CallRadioVoices(float p_call_seconds) : call_seconds_(p_call_seconds) {}
	bool settings(int p_type, RadioMessageSettings &r_settings) const override;

private:
	float call_seconds_ = 0.0f;
};

struct RadioMessage {
	int type = 0;
	int channel = 0;
	int priority = 0;
	int receiver = -1;
	double send_time = 0.0;
	float delay = 0.0f;

	bool operator<(const RadioMessage &p_other) const { return priority < p_other.priority; }
};

class CommunicationHub {
public:
	virtual ~CommunicationHub() = default;
	virtual void send_radio_message(int p_sender, int p_side, const RadioMessage &p_message) = 0;
};

struct ReceivedRadioMessage {
	float modifier = 1.0f;
	int sender = -1;
	double time = 0.0;
	bool confirmed = false;
	bool unconfirmable = false;
	int type = 0;
};

class CommunicationAgent {
public:
	CommunicationAgent(const RadioVoices &p_voices, CommunicationHub &r_hub);

	void reset();
	void update(double p_now, int p_player, int p_side);
	void poll_for_radio_message(double p_now, int p_player, int p_side);
	void clear_out_old_messages(double p_now);
	void call_for_medic(double p_now, bool p_infantry);
	void receive_radio_message(
			const RadioMessageSettings &p_settings, int p_sender, bool p_unconfirmable, double p_now);
	void enqueue_radio_message(
			int p_type, int p_channel, unsigned p_priority, float p_delay, int p_receiver, double p_now);
	bool peek_radio_message(RadioMessage &r_out) const;
	bool dequeue_radio_message(RadioMessage &r_out);
	bool wait_with_sending_message(const RadioMessage &p_message, double p_now) const;
	void post_event(const RadioMessage &p_message, int p_player, int p_side) const;
	void send_immediately(int p_type, int p_channel, unsigned p_priority, int p_receiver, double p_now, int p_player,
			int p_side) const;

	bool sent_message(int p_type, double p_now) const;
	float modifier_for_message_type(int p_type) const;
	const std::list<ReceivedRadioMessage> &received_messages(int p_type) const;
	bool received_message(ReceivedRadioMessage &r_out, int p_type, int p_sender) const;
	bool received_message_exists(int p_type, int p_sender) const;
	bool general_confirm_received_message(int p_type);
	bool confirm_received_message(int p_type, int p_sender);
	bool confirm_received_message(const ReceivedRadioMessage &p_info);
	size_t queued() const { return queue_.size(); }

private:
	const RadioVoices &voices_;
	CommunicationHub &hub_;
	std::array<std::list<ReceivedRadioMessage>, kRadioMessageTypes> received_{};
	std::map<int, double> pending_;
	std::priority_queue<RadioMessage> queue_;
	std::array<double, kRadioMessageTypes> blocked_until_{};
	double last_poll_time_ = 0.0;
	std::array<float, kRadioMessageTypes> receive_time_to_live_{};
	std::array<float, kRadioMessageTypes> max_send_period_{};
};

}
