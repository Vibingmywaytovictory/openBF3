#pragma once

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/swf_image.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::core::ui {

class As2ClipLoader {
public:
	enum class State : uint8_t {
		Idle,
		Loading,
		Loaded,
		Failed,
	};

	void load_clip(const As2Clip &p_target, std::string_view p_url);
	State poll();
	void unload_clip();

	State state() const { return state_; }
	const As2Clip &target() const { return target_; }
	const std::string &res_name() const { return res_name_; }
	const std::string &error() const { return error_; }

private:
	As2Clip target_;
	std::string res_name_;
	std::string error_;
	State state_ = State::Idle;
};

}
