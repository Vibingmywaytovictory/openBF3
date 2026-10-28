#include "ui_screen_records.h"

#include "bf3/core/ui/hud_screen.h"

#include <algorithm>

namespace bf3::godot_side {

double UiScreenRecord::number(std::string_view p_fact) const {
	const auto it = numbers.find(p_fact);
	return it != numbers.end() ? it->second : 0.0;
}

std::string UiScreenRecord::text(std::string_view p_fact) const {
	const auto it = texts.find(p_fact);
	return it != texts.end() ? it->second : std::string();
}

void add_screen_widget_facts(const bf3::core::ui::HudScreen &p_screen, UiScreenRecord &r_record) {
	using bf3::core::ui::As2Value;
	for (const std::unique_ptr<bf3::core::ui::HudWidgetState> &w : p_screen.widgets()) {
		if (!w->screen_widget || w->instance.empty()) {
			continue;
		}
		for (const auto &[key, value] : w->sent) {
			const std::string fact = w->instance + "." + key;
			if (value.kind == As2Value::Kind::String) {
				r_record.texts.insert_or_assign(fact, value.text);
			} else if (value.kind == As2Value::Kind::Number || value.kind == As2Value::Kind::Bool) {
				r_record.numbers.insert_or_assign(fact, value.to_number());
			}
		}
	}
}

UiScreenRecords::~UiScreenRecords() {
	for (UiScreenRecordSet *set : sets_) {
		set->sources_.erase(std::remove(set->sources_.begin(), set->sources_.end(), this), set->sources_.end());
	}
}

UiScreenRecordSet::~UiScreenRecordSet() {
	for (const UiScreenRecords *s : sources_) {
		s->sets_.erase(std::remove(s->sets_.begin(), s->sets_.end(), this), s->sets_.end());
	}
}

void UiScreenRecordSet::add(const UiScreenRecords *p_source) {
	if (p_source != nullptr && p_source != this && std::find(sources_.begin(), sources_.end(), p_source) == sources_.end()) {
		sources_.push_back(p_source);
		p_source->sets_.push_back(this);
	}
}

void UiScreenRecordSet::remove(const UiScreenRecords *p_source) {
	if (std::find(sources_.begin(), sources_.end(), p_source) == sources_.end()) {
		return;
	}
	sources_.erase(std::remove(sources_.begin(), sources_.end(), p_source), sources_.end());
	p_source->sets_.erase(std::remove(p_source->sets_.begin(), p_source->sets_.end(), this), p_source->sets_.end());
}

bool UiScreenRecordSet::reports(std::string_view p_partition) const {
	return std::any_of(sources_.begin(), sources_.end(), [&](const UiScreenRecords *p_s) { return p_s->reports(p_partition); });
}

std::optional<UiScreenRecord> UiScreenRecordSet::screen_record(std::string_view p_partition) const {
	for (const UiScreenRecords *s : sources_) {
		if (s->reports(p_partition)) {
			return s->screen_record(p_partition);
		}
	}
	return std::nullopt;
}
}
