#pragma once

#include "bf3/render/look_exposure.h"

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace godot {
class RenderingDevice;
}

namespace bf3::godot_side {

struct ExposureReading {
	bf3::render::ExposureState state;
	uint64_t frames = 0;
};

class Bf3ExposureEffect : public godot::CompositorEffect {
	GDCLASS(Bf3ExposureEffect, godot::CompositorEffect)

public:
	Bf3ExposureEffect();
	~Bf3ExposureEffect() override;

	void set_exposure(const bf3::render::LookExposure &p_exposure);
	void reset();
	std::optional<ExposureReading> read() const;
	std::string error() const;
	void release();

	void _render_callback(int32_t p_effect_callback_type, godot::RenderData *p_render_data) override;

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

private:
	bool create(godot::RenderingDevice &r_rd);

	mutable std::mutex mutex_;
	bf3::render::LookExposure exposure_;
	bool reset_pending_ = true;

	godot::RenderingDevice *rd_ = nullptr;
	godot::RID reduce_shader_, reduce_pipeline_;
	godot::RID finish_shader_, finish_pipeline_;
	godot::RID apply_shader_, apply_pipeline_;
	godot::RID partials_, state_;
	uint32_t partial_groups_ = 0;
	bool failed_ = false;
	std::string error_;
	std::optional<std::chrono::steady_clock::time_point> last_frame_;
};

}
