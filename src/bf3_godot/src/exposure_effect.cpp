#include "exposure_effect.h"

#include "godot_util.h"

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <array>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr uint32_t kReduceSide = 16;
constexpr uint32_t kApplySide = 8;
constexpr uint32_t kStateBytes = 16;

const char *kReduceShader = R"(#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D color_image;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Partials {
	vec4 sums[];
} partials;
layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 unused;
} params;
shared vec3 group_sum[256];
void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	uint i = gl_LocalInvocationIndex;
	group_sum[i] = p.x < params.size.x && p.y < params.size.y ? imageLoad(color_image, p).rgb : vec3(0.0);
	barrier();
	for (uint s = 128u; s > 0u; s >>= 1u) {
		if (i < s) {
			group_sum[i] += group_sum[i + s];
		}
		barrier();
	}
	if (i == 0u) {
		partials.sums[gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x] = vec4(group_sum[0], 0.0);
	}
}
)";

const char *kFinishHead = R"(#version 450
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) restrict readonly buffer Partials {
	vec4 sums[];
} partials;
layout(set = 0, binding = 1, std430) restrict buffer State {
	vec4 value;
} state;
layout(push_constant, std430) uniform Params {
	float middle_gray;
	float min_exposure;
	float max_exposure;
	float adjust_time;
	float dt;
	float reset;
	uint groups;
	float pixels;
} params;
shared vec3 total[256];
)";

const char *kFinishMain = R"(void main() {
	uint i = gl_LocalInvocationIndex;
	vec3 s = vec3(0.0);
	for (uint g = i; g < params.groups; g += 256u) {
		s += partials.sums[g].rgb;
	}
	total[i] = s;
	barrier();
	for (uint h = 128u; h > 0u; h >>= 1u) {
		if (i < h) {
			total[i] += total[i + h];
		}
		barrier();
	}
	if (i == 0u) {
		vec4 previous = state.value;
		vec4 next = exposure_step(previous, total[0] / params.pixels, params.middle_gray, params.min_exposure,
				params.max_exposure, params.adjust_time, params.dt, params.reset > 0.5);
		next.w = previous.w + 1.0;
		state.value = next;
	}
}
)";

const char *kApplyShader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(set = 0, binding = 1, std430) restrict readonly buffer State {
	vec4 value;
} state;
layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 unused;
} params;
void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= params.size.x || p.y >= params.size.y) {
		return;
	}
	vec4 c = imageLoad(color_image, p);
	imageStore(color_image, p, vec4(c.rgb * state.value.x, c.a));
}
)";

struct SizeParams {
	int32_t width = 0;
	int32_t height = 0;
	int32_t unused[2] = { 0, 0 };
};

struct FinishParams {
	float middle_gray = 0.0f;
	float min_exposure = 0.0f;
	float max_exposure = 0.0f;
	float adjust_time = 0.0f;
	float dt = 0.0f;
	float reset = 0.0f;
	uint32_t groups = 0;
	float pixels = 0.0f;
};

template <class T> PackedByteArray bytes_of(const T &p_value) {
	PackedByteArray out;
	out.resize(int64_t(sizeof(T)));
	std::memcpy(out.ptrw(), &p_value, sizeof(T));
	return out;
}

Ref<RDUniform> uniform(RenderingDevice::UniformType p_type, int32_t p_binding, const RID &p_id) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(p_type);
	u->set_binding(p_binding);
	u->add_id(p_id);
	return u;
}

RID uniform_set(const RID &p_shader, const RID &p_image, const RID &p_buffer) {
	TypedArray<RDUniform> uniforms;
	uniforms.push_back(uniform(RenderingDevice::UNIFORM_TYPE_IMAGE, 0, p_image));
	uniforms.push_back(uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_buffer));
	return UniformSetCacheRD::get_cache(p_shader, 0, uniforms);
}

RID buffer_set(const RID &p_shader, const RID &p_first, const RID &p_second) {
	TypedArray<RDUniform> uniforms;
	uniforms.push_back(uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_first));
	uniforms.push_back(uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_second));
	return UniformSetCacheRD::get_cache(p_shader, 0, uniforms);
}

}

Bf3ExposureEffect::Bf3ExposureEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
}

Bf3ExposureEffect::~Bf3ExposureEffect() = default;

void Bf3ExposureEffect::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		release();
	}
}

void Bf3ExposureEffect::set_exposure(const render::LookExposure &p_exposure) {
	std::lock_guard lock(mutex_);
	exposure_ = p_exposure;
}

void Bf3ExposureEffect::reset() {
	std::lock_guard lock(mutex_);
	reset_pending_ = true;
}

std::optional<ExposureReading> Bf3ExposureEffect::read() const {
	if (rd_ == nullptr || !state_.is_valid()) {
		return std::nullopt;
	}
	const PackedByteArray bytes = rd_->buffer_get_data(state_, 0, kStateBytes);
	if (bytes.size() < int64_t(kStateBytes)) {
		return std::nullopt;
	}
	float v[4];
	std::memcpy(v, bytes.ptr(), sizeof(v));
	if (!(v[2] > 0.5f)) {
		return std::nullopt;
	}
	return ExposureReading{ render::ExposureState{ v[0], v[1], true }, uint64_t(v[3]) };
}

std::string Bf3ExposureEffect::error() const {
	std::lock_guard lock(mutex_);
	return error_;
}

