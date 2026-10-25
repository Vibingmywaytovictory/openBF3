#include "ui_screen_records.h"

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

std::optional<UiLookupRecord> UiScreenRecordSet::lookup_record() const {
	std::optional<UiLookupRecord> out;
	for (const UiScreenRecords *s : sources_) {
		const std::optional<UiLookupRecord> r = s->lookup_record();
		if (!r) {
			continue;
		}
		if (!out) {
			out = UiLookupRecord{};
		}
		out->model_serial = std::max(out->model_serial, r->model_serial);
		out->draws += r->draws;
		out->missing_fonts += r->missing_fonts;
		out->missing_icons += r->missing_icons;
		out->missing_strings += r->missing_strings;
	}
	return out;
}

}
