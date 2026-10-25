#pragma once

#include "native_hud.h"
#include "ui_screen_host.h"

#include "bf3/core/ui/swf_scene.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/spawn.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {
class SwfFontSet;
}

namespace bf3::sim {
class CollisionWorld;
struct MotorParams;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3NativeHud;
class NativeHudMomentRun;

inline constexpr double kNativeHudMomentStep = 1.0 / 30.0;

struct NativeHudMomentPlace {
	float x = 0.0f;
	float z = 0.0f;
	float view_yaw = 0.0f;
};

class NativeHudMomentGroup {
public:
	virtual ~NativeHudMomentGroup() = default;
	virtual size_t moment_count() const = 0;
	virtual std::string_view moment_id(size_t p_moment) const = 0;
	virtual std::optional<NativeHudMomentPlace> place(NativeHudMomentRun &r_run, Bf3NativeHud &r_hud, size_t p_moment) = 0;
	virtual void settle(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment) = 0;
	virtual void check(NativeHudMomentRun &r_run, Bf3LevelSession &r_session, Bf3NativeHud &r_hud, size_t p_moment, const godot::Ref<godot::Image> &p_layer) = 0;
};

using NativeHudMomentGroups = std::vector<std::unique_ptr<NativeHudMomentGroup>>;

void register_native_hud_moment_files(NativeHudMomentGroups &r_groups);

std::string native_hud_xyz(const bf3::sim::Vec3 &p_v);
bool native_hud_at_feet(const bf3::sim::Vec3 &p_at, const bf3::sim::Vec3 &p_feet);
const char *native_hud_text_of(const UiScreenHost &p_hud, const char *p_path);
const bf3::core::ui::SwfSceneNode *native_hud_node_of(const UiScreenHost &p_hud, const char *p_path);
const bf3::core::ui::SwfScene *native_hud_scene_of(const UiScreenHost &p_hud, const char *p_path);
float native_hud_layer_scale(const UiScreenHost &p_hud, const char *p_path);

struct NativeHudMessageRow {
	float y = 0.0f;
	uint32_t frame = 0;
	float alpha = 0.0f;
	std::string text;
};

std::vector<NativeHudMessageRow> native_hud_message_rows(const UiScreenHost &p_hud, const char *p_widget, const char *p_text);

class NativeHudNameFit {
public:
	std::string open(const char *p_movie, std::initializer_list<const char *> p_linkages);
	float chain_scale(std::string_view p_field) const;
	float width(std::string_view p_field, std::string_view p_text);
	std::optional<uint32_t> label(std::string_view p_linkage, std::string_view p_label) const;
	uint32_t frames(std::string_view p_linkage) const;
	std::optional<float> alpha_at(std::string_view p_linkage, std::string_view p_child, uint32_t p_frame);
	std::string fit(std::string_view p_field, float p_units);

private:
	std::shared_ptr<bf3::core::ui::SwfScene> scene_;
	const bf3::core::ui::SwfFontSet *fonts_ = nullptr;
	std::string prefix_;
};

NativeHudMinimapPose native_hud_moment_pose(const bf3::sim::Vec3 &p_own, float p_view_yaw);
std::optional<bf3::sim::Vec3> native_hud_moment_feet(const bf3::sim::CollisionWorld &p_world, const bf3::sim::MotorParams &p_motor, std::span<const bf3::sim::SpawnPoint> p_spawns,
		float p_x, float p_z);
std::optional<bf3::sim::Vec3> native_hud_moment_floor_near(const bf3::sim::CollisionWorld &p_world, const bf3::sim::MotorParams &p_motor,
		std::span<const bf3::sim::SpawnPoint> p_spawns, float p_x, float p_z, float p_radius);

class NativeHudMomentRun {
public:
	NativeHudMomentRun(std::string p_out_dir, std::string_view p_ids);
	~NativeHudMomentRun();

	bool step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, double p_delta);
	int exit_code() const { return failures_ == 0 && captured_ > 0 ? 0 : 1; }

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	const bf3::sim::Vec3 &feet() const { return feet_; }

private:
	enum class Phase : uint8_t { WaitAlive, Place, Arrive, Settle, Render, Capture, Coverage, Done };
	struct Selected {
		NativeHudMomentGroup *group = nullptr;
		size_t moment = 0;
		std::string id;
	};
	void hold(Bf3LevelSession &r_session);
	void check_host(Bf3NativeHud &r_hud);
	void check_stack(const Bf3LevelSession &p_session, const Bf3NativeHud &p_hud);

	std::string out_dir_;
	NativeHudMomentGroups groups_;
	std::vector<Selected> moments_;
	Phase phase_ = Phase::WaitAlive;
	size_t index_ = 0;
	int32_t frames_ = 0;
	double waited_ = 0.0;
	double arriving_ = 0.0;
	bf3::sim::Vec3 feet_{};
	float view_yaw_ = 0.0f;
	uint32_t request_id_ = 0;
	uint32_t captured_ = 0;
	uint32_t failures_ = 0;
	godot::Ref<godot::Image> composite_;
	std::array<godot::Ref<godot::Image>, kNativeHudViews> colour_;
	std::map<std::string, godot::PackedByteArray, std::less<>> first_capture_;
};

}