bool Bf3ExposureEffect::create(RenderingDevice &r_rd) {
	const auto compile = [&](const std::string &p_code, const char *p_name, RID &r_shader, RID &r_pipeline) {
		Ref<RDShaderSource> source;
		source.instantiate();
		source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, to_godot(p_code));
		const Ref<RDShaderSPIRV> spirv = r_rd.shader_compile_spirv_from_source(source);
		if (spirv.is_null() || !spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty()) {
			std::lock_guard lock(mutex_);
			error_ = std::string(p_name) + ": " +
					(spirv.is_null() ? std::string("no SPIR-V")
									 : to_std(spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE)));
			return false;
		}
		r_shader = r_rd.shader_create_from_spirv(spirv, to_godot(p_name));
		if (!r_shader.is_valid()) {
			std::lock_guard lock(mutex_);
			error_ = std::string(p_name) + ": shader_create_from_spirv failed";
			return false;
		}
		r_pipeline = r_rd.compute_pipeline_create(r_shader);
		return r_pipeline.is_valid();
	};
	rd_ = &r_rd;
	if (!compile(kReduceShader, "exposure reduce", reduce_shader_, reduce_pipeline_) ||
			!compile(std::string(kFinishHead) + render::exposure_step_glsl() + kFinishMain, "exposure finish",
					finish_shader_, finish_pipeline_) ||
			!compile(kApplyShader, "exposure apply", apply_shader_, apply_pipeline_)) {
		return false;
	}
	const std::array<float, 4> initial{ 1.0f, 0.0f, 0.0f, 0.0f };
	state_ = r_rd.storage_buffer_create(kStateBytes, bytes_of(initial));
	return state_.is_valid();
}

void Bf3ExposureEffect::release() {
	if (rd_ == nullptr) {
		return;
	}
	for (RID *r : { &reduce_pipeline_, &reduce_shader_, &finish_pipeline_, &finish_shader_, &apply_pipeline_,
				 &apply_shader_, &partials_, &state_ }) {
		if (r->is_valid()) {
			rd_->free_rid(*r);
			*r = RID();
		}
	}
	rd_ = nullptr;
}

void Bf3ExposureEffect::_render_callback(int32_t p_effect_callback_type, RenderData *p_render_data) {
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT || p_render_data == nullptr || failed_) {
		return;
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	Ref<RenderSceneBuffersRD> buffers = p_render_data->get_render_scene_buffers();
	if (rd == nullptr || buffers.is_null()) {
		return;
	}
	if (rd_ == nullptr && !create(*rd)) {
		failed_ = true;
		return;
	}
	const Vector2i size = buffers->get_internal_size();
	if (size.x <= 0 || size.y <= 0) {
		return;
	}
	const uint32_t groups_x = (uint32_t(size.x) + kReduceSide - 1) / kReduceSide;
	const uint32_t groups_y = (uint32_t(size.y) + kReduceSide - 1) / kReduceSide;
	const uint32_t groups = groups_x * groups_y;
	if (groups > partial_groups_) {
		if (partials_.is_valid()) {
			rd->free_rid(partials_);
		}
		partials_ = rd->storage_buffer_create(int64_t(groups) * 16);
		partial_groups_ = partials_.is_valid() ? groups : 0;
		if (!partials_.is_valid()) {
			return;
		}
	}

	const auto now = std::chrono::steady_clock::now();
	const float dt = last_frame_ ? std::chrono::duration<float>(now - *last_frame_).count() : 0.0f;
	last_frame_ = now;
	FinishParams finish;
	{
		std::lock_guard lock(mutex_);
		finish.middle_gray = exposure_.middle_gray;
		finish.min_exposure = exposure_.min_exposure;
		finish.max_exposure = exposure_.max_exposure;
		finish.adjust_time = exposure_.adjust_time;
		finish.reset = reset_pending_ ? 1.0f : 0.0f;
		reset_pending_ = false;
	}
	finish.dt = dt;
	finish.groups = groups;
	finish.pixels = float(size.x) * float(size.y);
	SizeParams sp;
	sp.width = size.x;
	sp.height = size.y;
	const PackedByteArray size_bytes = bytes_of(sp);
	const PackedByteArray finish_bytes = bytes_of(finish);

	for (uint32_t view = 0; view < buffers->get_view_count() && view < 1; ++view) {
		const RID color = buffers->get_color_layer(view);
		const int64_t list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, reduce_pipeline_);
		rd->compute_list_bind_uniform_set(list, uniform_set(reduce_shader_, color, partials_), 0);
		rd->compute_list_set_push_constant(list, size_bytes, size_bytes.size());
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, finish_pipeline_);
		rd->compute_list_bind_uniform_set(list, buffer_set(finish_shader_, partials_, state_), 0);
		rd->compute_list_set_push_constant(list, finish_bytes, finish_bytes.size());
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, apply_pipeline_);
		rd->compute_list_bind_uniform_set(list, uniform_set(apply_shader_, color, state_), 0);
		rd->compute_list_set_push_constant(list, size_bytes, size_bytes.size());
		rd->compute_list_dispatch(list, (uint32_t(size.x) + kApplySide - 1) / kApplySide,
				(uint32_t(size.y) + kApplySide - 1) / kApplySide, 1);
		rd->compute_list_end();
	}
}

}
