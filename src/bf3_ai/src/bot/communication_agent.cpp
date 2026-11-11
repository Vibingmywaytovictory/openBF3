#include "bf3/ai/bot/communication_agent.h"

namespace bf3::ai {

bool CallRadioVoices::settings(int p_type, RadioMessageSettings &r_settings) const {
	if (p_type != kRadioCallForMedic) {
		return false;
	}
	r_settings = RadioMessageSettings{ p_type, 1.0f, call_seconds_, call_seconds_ };
	return true;
}

CommunicationAgent::CommunicationAgent(const RadioVoices &p_voices, CommunicationHub &r_hub) :
		voices_(p_voices), hub_(r_hub) {
	for (size_t type = 0; type < kRadioMessageTypes; ++type) {
		RadioMessageSettings s;
		if (voices_.settings(int(type), s)) {
			receive_time_to_live_[type] = s.receive_time_to_live;
			max_send_period_[type] = s.max_send_period;
		}
	}
	reset();
}

void CommunicationAgent::reset() {
	blocked_until_.fill(0.0);
	for (std::list<ReceivedRadioMessage> &inbox : received_) {
		inbox.clear();
	}
	pending_.clear();
	queue_ = {};
	last_poll_time_ = 0.0;
}

void CommunicationAgent::update(double p_now, int p_player, int p_side) {
	clear_out_old_messages(p_now);
	if (p_now < last_poll_time_) {
		last_poll_time_ = p_now;
	}
	if (p_now - last_poll_time_ > 1.0) {
		last_poll_time_ = p_now;
		poll_for_radio_message(p_now, p_player, p_side);
	}
}

void CommunicationAgent::poll_for_radio_message(double p_now, int p_player, int p_side) {
	RadioMessage message;
	while (peek_radio_message(message) && !wait_with_sending_message(message, p_now)) {
		post_event(message, p_player, p_side);
		dequeue_radio_message(message);
	}
}

void CommunicationAgent::clear_out_old_messages(double p_now) {
	for (size_t type = 0; type < kRadioMessageTypes; ++type) {
		if (blocked_until_[type] < p_now) {
			blocked_until_[type] = 0.0;
		}
		std::list<ReceivedRadioMessage> &inbox = received_[type];
		auto it = inbox.begin();
		while (it != inbox.end() && p_now < it->time + double(receive_time_to_live_[type])) {
			++it;
		}
		inbox.erase(it, inbox.end());
	}
}

void CommunicationAgent::call_for_medic(double p_now, bool p_infantry) {
	if (p_infantry) {
		enqueue_radio_message(kRadioCallForMedic, kRadioChannelInfantry, kRadioCallPriority, 0.0f, -1, p_now);
	} else {
		enqueue_radio_message(kRadioCallForRepair, kRadioChannelVehicle, kRadioCallPriority, 0.0f, 0, p_now);
	}
}

void CommunicationAgent::receive_radio_message(
		const RadioMessageSettings &p_settings, int p_sender, bool p_unconfirmable, double p_now) {
	const int type = p_settings.type;
	if (type < 0 || size_t(type) >= kRadioMessageTypes) {
		return;
	}
	if (type == 0 || type == 1) {
		std::list<ReceivedRadioMessage> &other = received_[type == 1 ? 0 : 1];
		for (auto it = other.begin(); it != other.end(); ++it) {
			if (it->sender == p_sender) {
				other.erase(it, other.end());
				break;
			}
		}
	}
	std::list<ReceivedRadioMessage> &inbox = received_[size_t(type)];
	for (ReceivedRadioMessage &m : inbox) {
		if (m.sender == p_sender) {
			m.modifier = p_settings.modifier;
			m.confirmed = false;
			m.time = p_now;
			return;
		}
	}
	ReceivedRadioMessage info;
	info.modifier = p_settings.modifier;
	info.sender = p_sender;
	info.time = p_now;
	info.unconfirmable = p_unconfirmable;
	info.type = type;
	inbox.push_back(info);
}

void CommunicationAgent::enqueue_radio_message(
		int p_type, int p_channel, unsigned p_priority, float p_delay, int p_receiver, double p_now) {
	if (p_type < 0 || size_t(p_type) >= kRadioMessageTypes || !(blocked_until_[size_t(p_type)] <= p_now)) {
		return;
	}
	RadioMessage message;
	message.type = p_type;
	message.channel = p_channel;
	message.priority = int(p_priority);
	message.receiver = p_receiver;
	message.send_time = p_now + double(p_delay);
	message.delay = p_delay;
	if (pending_.emplace(p_type, message.send_time).second) {
		queue_.push(message);
	}
	blocked_until_[size_t(p_type)] = p_now + double(max_send_period_[size_t(p_type)]);
}

bool CommunicationAgent::peek_radio_message(RadioMessage &r_out) const {
	if (queue_.empty()) {
		return false;
	}
	r_out = queue_.top();
	return true;
}

bool CommunicationAgent::dequeue_radio_message(RadioMessage &r_out) {
	if (queue_.empty()) {
		return false;
	}
	r_out = queue_.top();
	pending_.erase(r_out.type);
	queue_.pop();
	return true;
}

bool CommunicationAgent::wait_with_sending_message(const RadioMessage &p_message, double p_now) const {
	if (p_message.send_time > p_now) {
		return true;
	}
	const std::list<ReceivedRadioMessage> &inbox = received_[size_t(p_message.type)];
	if (inbox.empty()) {
		return false;
	}
	const ReceivedRadioMessage &newest = inbox.front();
	return !(double(max_send_period_[size_t(newest.type)]) <= p_now - newest.time);
}

void CommunicationAgent::post_event(const RadioMessage &p_message, int p_player, int p_side) const {
	RadioMessageSettings voice;
	if (!voices_.settings(p_message.type, voice)) {
		return;
	}
	hub_.send_radio_message(p_player, p_side, p_message);
}

void CommunicationAgent::send_immediately(
		int p_type, int p_channel, unsigned p_priority, int p_receiver, double p_now, int p_player, int p_side) const {
	RadioMessage message;
	message.type = p_type;
	message.channel = p_channel;
	message.priority = int(p_priority);
	message.send_time = p_now;
	message.receiver = p_receiver;
	post_event(message, p_player, p_side);
}

bool CommunicationAgent::sent_message(int p_type, double p_now) const {
	return 0.0 > p_now - blocked_until_[size_t(p_type)];
}

float CommunicationAgent::modifier_for_message_type(int p_type) const {
	RadioMessageSettings voice;
	return voices_.settings(p_type, voice) ? voice.modifier : 1.0f;
}

const std::list<ReceivedRadioMessage> &CommunicationAgent::received_messages(int p_type) const {
	return received_[size_t(p_type)];
}

bool CommunicationAgent::received_message(ReceivedRadioMessage &r_out, int p_type, int p_sender) const {
	for (const ReceivedRadioMessage &m : received_[size_t(p_type)]) {
		if (m.sender == p_sender) {
			r_out = m;
			return true;
		}
	}
	return false;
}

bool CommunicationAgent::received_message_exists(int p_type, int p_sender) const {
	for (const ReceivedRadioMessage &m : received_[size_t(p_type)]) {
		if (p_sender == -1 || m.sender == p_sender) {
			return true;
		}
	}
	return false;
}

bool CommunicationAgent::general_confirm_received_message(int p_type) {
	bool confirmed = false;
	for (ReceivedRadioMessage &m : received_[size_t(p_type)]) {
		if (!m.confirmed && !m.unconfirmable) {
			m.confirmed = true;
			confirmed = true;
		}
	}
	return confirmed;
}

bool CommunicationAgent::confirm_received_message(int p_type, int p_sender) {
	bool confirmed = false;
	for (ReceivedRadioMessage &m : received_[size_t(p_type)]) {
		if (m.sender == p_sender && !m.confirmed && !m.unconfirmable) {
			m.confirmed = true;
			confirmed = true;
		}
	}
	return confirmed;
}

bool CommunicationAgent::confirm_received_message(const ReceivedRadioMessage &p_info) {
	for (ReceivedRadioMessage &m : received_[size_t(p_info.type)]) {
		if (m.type == p_info.type && m.sender == p_info.sender && m.time == p_info.time) {
			if (m.confirmed || m.unconfirmable) {
				return false;
			}
			m.confirmed = true;
			return true;
		}
	}
	return false;
}

}
