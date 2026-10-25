#include "bf3/core/ui/as2_clip_loader.h"

namespace bf3::core::ui {

void As2ClipLoader::load_clip(const As2Clip &p_target, std::string_view p_url) {
	target_ = p_target;
	res_name_ = swf_image_res_name(p_url);
	error_.clear();
	state_ = State::Loading;
	if (!target_.exists()) {
		error_ = "no clip at " + target_.path();
		state_ = State::Failed;
	} else if (res_name_.empty()) {
		error_ = "empty url";
		state_ = State::Failed;
	}
}

As2ClipLoader::State As2ClipLoader::poll() {
	if (state_ != State::Loading) {
		return state_;
	}
	SwfScene *scene = target_.scene();
	SwfImageSource *source = scene != nullptr ? scene->image_source() : nullptr;
	if (source == nullptr || !target_.exists()) {
		error_ = source == nullptr ? "the scene has no image source" : "no clip at " + target_.path();
		state_ = State::Failed;
		return state_;
	}
	const SwfImageInfo info = source->request(res_name_);
	if (info.status == SwfImageStatus::Loading) {
		return state_;
	}
	if (info.status == SwfImageStatus::Missing) {
		error_ = info.error;
		state_ = State::Failed;
		return state_;
	}
	const Status st = scene->load_image(target_.path(), res_name_, info.width, info.height);
	if (!st) {
		error_ = st.error().message;
		state_ = State::Failed;
		return state_;
	}
	state_ = State::Loaded;
	return state_;
}

void As2ClipLoader::unload_clip() {
	if (state_ == State::Loaded && target_.exists()) {
		const Status st = target_.scene()->unload_image(target_.path());
		if (!st) {
			error_ = st.error().message;
		}
	}
	state_ = State::Idle;
}

}
