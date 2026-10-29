#include "bf3/ai/knowledge/plug_ins.h"

namespace bf3::ai {

IPICover::IPICover(Information *p_information) : InformationPlugIn(p_information) {
	const AiObjectCover *const cover = p_information->ai_object()->cover();
	if (cover != nullptr) {
		outline_ = cover->object_outline();
	}
}

void IPICover::set_time() {
	set_time(information_->context().now());
}

void IPICover::set_time(sim::Tick) {
	const AiObjectCover *const cover = information_->ai_object()->cover();
	outline_ = cover != nullptr ? cover->object_outline() : std::vector<Vec2>{};
}

std::vector<Vec2> IPICover::local_outline() const {
	const AiObjectCover *const cover = information_->ai_object()->cover();
	return cover != nullptr ? cover->local_object_outline() : std::vector<Vec2>{};
}

float IPICover::volume() const {
	const AiObjectPhysical *const physical = information_->ai_object()->physical();
	return physical != nullptr ? physical->volume() : 0.0f;
}

float IPICover::cover_value() const {
	const AiObjectCover *const cover = information_->ai_object()->cover();
	return cover != nullptr ? cover->cover_value() : 0.0f;
}

float IPICover::height() const {
	const AiObjectCover *const cover = information_->ai_object()->cover();
	return cover != nullptr ? cover->height() : 0.0f;
}

float IPICover::transparency() const {
	const AiObjectCover *const cover = information_->ai_object()->cover();
	return cover != nullptr ? cover->transparency() : 0.0f;
}

}
